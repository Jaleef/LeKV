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

        PrintRole();
        std::cout << "Shard " << shard_id << " -> " << peer.ip << ":" << peer.port << std::endl;
    }
}

uint32_t RaftNode::GetShardId(const std::string& key) const {
    return std::hash<std::string>{}(key) % kShardCount;
}

std::string RaftNode::HandleGetShard(const std::string& key) {
    uint32_t shard = GetShardId(key);
    auto it = shard_map_.find(shard);
    if (it == shard_map_.end()) {
        return TextProtocol::Err("NO_SHARD", "No shard for this key");
    }
    return TextProtocol::Ok(std::to_string(shard) + " " + it->second.ip + " " + std::to_string(it->second.port));
}

std::string RaftNode::HandleShards() {
    std::string result = std::to_string(shard_map_.size()) + " ";
    bool first = true;
    for (const auto& [shard_id, peer] : shard_map_) {
        if (!first) result += " ";
        result += std::to_string(shard_id) + ":" + peer.ip + ":" + std::to_string(peer.port);
        first = false;
    }
    return TextProtocol::Ok(result);
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
    // 启动二进制 RPC 服务
    binary_server_ = std::make_unique<BinaryRpcServer>(port_);

    // 绑定命令处理器
    auto handler = [this](uint32_t req_id, const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
        return this->HandleBinaryRequest(req_id, payload);
    };

    if (!binary_server_->Start(handler)) {
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
        if (IsDataNode() && wal_file_.is_open()) {
            std::lock_guard<std::mutex> lock(wal_mutex_);
            wal_file_.flush();
            wal_file_.close();
            std::cout << "[WAL] Closed" << std::endl;
        }
    }
}


// ========== 命令处理：Proxy 转发 或者 DataNode 本地处理 ==========
std::vector<uint8_t> RaftNode::HandleBinaryRequest(uint32_t req_id, const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        return {BinaryProtocol::ST_BAD_REQUEST};
    }

    uint8_t opcode = payload[0];

    if (IsProxy()) {
        // Proxy 只负责转发，不存数据
        if (opcode == BinaryProtocol::OP_GET_ROUTE) {
            return HandleProxyGetRoute(req_id, payload);
        } else if (opcode == BinaryProtocol::OP_SHARDS) {
            return HandleProxyShards(req_id);
        } else {
            // Proxy 不处理数据操作
            return {BinaryProtocol::ST_BAD_REQUEST};
        }
    } else {
        // DataNode 处理数据操作
        if (opcode == BinaryProtocol::OP_PUT) {
            return HandleDataNodePut(payload);
        } else if (opcode == BinaryProtocol::OP_GET) {
            return HandleDataNodeGet(payload);
        } else if (opcode == BinaryProtocol::OP_DELETE) {
            return HandleDataNodeDelete(payload);
        } else {
            return {BinaryProtocol::ST_BAD_REQUEST};
        }
    }
}

// ========== Proxy 处理逻辑 ==========
std::vector<uint8_t> RaftNode::HandleProxyGetRoute(uint32_t req_id, const std::vector<uint8_t>& payload) {
    if (payload.size() < 3) {
        // payload: [1B opcode][2B key_len][key_len bytes key]
        // 至少有 3B
        return {BinaryProtocol::ST_BAD_REQUEST};
    }
    uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    if (payload.size() < 3 + key_len) {
        return {BinaryProtocol::ST_BAD_REQUEST};
    }

    std::string key(payload.begin() + 3, payload.begin() + 3 + key_len);

    uint32_t shard = GetShardId(key);
    auto it = shard_map_.find(shard);
    if (it == shard_map_.end()) {
        return {BinaryProtocol::ST_NO_SHARD};
    }

    std::string route = it->second.ip + ":" + std::to_string(it->second.port);
    auto resp = BinaryProtocol::EncodeRouteResponse(req_id, BinaryProtocol::ST_OK, static_cast<uint8_t>(shard), 1, route);

    // BinaryRpcServer 会自动打包帧头，这里只要返回payload 部分

    std::vector<uint8_t> result{BinaryProtocol::ST_OK, static_cast<uint8_t>(shard)};
    uint32_t ep = htonl(1);
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 4);
    uint16_t rl = htons(route.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&rl), reinterpret_cast<uint8_t*>(&rl) + 2);
    result.insert(result.end(), route.begin(), route.end());
    return result;
}

std::vector<uint8_t> RaftNode::HandleProxyShards(uint32_t req_id) {
    std::string body = std::to_string(shard_map_.size()) + " ";
    bool first = true;
    for (const auto& [sid, peer] : shard_map_) {
        if (!first) body += " ";
        body += std::to_string(sid) + ":" + peer.ip + ":" + std::to_string(peer.port);
        first = false;
    }
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

// ========== DataNode 本地处理逻辑 ==========
std::vector<uint8_t> RaftNode::HandleDataNodePut(const std::vector<uint8_t>& payload) {
    if (payload.size() < 7) return {BinaryProtocol::ST_BAD_REQUEST};
    uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    uint32_t val_len = ntohl(*reinterpret_cast<const uint32_t*>(payload.data() + 3));
    if (payload.size() < 7 + key_len + val_len) return {BinaryProtocol::ST_BAD_REQUEST};

    std::string key(payload.begin() + 7, payload.begin() + 7 + key_len);
    std::string value(payload.begin() + 7 + key_len, payload.begin() + 7 + key_len + val_len);

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
    return {BinaryProtocol::ST_OK};
}

std::vector<uint8_t> RaftNode::HandleDataNodeGet(const std::vector<uint8_t>& payload) {
    if (payload.size() < 7) return {BinaryProtocol::ST_BAD_REQUEST};
    uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    uint32_t val_len = ntohl(*reinterpret_cast<const uint32_t*>(payload.data() + 3));
    if (val_len != 0) return {BinaryProtocol::ST_BAD_REQUEST};

    std::string key(payload.begin() + 7, payload.begin() + 7 + key_len);
    auto val = storage_.Get(key);
    if (val.has_value()) {
        std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
        uint32_t vl = htonl(val.value().size());
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&vl), reinterpret_cast<uint8_t*>(&vl) + 4);
        result.insert(result.end(), val.value().begin(), val.value().end());
        return result;
    } else {
        return {BinaryProtocol::ST_NOT_FOUND};
    }
}

std::vector<uint8_t> RaftNode::HandleDataNodeDelete(const std::vector<uint8_t>& payload) {
    if (payload.size() < 7) return {BinaryProtocol::ST_BAD_REQUEST};
    uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    uint32_t val_len = ntohl(*reinterpret_cast<const uint32_t*>(payload.data() + 3));
    if (val_len != 0) return {BinaryProtocol::ST_BAD_REQUEST};
    if (payload.size() < 7 + key_len) return {BinaryProtocol::ST_BAD_REQUEST};

    std::string key(payload.begin() + 7, payload.begin() + 7 + key_len);

    std::lock_guard<std::mutex> lock(mutex_);
    
    LogEntry entry;
    entry.term = 1;
    entry.index = GetLastLogIndex() + 1;
    entry.command = "DELETE " + key;
    
    log_.push_back(entry);
    AppendToWAL(entry);
    ApplyLogEntry(entry);
    
    return {BinaryProtocol::ST_OK};
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
