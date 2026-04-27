#include "raft_node.h"

#include <complex>
#include <iostream>
#include <bits/this_thread_sleep.h>

#include "rpc/text_protocol.h"

RaftNode::RaftNode(uint64_t node_id, uint16_t port, const std::vector<PeerInfo>& peers)
    : node_id_(node_id), port_(port), peers_(peers), rpc_server_(port) {

    if (IsProxy()) {
        std::cout << "[Node " << node_id_ << "] PROXY on port " << port_ << std::endl;

        BuildShardMap();

        // Proxy 建立到所有 DataNode 的连接
        for (const auto& [shard_id, peer]: shard_map_) {
            auto client = std::make_unique<RpcClient>();
            PrintRole();
            if (client->Connect(peer.ip, peer.port)) {
                shard_clients_[shard_id] = std::move(client);
                std::cout << "Connected to Shard " << shard_id << " at " << peer.ip << ":" << peer.port << std::endl;
            } else {
                std::cerr << "Warning: Failed to connect to Shard " << shard_id << std::endl;
            }
        }
        
    } else {
        std::cout << "[Node " << node_id_ << "] DATA NODE on port " << port_ << std::endl;
        // DataNode 初始化日志和 WAL
        log_.push_back({0, 0, ""});
        if (!InitWAL()) {
            PrintRole();
            std::cerr << "WAL init failed" << std::endl;
        }
    }
}

RaftNode::~RaftNode() {
    Stop();
}

// ========== 分片路由实现 ==========
void RaftNode::BuildShardMap() {
    uint32_t shard_id = 0;
    for (const auto& peer : peers_) {
        if (peer.port == LEADER_PORT) continue;
        shard_map_[shard_id] = peer;
        shard_id++;
    }
    std::cout << "[Proxy] Built shard map: " << shard_map_.size()
        << " shards (expect " << kShardCount << ")" << std::endl;
}

uint32_t RaftNode::GetShardId(const std::string& key) const {
    return std::hash<std::string>{}(key) % kShardCount;
}

RpcClient* RaftNode::GetShardClient(uint32_t shard_id) {
    auto it = shard_clients_.find(shard_id);
    if (it == shard_clients_.end()) return nullptr;

    if (!it->second->IsConnected()) {
        // 尝试重连
        auto peer_it = shard_map_.find(shard_id);
        if (peer_it != shard_map_.end()) {
            if (it->second->Connect(peer_it->second.ip, peer_it->second.port)) {
                return it->second.get();
            }
        }
        return nullptr;
    }
    return it->second.get();
}

std::string RaftNode::ForwardToShard(uint32_t shard_id, const std::string& cmd) {
    auto* client = GetShardClient(shard_id);
    if (!client) {
        return TextProtocol::Err("SHARD_DOWN", "Shard " + std::to_string(shard_id) + " unavailable");
    }

    std::string data = cmd;

    if (!EndsWith(data, "\r\n")) data += "\r\n";

    std::string resp = client->Send(data, 3000);    // 3秒超时
    if (resp.empty()) {
        return TextProtocol::Err("TIMEOUT", "Shard " + std::to_string(shard_id) + " timeout");
    }

    return resp;
}

// ========== DataNode 应用线程（WAL 恢复到 Storage） ==========
void RaftNode::ApplyLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return !running_ || last_applied_ < GetLastLogIndex();
        });

        while (last_applied_ < GetLastLogIndex()) {
            last_applied_++;
            if (last_applied_ < log_.size()) {
                const auto& entry = log_[last_applied_];
                lock.unlock();

                // 应用到存储引擎
                ApplyLogEntry(entry);

                lock.lock();
            }
        }
    }
}


void RaftNode::ApplyLogEntry(const LogEntry& entry) {
    // 解析 command (格式: "PUT k v" 或 "DELETE k"
    std::istringstream iss(entry.command);
    std::string cmd, key, value;
    iss >> cmd >> key >> value;
    
    PrintRole();
    if (cmd == "PUT") {
        storage_.Put(key, value);

        std::cout << "Applied[" << entry.index << "]: PUT " << key << " = " << value << std::endl;
    } else if (cmd == "DELETE") {
        storage_.Delete(key);

        std::cout << "Applied[" << entry.index << "]: DELETE " << key << std::endl;
    }
}

// ========== 生命周期 ==========
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

    // 启动 ApplyLoop （DataNode）
    if (IsDataNode()) {
        apply_thread_ = std::thread(&RaftNode::ApplyLoop, this);
    }

    std::cout << "Node: " << node_id_ << " running at port " << port_
        << " [" << (IsProxy() ? "Proxy" : "DataNode") << "]" << std::endl;

    
    // 阻塞等待
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return !running_; });

}

void RaftNode::Stop() {
    // 已经停止，避免重复执行
    if (!running_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();

    rpc_server_.Stop();

    if (apply_thread_.joinable()) { apply_thread_.join(); }

    // 关闭 WAL
    {
        std::lock_guard<std::mutex> lock(wal_mutex_);
        if (wal_file_.is_open()) {
            wal_file_.flush();
            wal_file_.close();
            std::cout << "[WAL] Closed" << std::endl;
        }
    }

    for (auto& [id, client] : shard_clients_) {
        client->Close();
    }
}


// ========== 命令处理：Proxy 转发 或者 DataNode 本地处理 ==========
std::string RaftNode::HandleCommand(const Command& cmd) {
    if (IsProxy()) {
        // Proxy 只负责转发，不存数据
        if (cmd.name == "PUT") {
            if (cmd.args.size() < 2) return TextProtocol::Err("ARGS", "PUT <key> <value>");
            return ProxyPut(cmd.args[0], cmd.args[1]);
        } else if (cmd.name == "GET") {
            if (cmd.args.empty()) return TextProtocol::Err("ARGS", "GET <key>");
            return ProxyGet(cmd.args[0]);
        }
        else if (cmd.name == "DELETE" || cmd.name == "DEL") {
            if (cmd.args.empty()) return TextProtocol::Err("ARGS", "DELETE <key>");
            return ProxyDelete(cmd.args[0]);
        }
    } else {
        // DataNode: 直接处理本地存储请求；
        if (cmd.name == "PUT") {
            if (cmd.args.size() < 2) {
                return TextProtocol::Err("ARGS", "PUT <key> <value>");
            }
            return DataNodePut(cmd.args[0], cmd.args[1]);

        } else if (cmd.name == "GET") {
            if (cmd.args.empty()) {
                return TextProtocol::Err("ARGS", "GET <key>");
            }
            return DataNodeGet(cmd.args[0]);

        } else if (cmd.name == "DELETE" || cmd.name == "DEL") {
            if (cmd.args.empty()) {
                return TextProtocol::Err("ARGS", "DELETE <key>");
            }
            return DataNodeDelete(cmd.args[0]);

        } 
    }
    return TextProtocol::Err("UNKNOWN", cmd.name);
}

// ========== Proxy 转发逻辑 ==========
std::string RaftNode::ProxyPut(const std::string& key, const std::string& value) {
    uint32_t shard = GetShardId(key);
    std::string cmd = "PUT " + key + " " + value;
    std::cout << "[Proxy] Forwarding PUT " << key << " -> Shard " << shard << std::endl;
    return ForwardToShard(shard, cmd);
}

std::string RaftNode::ProxyGet(const std::string& key) {
    uint32_t shard = GetShardId(key);
    std::string cmd = "GET " + key;
    std::cout << "[Proxy] Forwarding GET " << key << " -> Shard " << shard << std::endl;
    return ForwardToShard(shard, cmd);
}

std::string RaftNode::ProxyDelete(const std::string& key) {
    uint32_t shard = GetShardId(key);
    std::string cmd = "DELETE " + key;
    std::cout << "[Proxy] Forwarding DELETE " << key << " -> Shard " << shard << std::endl;
    return ForwardToShard(shard, cmd);
}

// ========== DataNode 本地处理逻辑 ==========
std::string RaftNode::DataNodePut(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    LogEntry entry;
    entry.term = 1;     // 固定为 1，目前无实际意义
    entry.index = GetLastLogIndex() + 1;
    entry.command = "PUT " + key + " " + value;

    log_.push_back(entry);
    AppendToWAL(entry);
    ApplyLogEntry(entry);

    PrintRole();
    std::cout << "Stored " << key << " " << value << std::endl;
    return TextProtocol::Ok();
}

std::string RaftNode::DataNodeGet(const std::string& key) {
    auto val = storage_.Get(key);
    if (val.has_value()) {
        return TextProtocol::Ok(val.value());
    }
    return TextProtocol::Err("NOT_FOUND", key);
}

std::string RaftNode::DataNodeDelete(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LogEntry entry;
    entry.term = 1;
    entry.index = GetLastLogIndex() + 1;
    entry.command = "DELETE " + key;
    
    log_.push_back(entry);
    AppendToWAL(entry);
    ApplyLogEntry(entry);
    
    return TextProtocol::Ok();
}

// ========== DataNode WAL 实现 ==========
bool RaftNode::InitWAL() {
    wal_filename_ = "wal_" + std::to_string(node_id_) + ".log";

    // 尝试恢复
    RestoreFromWAL();

    // 以追加模式打开 (如果不存在则创建)
    wal_file_.open(wal_filename_, std::ios::binary | std::ios::app);
    if (!wal_file_.is_open()) {
        PrintRole();
        std::cerr << "Failed to open file: " << wal_filename_ << std::endl;
        return false;
    }

    PrintRole();
    std::cout << "WAL ready, restored " << log_.size() - 1 << " entries" << std::endl;
    
    return true;
}

void RaftNode::AppendToWAL(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(wal_mutex_);

    if (!wal_file_.is_open()) {
        return ;
    }

    // 二进制格式：[8 bytes term][8 bytes index][4 bytes cmd_len][cmd_len bytes command]
    uint64_t term = entry.term;
    uint64_t index = entry.index;
    uint32_t cmd_len = static_cast<uint32_t>(entry.command.size());

    wal_file_.write(reinterpret_cast<const char*>(&term), sizeof(term));
    wal_file_.write(reinterpret_cast<const char*>(&index), sizeof(index));
    wal_file_.write(reinterpret_cast<const char*>(&cmd_len), sizeof(cmd_len));
    wal_file_.write(entry.command.data(), cmd_len);

    // 批量刷盘策略：每 WAL_FSYNC_INTERVAL 条目刷一次
    if (entry.index - wal_last_fsync_index_ >= WAL_FSYNC_INTERVAL) {
        wal_file_.flush();
        wal_last_fsync_index_ = entry.index;
    }
}

void RaftNode::RestoreFromWAL() {
    std::ifstream file(wal_filename_, std::ios::binary);
    if (!file || !file.is_open()) {
        PrintRole();
        std::cout << "No existing WAL file, starting fresh" << std::endl;
        return;
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size == 0) {
        PrintRole();
        std::cout << "Empty WAL file" << std::endl;
        return;
    }

    uint64_t max_index = 0;

    while (file.tellg() < file_size) {
        uint64_t term, index;
        uint32_t cmd_len;

        // 读取条目头
        file.read(reinterpret_cast<char*>(&term), sizeof(term));
        file.read(reinterpret_cast<char*>(&index), sizeof(index));
        file.read(reinterpret_cast<char*>(&cmd_len), sizeof(cmd_len));

        if (file.gcount() != sizeof(cmd_len)) {
            break;  // 读取失败，可能是文件损坏或不完整
        }

        // 读取命令
        std::string command(cmd_len, '\0');
        file.read(&command[0], cmd_len);

        // 恢复到内存日志
        log_.push_back({term, index, command});
        max_index = index;
    }

    if (max_index > 0) {
        PrintRole();
        std::cout << "Restored " << (log_.size() - 1) << " entries" << std::endl;
    }
}

// ========== 工具函数 ==========
uint64_t RaftNode::GetLastLogIndex() const {
    return log_.empty() ? 0 : log_.size() - 1;
}

void RaftNode::PrintRole() const {
    if (IsProxy()) {
        std::cout << "[Proxy] ";
    } else {
        std::cout << "[DataNode " << port_ << "] ";
    }
}

bool RaftNode::EndsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}
