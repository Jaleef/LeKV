#include "raft_node.h"

#include <iostream>
#include <algorithm>
#include <cstring>

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

        std::string db_path = "db_" + std::to_string(node_id_);
        try {
            storage_ = std::make_unique<StorageEngine>(db_path);
            std::cout << "[DataNode " << port_ << "] LevelDB opened at " << db_path << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[DataNode " << port_ << "] Failed to open LevelDB: " << e.what() << std::endl;
        }
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
    balancer_running_ = false;

    if (binary_server_) { binary_server_->Stop(); }

    if (balancer_thread_.joinable()) { balancer_thread_.join(); }

    for (auto& [id, c] : node_clients_) { c->Close(); }

    storage_.reset();  // 关闭 LevelDB，释放资源
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
    next_tablet_id_ = 3;
    epoch_ = 1;

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
    std::shared_lock<std::shared_mutex> lock(tablet_mutex_);

    if (tablets_.empty()) { return false; }

    size_t idx = FindTabletIndex(key);
    if (idx >= tablets_.size()) { return false; }

    const auto& t = tablets_[idx];
    // 检查 end_key：空表示无穷，否则 key 必须 < end_key
    if (!t.end_key.empty() && key >= t.end_key) { return false; }

    out = t;
    return true;
}

// ========== Tablet Stats 同步（栈拷贝，无迭代器失效）==========
void RaftNode::SyncAllTabletStats() {
    std::vector<Tablet> copy;
    {
        std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
        if (tablets_.empty()) return;
        copy = tablets_;
    }

    std::cout << "[Sync] Syncing tablet stats..." << std::endl;
    bool changed = false;

    for (const auto& t : copy) {
        uint64_t key_count = 0;
        std::string median_key;
        bool ok = QueryTabletStats(t.node_id, t.start_key, t.end_key, key_count, median_key);
        if (!ok) continue;

        {
            std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
            for (auto& real : tablets_) {
                if (real.id == t.id) {
                    if (real.key_count != key_count) {
                        real.key_count = key_count;
                        changed = true;
                    }
                    break;
                }
            }
        }
    }

    if (changed) std::cout << "[Sync] Stats updated" << std::endl;
    std::cout << "[Sync] Done" << std::endl;
}

// ========== 向 DataNode 查询 Tablet 统计 ==========
bool RaftNode::QueryTabletStats(uint32_t node_id, const std::string& start,
                                const std::string& end, uint64_t& key_count,
                                std::string& median_key) {
    auto it = node_clients_.find(node_id);
    if (it == node_clients_.end() || !it->second->IsConnected()) return false;

    uint32_t rid = 1;
    std::vector<uint8_t> payload{BinaryProtocol::OP_TABLET_STATS};
    uint16_t sl = htons(start.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&sl), reinterpret_cast<uint8_t*>(&sl) + 2);
    payload.insert(payload.end(), start.begin(), start.end());
    uint16_t el = htons(end.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&el), reinterpret_cast<uint8_t*>(&el) + 2);
    payload.insert(payload.end(), end.begin(), end.end());

    auto req = BinaryProtocol::EncodeCustomRequest(rid, payload);
    if (!it->second->Send(req)) return false;

    std::vector<uint8_t> resp_frame;
    if (!it->second->RecvFrame(resp_frame, 3000)) return false;

    uint32_t resp_req;
    std::vector<uint8_t> resp_payload;
    size_t consumed = 0;
    if (!BinaryProtocol::TryDecode(resp_frame, consumed, resp_req, resp_payload)) return false;

    // [1B Status][4B ValueLen][4B KeyCount][2B MedianLen][MedianKey]
    if (resp_payload.size() < 11) return false;
    if (resp_payload[0] != BinaryProtocol::ST_OK) return false;

    key_count = ntohl(*reinterpret_cast<const uint32_t*>(resp_payload.data() + 5));
    uint16_t med_len = ntohs(*reinterpret_cast<const uint16_t*>(resp_payload.data() + 9));
    if (resp_payload.size() < 11 + med_len) return false;
    median_key.assign(resp_payload.begin() + 11, resp_payload.begin() + 11 + med_len);
    return true;
}

// ========== 尝试分裂 Tablet（栈拷贝 + 无锁 RPC + 一次性加锁）==========
bool RaftNode::TrySplitTablet(size_t idx) {
    // Step 1: 读锁下拷贝到栈，立即释放
    Tablet old_t;
    {
        std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
        if (idx >= tablets_.size()) return false;
        old_t = tablets_[idx];
        if (old_t.key_count < SPLIT_THRESHOLD) return false;
    }

    // Step 2: 无锁状态下做 RPC（可能阻塞）
    std::string median_key;
    bool ok = QueryTabletStats(old_t.node_id, old_t.start_key, old_t.end_key,
                               old_t.key_count, median_key);
    if (!ok || median_key.empty() ||
        median_key <= old_t.start_key ||
        (!old_t.end_key.empty() && median_key >= old_t.end_key)) {
        return false;
    }

    // Step 3: 一次性加锁做纯内存修改
    {
        std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
        if (idx >= tablets_.size() || tablets_[idx].id != old_t.id) return false;
        if (tablets_[idx].key_count < SPLIT_THRESHOLD) return false;

        Tablet left = old_t;
        Tablet right = old_t;
        left.id = next_tablet_id_++;
        right.id = next_tablet_id_++;
        left.end_key = median_key;
        right.start_key = median_key;
        left.key_count = old_t.key_count / 2;
        right.key_count = old_t.key_count - left.key_count;

        tablets_[idx] = left;
        tablets_.insert(tablets_.begin() + idx + 1, right);
        epoch_++;

        std::cout << "[Proxy] Split T" << old_t.id
                  << " [" << old_t.start_key << "," << old_t.end_key
                  << ") at \"" << median_key << "\" -> T" << left.id
                  << " [" << left.start_key << "," << left.end_key
                  << ") & T" << right.id
                  << " [" << right.start_key << "," << right.end_key << ")" << std::endl;
    }
    return true;
}

// ========== 负载均衡（差值比较，无除零）==========
bool RaftNode::DoLoadBalance() {
    std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
    if (tablets_.size() < 2) return false;

    std::map<uint32_t, uint64_t> node_load;
    for (const auto& t : tablets_) node_load[t.node_id] += t.key_count;
    if (node_load.size() < 2) return false;

    auto max_it = std::max_element(node_load.begin(), node_load.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    auto min_it = std::min_element(node_load.begin(), node_load.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    uint64_t diff = (max_it->second > min_it->second) ? (max_it->second - min_it->second) : 0;
    if (diff < BALANCE_DIFF_THRESHOLD) return false;

    uint32_t src_node = max_it->first;
    uint32_t dst_node = min_it->first;

    // 找 src_node 上 key_count 最大的 Tablet
    size_t target_idx = tablets_.size();
    uint64_t max_count = 0;
    for (size_t i = 0; i < tablets_.size(); ++i) {
        if (tablets_[i].node_id == src_node && tablets_[i].key_count > max_count) {
            max_count = tablets_[i].key_count;
            target_idx = i;
        }
    }
    if (target_idx >= tablets_.size()) return false;

    std::string start = tablets_[target_idx].start_key;
    std::string end = tablets_[target_idx].end_key;
    lock.unlock();  // 释放锁后再做数据迁移（RPC 可能阻塞）

    bool ok = MigrateTablet(target_idx, dst_node);
    if (ok) {
        std::cout << "[Proxy] Move T" << tablets_[target_idx].id
                  << " N" << src_node << "->N" << dst_node << std::endl;
    }
    return ok;
}

// ========== 数据迁移：扫描 + 写入 ==========
bool RaftNode::MigrateTablet(size_t idx, uint32_t dst_node) {
    Tablet t;
    {
        std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
        if (idx >= tablets_.size()) return false;
        t = tablets_[idx];
    }

    // 数据已在目标节点？无需迁移
    if (t.node_id == dst_node) return false;

    std::cout << "[Migrate] Scanning T" << t.id << " [" << t.start_key << "," << t.end_key
              << ") from N" << t.node_id << " -> N" << dst_node << std::endl;

    bool ok = ScanAndTransferData(t.node_id, dst_node, t.start_key, t.end_key);
    if (!ok) {
        std::cerr << "[Migrate] Data transfer failed for T" << t.id << std::endl;
        return false;
    }

    // 更新路由表
    {
        std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
        if (idx < tablets_.size() && tablets_[idx].id == t.id) {
            tablets_[idx].node_id = dst_node;
            epoch_++;
            std::cout << "[Migrate] T" << t.id << " now on N" << dst_node << std::endl;
        }
    }
    return true;
}

bool RaftNode::ScanAndTransferData(uint32_t src_node, uint32_t dst_node,
                                   const std::string& start, const std::string& end) {
    // 1. 向源节点发 OP_SCAN_RANGE 请求
    auto src_it = node_clients_.find(src_node);
    auto dst_it = node_clients_.find(dst_node);
    if (src_it == node_clients_.end() || !src_it->second->IsConnected()) return false;
    if (dst_it == node_clients_.end() || !dst_it->second->IsConnected()) return false;

    uint32_t rid = 1;
    std::vector<uint8_t> payload{BinaryProtocol::OP_SCAN_RANGE};
    uint16_t sl = htons(start.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&sl), reinterpret_cast<uint8_t*>(&sl) + 2);
    payload.insert(payload.end(), start.begin(), start.end());
    uint16_t el = htons(end.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&el), reinterpret_cast<uint8_t*>(&el) + 2);
    payload.insert(payload.end(), end.begin(), end.end());

    auto req = BinaryProtocol::EncodeCustomRequest(rid, payload);
    if (!src_it->second->Send(req)) return false;

    std::vector<uint8_t> resp_frame;
    if (!src_it->second->RecvFrame(resp_frame, 30000)) return false;  // 扫描可能慢，给 30s

    uint32_t resp_req;
    std::vector<uint8_t> resp_payload;
    size_t consumed = 0;
    if (!BinaryProtocol::TryDecode(resp_frame, consumed, resp_req, resp_payload)) return false;
    if (resp_payload.empty() || resp_payload[0] != BinaryProtocol::ST_OK) return false;
    if (resp_payload.size() < 5) return false;  // [Status][4B val_len] at least

    // 2. 解析响应中的 KV 列表
    // payload: [Status][4B val_len][4B count][count * {2B key_len}{key}{4B val_len}{value}]
    uint32_t val_len = ntohl(*reinterpret_cast<const uint32_t*>(resp_payload.data() + 1));
    if (resp_payload.size() < 5 + val_len) return false;

    size_t pos = 5;
    if (pos + 4 > resp_payload.size()) return false;
    uint32_t count = ntohl(*reinterpret_cast<const uint32_t*>(resp_payload.data() + pos));
    pos += 4;

    std::cout << "[Migrate] Transferring " << count << " keys..." << std::endl;
    uint32_t transferred = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > resp_payload.size()) break;
        uint16_t key_len = ntohs(*reinterpret_cast<const uint16_t*>(resp_payload.data() + pos));
        pos += 2;
        if (pos + key_len > resp_payload.size()) break;
        std::string key(resp_payload.begin() + pos, resp_payload.begin() + pos + key_len);
        pos += key_len;

        if (pos + 4 > resp_payload.size()) break;
        uint32_t vlen = ntohl(*reinterpret_cast<const uint32_t*>(resp_payload.data() + pos));
        pos += 4;
        if (pos + vlen > resp_payload.size()) break;
        std::string value(resp_payload.begin() + pos, resp_payload.begin() + pos + vlen);
        pos += vlen;

        // 3. 向目标节点发 PUT
        uint32_t put_rid = rid + i + 1;
        auto put_req = BinaryProtocol::EncodeRequest(put_rid, BinaryProtocol::OP_PUT, key, value);
        if (!dst_it->second->Send(put_req)) {
            std::cerr << "[Migrate] PUT failed for key=" << key << std::endl;
            continue;
        }

        std::vector<uint8_t> put_resp_frame;
        if (!dst_it->second->RecvFrame(put_resp_frame, 5000)) {
            std::cerr << "[Migrate] PUT recv timeout for key=" << key << std::endl;
            continue;
        }
        transferred++;
    }

    std::cout << "[Migrate] Transferred " << transferred << "/" << count << " keys" << std::endl;
    return transferred == count;
}

// ========== 均衡器主循环 ==========
void RaftNode::BalancerLoop() {
    using namespace std::chrono;
    while (balancer_running_) {
        std::this_thread::sleep_for(seconds(5));
        if (!balancer_running_) break;

        SyncAllTabletStats();

        // 找第一个需要分裂的 Tablet
        size_t split_idx = tablets_.size();
        {
            std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
            for (size_t i = 0; i < tablets_.size(); ++i) {
                if (tablets_[i].key_count >= SPLIT_THRESHOLD) {
                    split_idx = i;
                    break;
                }
            }
        }
        if (split_idx < tablets_.size()) {
            TrySplitTablet(split_idx);
            continue;
        }

        DoLoadBalance();
    }
}

// ========== DataNode 统计（LevelDB RangeStats）==========
bool RaftNode::DataNodeGetTabletStats(const std::string& start, const std::string& end,
                                      uint64_t& key_count, std::string& median_key) {
    if (!storage_) {
        key_count = 0;
        median_key.clear();
        return false;
    }
    return storage_->RangeStats(start, end, key_count, median_key);
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
        if (opcode == BinaryProtocol::OP_TABLET_STATS) { return HandleDataNodeTabletStats(payload); }
        if (opcode == BinaryProtocol::OP_SCAN_RANGE) { return HandleDataNodeScanRange(req_id, payload); }
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
    // 返回：Status(1) + ValueLen(4) + TabletID(1) + Epoch(4) + RouteLen(2) + Route
    std::vector<uint8_t> result{BinaryProtocol::ST_OK};
    result.push_back(static_cast<uint8_t>(t.id));
    uint32_t ep = htonl(static_cast<uint32_t>(epoch_));
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 4);
    uint16_t rl = htons(addr.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&rl), reinterpret_cast<uint8_t*>(&rl) + 2);
    result.insert(result.end(), addr.begin(), addr.end());

    // BinaryRpcServer 会自动打包帧头, 并且将 ValueLen 字段写入，这里只要返回payload 部分
    return result;
}

std::vector<uint8_t> RaftNode::HandleProxyShards(uint32_t req_id) {
    std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
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

    if (!storage_ || !storage_->Put(key, value)) {
        return {BinaryProtocol::ST_TIMEOUT};
    }
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
    
    if (!storage_) { return {BinaryProtocol::ST_TIMEOUT}; }

    auto val = storage_->Get(key);
    if (val.has_value()) {
        PrintRole();
        std::cout << "GET " << key << " -> " << val.value() << std::endl;
        
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
    
    if (!storage_ || !storage_->Delete(key)) {
        return {BinaryProtocol::ST_TIMEOUT};
    }

    PrintRole();
    std::cout << "DELETE " << key << std::endl;
    
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    return result;
}

std::vector<uint8_t> RaftNode::HandleDataNodeTabletStats(const std::vector<uint8_t>& payload) {
    if (payload.size() < 5) return {BinaryProtocol::ST_BAD_REQUEST};

    uint16_t sl = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    if (payload.size() < 1 + 2 + sl + 2) return {BinaryProtocol::ST_BAD_REQUEST};
    std::string start(payload.begin() + 3, payload.begin() + 3 + sl);

    uint16_t el = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 3 + sl));
    if (payload.size() < 1 + 2 + sl + 2 + el) return {BinaryProtocol::ST_BAD_REQUEST};
    std::string end(payload.begin() + 3 + sl + 2, payload.begin() + 3 + sl + 2 + el);

    uint64_t key_count = 0;
    std::string median_key;
    DataNodeGetTabletStats(start, end, key_count, median_key);

    // [1B Status][4B val_len][4B key_count][2B median_len][median_key]
    std::vector<uint8_t> result{BinaryProtocol::ST_OK};
    uint32_t kc = htonl(static_cast<uint32_t>(key_count));
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&kc), reinterpret_cast<uint8_t*>(&kc) + 4);
    uint16_t ml = htons(static_cast<uint16_t>(median_key.size()));
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ml), reinterpret_cast<uint8_t*>(&ml) + 2);
    result.insert(result.end(), median_key.begin(), median_key.end());
    return result;
}

// ========== DataNode 区间扫描（OP_SCAN_RANGE）==========
std::vector<uint8_t> RaftNode::HandleDataNodeScanRange(uint32_t req_id, const std::vector<uint8_t>& payload) {
    if (payload.size() < 5) return {BinaryProtocol::ST_BAD_REQUEST};

    uint16_t sl = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 1));
    if (payload.size() < 1 + 2 + sl + 2) return {BinaryProtocol::ST_BAD_REQUEST};
    std::string start(payload.begin() + 3, payload.begin() + 3 + sl);

    uint16_t el = ntohs(*reinterpret_cast<const uint16_t*>(payload.data() + 3 + sl));
    if (payload.size() < 1 + 2 + sl + 2 + el) return {BinaryProtocol::ST_BAD_REQUEST};
    std::string end(payload.begin() + 3 + sl + 2, payload.begin() + 3 + sl + 2 + el);

    if (!storage_) return {BinaryProtocol::ST_TIMEOUT};

    // 扫描区间并编码
    // body 格式: [4B count][count * {2B key_len}{key}{4B val_len}{value}]
    std::vector<uint8_t> body;
    uint32_t count = 0;

    // 预留 count 的位置
    body.resize(4);

    storage_->RangeQuery(start, end, [&](const std::string& k, const std::string& v) {
        uint16_t kl = htons(static_cast<uint16_t>(k.size()));
        body.insert(body.end(), reinterpret_cast<uint8_t*>(&kl), reinterpret_cast<uint8_t*>(&kl) + 2);
        body.insert(body.end(), k.begin(), k.end());
        uint32_t vl = htonl(static_cast<uint32_t>(v.size()));
        body.insert(body.end(), reinterpret_cast<uint8_t*>(&vl), reinterpret_cast<uint8_t*>(&vl) + 4);
        body.insert(body.end(), v.begin(), v.end());
        count++;
        return true;
    });

    // 回填 count
    uint32_t cnt_be = htonl(count);
    memcpy(body.data(), &cnt_be, 4);

    PrintRole();
    std::cout << "SCAN_RANGE [" << start << "," << end << ") -> " << count << " keys" << std::endl;

    // 用 EncodeResponse 打包: [Status][4B val_len][value=body]
    std::string value_str(body.begin(), body.end());
    std::vector<uint8_t> result = {BinaryProtocol::ST_OK};
    result.insert(result.end(), value_str.begin(), value_str.end());
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
    if (IsProxy()) {
        balancer_running_ = true;
        balancer_thread_ = std::thread(&RaftNode::BalancerLoop, this);
    }

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
