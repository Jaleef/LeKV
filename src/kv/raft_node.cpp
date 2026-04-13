#include "raft_node.h"

#include <complex>
#include <iostream>
#include <bits/this_thread_sleep.h>

#include "rpc/text_protocol.h"

RaftNode::RaftNode(uint64_t node_id, uint16_t port, const std::vector<PeerInfo>& peers)
    : node_id_(node_id), port_(port), peers_(peers), rpc_server_(port),
    quorum_size_(peers.size() / 2 + 1) {

    // 初始化日志 (占位符，index 从 1 开始)
    log_.push_back({0, 0, ""});
    if (IsLeader()) {
        std::cout << "[Node " << node_id_ << "] Starting as LEADER (fixed " << LEADER_PORT << ")" << std::endl;
        std::cout << "[Leader] Quorum size: " << quorum_size_ << " (need " << quorum_size_ << " acks to commit)" << std::endl;

        // 初始化 Leader 状态: next_index 为 last + 1, match_index 为 0
        uint64_t last_idx = GetLastLogIndex();
        for (const auto& peer : peers) {
            if (peer.port != LEADER_PORT) {
                next_index_[peer.id] = last_idx + 1;
                match_index_[peer.id] = 0;

                // 建立到 Follower 的长连接
                auto client = std::make_unique<RpcClient>();
                if (client->Connect(peer.ip, peer.port)) {
                    peer_clients_[peer.id] = std::move(client);
                    std::cout << "[Leader] Connected to Follower " << peer.port << std::endl;
                } else {
                    std::cout << "[Leader] Warning: Failed to connect to " << peer.port << std::endl;
                }
            }
        }
    } else {
        std::cout << "[Node " << node_id_ << "] Starting as FOLLOWER (accepting from " << LEADER_PORT << ")" << std::endl;
    }
}

RaftNode::~RaftNode() {
    Stop();
}

void RaftNode::Run() {
    // 绑定命令处理器
    auto handler = [this](const Command& cmd) -> std::string {
        return this->HandleCommand(cmd);
    };

    if (!rpc_server_.Start(handler)) {
        std::cerr << "Failed to start server on port " << port_ << std::endl;
        return;
    }

    running_ = true;

    // 启动 ApplyLoop (所有节点)
    apply_thread_ = std::thread(&RaftNode::ApplyLoop, this);

    // Leader 启动复制循环
    if (IsLeader()) {
        leader_thread_ = std::thread(&RaftNode::LeaderLoop, this);
    }

    std::cout << "Node: " << node_id_ << " running at port " << port_
        << " [Term: " << current_term_ << "]" << std::endl;

    // 阻塞等待
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return !running_; });

}

void RaftNode::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();

    rpc_server_.Stop();

    if (leader_thread_.joinable()) { leader_thread_.join(); }
    if (apply_thread_.joinable()) { apply_thread_.join(); }

    for (auto& [id, client] : peer_clients_) {
        client->Close();
    }
}

// ========== 所有节点: 应用到状态机 ==========
void RaftNode::ApplyLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return !running_ || last_applied_ < commit_index_;
        });

        while (last_applied_ < commit_index_) {
            last_applied_++;
            const auto& entry = log_[last_applied_];
            lock.unlock();

            // 应用到存储引擎
            ApplyLogEntry(entry);

            lock.lock();
        }
    }
}

// ========== Leader 核心循环: 心跳 + 日志复制 ==========
void RaftNode::LeaderLoop() {
    while (running_) {
        std::vector<std::thread> send_threads;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 给每个 Follower 发送日志 (或心跳)
            for (const auto& [peer_id, client] : peer_clients_) {
                if (!client->IsConnected()) { continue; }

                // 简单策略: 每次循环都尝试同步该 Follower 缺失的日志
                send_threads.emplace_back([this, peer_id]() {
                    this->ReplicateLog(peer_id, false);
                });
            }
        }

        // 等待本轮发送完毕 (简化处理，实际可以异步)
        for (auto& t : send_threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        // 检查是否可以提交新日志 (半数以上确认)
        AdvanceCommitIndex();

        // 50ms 心跳间隔
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ========== Leader 向单个 Follower 复制日志
void RaftNode::ReplicateLog(uint64_t peer_id, bool heartbeat) {
    auto it = peer_clients_.find(peer_id);
    if (it == peer_clients_.end() || !it->second->IsConnected()) return;

    uint64_t next_idx, match_idx, prev_idx, prev_term;
    std::vector<LogEntry> entries_to_send;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_idx = next_index_[peer_id];
        match_idx = match_index_[peer_id];
        uint64_t last_idx = GetLastLogIndex();

        // 如果没有新日志且不是强制心跳, 跳过 (LeaderLoop 里定期发送)
        if (next_idx > last_idx && !heartbeat) return;

        // 计算 prev_index 和 prev_term
        prev_idx = next_idx - 1;
        prev_term = (prev_idx > 0 && prev_idx < log_.size()) ? log_[prev_idx].term : 0;

        // 收集要发送的条目(从 next_idx 到 last_idx, 限制批量大小)
        for (uint64_t i = next_idx ; i <= last_idx && entries_to_send.size() < 100 ; ++i) {
            entries_to_send.push_back(log_[i]);
        }
    }

    // 构造 APPEND 命令: APPEND <term> <leader_id> <prev_id> <prev_term> <commit_idx> [<entry>...]
    // 条目格式: term,index,command
    std::string cmd = "APPEND " + std::to_string(current_term_) + " "
        + std::to_string(node_id_) + " "
        + std::to_string(prev_idx) + " "
        + std::to_string(prev_term) + " "
        + std::to_string(commit_index_);

    for (const auto& entry : entries_to_send) {
        cmd += " " + entry.Serialize();
    }

    // 同步发送并等待响应
    std::string resp = it->second->Send(cmd, 1000);     // 1s 超时

    if (resp.empty()) {
        // 超时或失败, 不更新进度, 下次重试
        return;
    }

    // 打印 APPEND 响应，便于调试
    PrintRole();
    std::cout << "Received APPEND_RESP from " << peer_id << ": " << resp << std::endl;

    // 解析响应: APPEND_RESP <term> <success>
    auto parts = TextProtocol::SplitArgs(resp);
    if (parts.size() >= 3 && parts[0] == "APPEND_RESP") {
        bool success = (parts[2] == "true" || parts[2] == "+OK");

        std::lock_guard<std::mutex> lock(mutex_);
        if (success) {
            // 成功, 更新 match_index 和 next_index
            if (!entries_to_send.empty()) {
                uint64_t new_match = entries_to_send.back().index;
                match_index_[peer_id] = std::max(match_index_[peer_id], new_match);
                next_index_[peer_id] = new_match + 1;
            }
        } else {
            // 失败 (日志不匹配): Leader 递减 next_index 重试
            if (next_index_[peer_id] > 1) {
                next_index_[peer_id]--;
            }
        }
    }
}

// ========== Leader 检查半数以上确认, 推进 CommitIndex ==========
void RaftNode::AdvanceCommitIndex() {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t last_idx = GetLastLogIndex();

    // 从当前 commit_index + 1 开始检查，找到最大的可提交 N
    for (uint64_t n = commit_index_ + 1 ; n <= last_idx ; ++n) {
        // 只提交当前 Term 的日志
        if (log_[n].term != current_term_) continue;

        // 统计有多少节点 (包括 Leader 自己) 已经复制了日志 N
        int ack_count = 1;      // Leader 自己
        for (const auto& [peer_id, match_idx] : match_index_) {
            if (match_idx >= n) { ack_count++; }
        }

        // 如果半数以上 (quorum), 则可以提交
        if (ack_count >= static_cast<int>(quorum_size_)) {
            if (n > commit_index_) {
                commit_index_ = n;
                cv_.notify_all();

                PrintRole();
                std::cout << "CommitIndex advanced to " << commit_index_
                    << " (acked by " << ack_count << "/" << peers_.size() << ")" << std::endl;
            }
        } else {
            break;      // 不满足多数派，停止检查更大的N
        }
    }
}


void RaftNode::ApplyLogEntry(const LogEntry& entry) {
    // 解析 command (格式: "PUT k v" 或 "DELETE k"
    std::istringstream iss(entry.command);
    std::string cmd_type, key, value;
    iss >> cmd_type >> key >> value;

    if (cmd_type == "PUT") {
        storage_.Put(key, value);

        PrintRole();
        std::cout << "Applied[" << entry.index << "]: PUT " << key << " = " << value << std::endl;

    } else if (cmd_type == "DELETE") {
        storage_.Delete(key);

        PrintRole();
        std::cout << "Applied[" << entry.index << "]: DELETE " << key << std::endl;

    }
}

// ========== 命令处理 ==========
std::string RaftNode::HandleCommand(const Command& cmd) {
    
    // 简单日志输出收到的命令，便于调试，后续需要删除
    PrintRole();
    std::cout << "Received command: " << cmd.name;
    for (const std::string& arg : cmd.args) {
        std::cout << " " << arg;
    }
    std::cout << std::endl;

    if (cmd.name == "PUT" || cmd.name == "DELETE") {
        return HandleClientPut(cmd.args.size() > 0 ?  cmd.args[0] : "",
                                cmd.args.size() > 1 ? cmd.args[1] : "");
    } else if (cmd.name == "GET") {
        if (cmd.args.empty()) {
            return TextProtocol::Err("ARGS", "GET <key>");
        }
        return HandleClientGet(cmd.args[0]);
    } else if (cmd.name == "APPEND") {
        return HandleAppendEntries(cmd);
    }
    return TextProtocol::Err("UNKNOWN", cmd.name);
}

std::string RaftNode::HandleClientPut(const std::string& key, const std::string& value) {
    if (!IsLeader()) {
        return TextProtocol::Err("NOT_LEADER", GetLeaderAddr());
    }

    if (key.empty()) {
        return TextProtocol::Err("ARGS", "Empty key");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 构造日志条目
    LogEntry entry;
    entry.term = current_term_;
    entry.index = GetLastLogIndex() + 1;
    entry.command = value.empty() ? ("DELETE " + key) : ("PUT " + key + " " + value);

    // 追加到 Leader 日志 (但未提交，需等待复制到多数派)
    log_.push_back(entry);

    PrintRole();
    std::cout << "New log[" << entry.index << "] at Term "
        << entry.term << ": " << entry.command << std::endl;

    // 注意：这里立即返回 OK，但数据实际还未提交（Raft 标准做法是异步等待或客户端轮询）
    // 生产环境应阻塞等待 commit_index >= entry.index 或返回 proposal id 供查询
    return TextProtocol::Ok(std::to_string(entry.index));
}

std::string RaftNode::HandleClientGet(const std::string& key) {
    // 直接从状态机读取
    // ApplyLoop 保证只有 commit_index 之前的才会应用
    auto val = storage_.Get(key);
    if (val.has_value()) {
        return TextProtocol::Ok(val.value());
    }
    return TextProtocol::Err("NOT_FOUND", key);
}

// ========== Follower 处理 Leader 的 AppendEnties ==========
std::string RaftNode::HandleAppendEntries(const Command& cmd) {
    // 解析: APPEND <term> <leader_id> <prev_idx> <prev_term> <leader_commit> <entries...>
    if (cmd.args.size() < 5) {
        return TextProtocol::EncodeAppendResponse(current_term_, false);
    }

    uint64_t term = std::stoull(cmd.args[0]);
    uint64_t leader_id = std::stoull(cmd.args[1]);
    uint64_t prev_idx = std::stoull(cmd.args[2]);
    uint64_t prev_term = std::stoull(cmd.args[3]);
    uint64_t leader_commit = std::stoull(cmd.args[4]);

    // 检查 Term
    if (term < current_term_) {
        return TextProtocol::EncodeAppendResponse(current_term_, false);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 日志一致性检查: prev_idx 和 prev_term 必须匹配本地日志
    if (prev_idx > 0) {
        if (prev_idx >= log_.size()) {
            // Leader 日志比本地长, 拒绝并表示需要更早的日志 (实际需要 Leader 的 next_index-- 处理)
            return TextProtocol::EncodeAppendResponse(current_term_, false);
        }
        if (log_[prev_idx].term != prev_term) {
            return TextProtocol::EncodeAppendResponse(current_term_, false);
        }
    }

    // 截断冲突日志 (如果 prev_idx 之后有日志, 且 term 不一致)
    // 简化: 如果 prev_idx 之后有日志，全部删除
    if (log_.size() > prev_idx + 1) {
        log_.resize(prev_idx + 1);
    }

    // 追加新条目 (从 args[5] 开始)
    LogEntry entry;
    for (size_t i = 5 ; i < cmd.args.size() ; ++i) {
        if (entry.Deserialize(cmd.args[i])) {

        } else {
            entry.command += " " + cmd.args[i];
        }
    }
    // 确保索引连接
    if (entry.index == log_.size()) {
        log_.push_back(entry);
    }

    PrintRole();
    for (const auto& entry : log_) {
        std::cout << "Log[" << entry.index << "]: term = " << entry.term
            << ", cmd = " << entry.command << std::endl;
    }
    std::cout << std::endl;

    // 更新 commit_index (Follower 的 commit 不能超过 Leader 告知的 commit_index)
    if (leader_commit > commit_index_) {
        commit_index_ = std::min(leader_commit, GetLastLogIndex());
        cv_.notify_all();
    }

    PrintRole();
    std::cout << "commit_index: " << commit_index_ << ", applied_index: " << last_applied_ << std::endl;

    return TextProtocol::EncodeAppendResponse(current_term_, true);
}

// ========== 工具函数 ==========
uint64_t RaftNode::GetLastLogIndex() const {
    return log_.empty() ? 0 : log_.size() - 1;
}

uint64_t RaftNode::GetLastLogTerm() const {
    if (log_.size() <= 1) {
        return 0;
    }
    return log_.back().term;
}

void RaftNode::PrintRole() const {
    if (IsLeader()) {
        std::cout << "[Leader] ";
    } else {
        std::cout << "[Follower " << port_ << "] ";
    }
}
