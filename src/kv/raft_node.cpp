#include "raft_node.h"

#include <iostream>
#include <algorithm>

#include "rpc/text_protocol.h"

RaftNode::RaftNode(uint64_t node_id, uint16_t port, const std::vector<PeerInfo>& peers)
    : node_id_(node_id), port_(port), peers_(peers) {

    if (IsProxy()) {
        std::cout << "[Node " << node_id_ << "] PROXY on port " << port_ << std::endl;

        // 建立到所有 DataNode 的管理连接
        for (const auto& peer : peers) {
            if (peer.port == LEADER_PORT) continue;
            auto client = std::make_unique<BinaryRpcClient>();
            if (client->Connect(peer.ip, peer.port)) {
                node_clients_[peer.id] = std::move(client);
                std::cout << "[Proxy] Connected to DataNode " << peer.id << std::endl;
            }
        }
        BuildInitialTablets();
    } else {
        std::cout << "[Node " << node_id_ << "] DATA NODE on port " << port_ << std::endl;
    }
}

RaftNode::~RaftNode() {
    Stop();
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

    if (binary_server_) { binary_server_->Stop(); }

    for (auto& [id, c] : node_clients_) { c->Close(); }
}

// ========== 节点地址查询 ==========
std::string RaftNode::GetNodeAddr(uint32_t node_id) const {
    for (const auto& p : peers_) {
        if (p.id == node_id) {
            return p.ip + ":" + std::to_string(p.port);
        }
    }
    return "";
}

// ========== 固定分片路由表（初始化后只读）==========
void RaftNode::BuildInitialTablets() {
    std::vector<uint32_t> data_nodes;
    for (const auto& p : peers_) {
        if (p.port != LEADER_PORT) data_nodes.push_back(p.id);
    }
    if (data_nodes.empty()) return;

    // 固定2分片：["", "m") 和 ["m", "")
    Tablet t1{1, "", "m", data_nodes[0], 0};
    Tablet t2{2, "m", "", data_nodes[1], 0};
    tablets_.push_back(t1);
    tablets_.push_back(t2);

    std::cout << "[Proxy] Tablet 1 [\"\", m) -> Node " << t1.node_id << std::endl;
    std::cout << "[Proxy] Tablet 2 [m, \"\") -> Node " << t2.node_id << std::endl;
}

// ========== Tablet 路由表：二分查找 ==========
size_t RaftNode::FindTabletIndex(const std::string& key) const {
    // 找到最后一个满足 tablet.start_key <= key 的 tablet
    size_t left = 0, right = tablets_.size();
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (tablets_[mid].start_key <= key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    // left 是第一个 start_key 大于 key 的位置，所以返回 left - 1
    if (left == 0) { return tablets_.size(); }  // 没有 tablet 包含这个 key
    return left - 1;
}

bool RaftNode::GetTabletRoute(const std::string& key, Tablet& out) const {
    if (tablets_.empty()) { return false; }

    size_t idx = FindTabletIndex(key);
    if (idx >= tablets_.size()) { return false; }

    const auto& t = tablets_[idx];
    // 检查 end_key：空表示无穷，否则 key 必须 < end_key
    if (!t.end_key.empty() && key >= t.end_key) { return false; }

    out = t;
    return true;
}

// ========== 命令处理：Proxy 转发 或者 DataNode 本地处理 ==========
std::vector<uint8_t> RaftNode::HandleBinaryRequest(uint32_t req_id, const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        return {BinaryProtocol::ST_BAD_REQUEST};
    }

    uint8_t opcode = payload[0];

    if (IsProxy()) {
        // Proxy 只负责转发，不存数据
        if (opcode == BinaryProtocol::OP_GET_ROUTE) { return HandleProxyGetRoute(req_id, payload); } 
        if (opcode == BinaryProtocol::OP_SHARDS) { return HandleProxyShards(req_id); } 
    } else {
        // DataNode 处理数据操作
        if (opcode == BinaryProtocol::OP_PUT) { return HandleDataNodePut(payload); }
        if (opcode == BinaryProtocol::OP_GET) { return HandleDataNodeGet(payload); } 
        if (opcode == BinaryProtocol::OP_DELETE) { return HandleDataNodeDelete(payload); }
    }

    return {BinaryProtocol::ST_BAD_REQUEST};
}

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

    Tablet t;
    if (!GetTabletRoute(key, t)) { return {BinaryProtocol::ST_NO_SHARD}; }

    std::string addr = GetNodeAddr(t.node_id);
    // 返回：Status(1) + TabletID(1) + Epoch(4) + RouteLen(2) + Route
    std::vector<uint8_t> result{BinaryProtocol::ST_OK};
    result.push_back(static_cast<uint8_t>(t.id));
    uint32_t ep = htonl(1);
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 4);
    uint16_t rl = htons(addr.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&rl), reinterpret_cast<uint8_t*>(&rl) + 2);
    result.insert(result.end(), addr.begin(), addr.end());

    // BinaryRpcServer 会自动打包帧头，这里只要返回payload 部分
    return result;
}

std::vector<uint8_t> RaftNode::HandleProxyShards(uint32_t req_id) {
    std::string body = std::to_string(tablets_.size()) + " ";
    bool first = true;
    for (const auto& t : tablets_) {
        if (!first) body += " ";
        body += std::to_string(t.id) + ":" + t.start_key + ":" + t.end_key + ":" + GetNodeAddr(t.node_id);
        first = false;
    }
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

std::vector<uint8_t> RaftNode::HandleDataNodePut(const std::vector<uint8_t>& payload) {
    if (payload.size() < 7) return {BinaryProtocol::ST_BAD_REQUEST};
    uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    uint32_t val_len = ntohl(*reinterpret_cast<const uint32_t*>(payload.data() + 3));
    if (payload.size() < 7 + key_len + val_len) return {BinaryProtocol::ST_BAD_REQUEST};

    std::string key(payload.begin() + 7, payload.begin() + 7 + key_len);
    std::string value(payload.begin() + 7 + key_len, payload.begin() + 7 + key_len + val_len);

    storage_.Put(key, value);
    PrintRole();
    std::cout << "PUT " << key << " " << value << std::endl;
    
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    return result;
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
    storage_.Delete(key);
    PrintRole();
    std::cout << "DELETE " << key << std::endl;
    
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    return result;
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

    std::cout << "Node: " << node_id_ << " running at port " << port_
        << " [" << (IsProxy() ? "Proxy" : "DataNode") << "]" << std::endl;

    
    // 阻塞等待
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return !running_; });

}

void RaftNode::PrintRole() const {
    if (IsProxy()) {
        std::cout << "[Proxy] ";
    } else {
        std::cout << "[DataNode " << port_ << "] ";
    }
}
