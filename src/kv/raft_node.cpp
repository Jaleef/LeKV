#include "raft_node.h"

#include <complex>
#include <iostream>
#include <bits/this_thread_sleep.h>

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
                std::cout << "[Proxy] Management channel to Node " << peer.id << std::endl;
            }
        }
        InitTablets();
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
    if (apply_thread_.joinable()) { apply_thread_.join(); }
    if (balancer_thread_.joinable()) { balancer_thread_.join(); }

    // 关闭 WAL
    if (IsDataNode() && wal_file_.is_open()) {
        std::lock_guard<std::mutex> lock(wal_mutex_);
        wal_file_.flush();
        wal_file_.close();
        std::cout << "[WAL] Closed" << std::endl;
    }

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

// ========== 元数据持久化 ==========
void RaftNode::SaveMeta() {
    std::string filename = "meta_" + std::to_string(node_id_) + ".json";
    std::ofstream ofs(filename);
    if (!ofs) return;

    ofs << "{\n";
    ofs << "  \"epoch\": " << epoch_ << ",\n";
    ofs << "  \"next_tablet_id\": " << next_tablet_id_ << ",\n";
    ofs << "  \"tablets\": [\n";
    for (size_t i = 0; i < tablets_.size(); ++i) {
        const auto& t = tablets_[i];
        ofs << "    {\"id\": " << t.id 
            << ", \"start\": \"" << t.start_key << "\""
            << ", \"end\": \"" << t.end_key << "\""
            << ", \"node_id\": " << t.node_id
            << ", \"key_count\": " << t.key_count << "}";
        if (i + 1 < tablets_.size()) ofs << ",";
        ofs << "\n";
    }
    ofs << "  ]\n}\n";
}

void RaftNode::LoadMeta() {
    std::string filename = "meta_" + std::to_string(node_id_) + ".json";
    std::ifstream ifs(filename);
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("\"epoch\"") != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) epoch_ = std::stoull(line.substr(pos + 1));
        }
        else if (line.find("\"next_tablet_id\"") != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) next_tablet_id_ = std::stoull(line.substr(pos + 1));
        }
        else if (line.find("\"id\"") != std::string::npos) {
            Tablet t;
            size_t p = line.find("\"id\":");
            if (p != std::string::npos) t.id = std::stoull(line.substr(p + 5));
            p = line.find("\"start\":");
            if (p != std::string::npos) {
                size_t q1 = line.find('\"', p + 8);
                size_t q2 = line.find('\"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    t.start_key = line.substr(q1 + 1, q2 - q1 - 1);
            }
            p = line.find("\"end\":");
            if (p != std::string::npos) {
                size_t q1 = line.find('\"', p + 6);
                size_t q2 = line.find('\"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    t.end_key = line.substr(q1 + 1, q2 - q1 - 1);
            }
            p = line.find("\"node_id\":");
            if (p != std::string::npos) t.node_id = std::stoul(line.substr(p + 10));
            p = line.find("\"key_count\":");
            if (p != std::string::npos) t.key_count = std::stoull(line.substr(p + 12));
            tablets_.push_back(t);
        }
    }
}

void RaftNode::BuildInitialTablets() {
    // 根据 DataNode 数量创建初始 Tablet
    std::vector<uint32_t> data_nodes;
    for (const auto& p : peers_) {
        if (p.port != LEADER_PORT) {
            data_nodes.push_back(p.id);
        }
    }

    if (data_nodes.empty()) return;

    // 简单按字母表切分：a - m, m - z 等
    // 2 节点：["", "m"), ["m", "")
    // 3 节点：["", "i"), ["i", "r"), ["r", "")
    // 更多节点类似
    const char* splits[] = {"", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                            "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", ""};
    int split_count = static_cast<int>(data_nodes.size());
    int step = 26 / split_count;  // 26 个字母平均分

    for (size_t i = 0 ; i < data_nodes.size() ; ++i) {
        Tablet t;
        t.id = next_tablet_id_++;
        int start_idx = i * step;
        int end_idx = (i == data_nodes.size() - 1) ? 27 : (i + 1) * step;
        t.start_key = splits[start_idx];
        t.end_key = splits[end_idx];
        t.node_id = data_nodes[i];
        t.key_count = 0;
        tablets_.push_back(t);

        std::cout << "[Proxy] Initial Tablet " << t.id
                  << " [" << t.start_key << ", " << t.end_key
                  << ") -> Node " << t.node_id << std::endl;
    }

    epoch_ = 1;
    SaveMeta();
}

void RaftNode::InitTablets() {
    LoadMeta();
    if (tablets_.empty()) {
        BuildInitialTablets();
    } else {
        std::cout << "[Proxy] Loaded " << tablets_.size()
                  << " tablets, epoch=" << epoch_ << std::endl;
    }
}

// ========== 自动分裂与负载均衡 ==========
bool RaftNode::QueryTabletStats(uint32_t node_id, const std::string& start,
                                const std::string& end, uint64_t& key_count,
                                std::string& median_key) {
    auto it = node_clients_.find(node_id);
    if (it == node_clients_.end() || !it->second->IsConnected()) { return false; }

    uint32_t rid = 1;  // 管理请求用固定 ID 或自增
    // 构造 TABLET_STATS 请求
    // payload: [opcode][2B start_len][start][2B end_len][end]
    std::vector<uint8_t> payload{BinaryProtocol::OP_TABLET_STATS};
    uint16_t sl = htons(start.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&sl), reinterpret_cast<uint8_t*>(&sl) + 2);
    payload.insert(payload.end(), start.begin(), start.end());
    uint16_t el = htons(end.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&el), reinterpret_cast<uint8_t*>(&el) + 2);
    payload.insert(payload.end(), end.begin(), end.end());

    auto req = BinaryProtocol::EncodeCustomRequest(rid, payload);

    if (!it->second->Send(req)) { return false; }

    std::vector<uint8_t> resp_frame;
    if(!it->second->RecvFrame(resp_frame, 3000)) { return false; }

    uint32_t resp_req;
    std::vector<uint8_t> resp_payload;
    size_t consumed = 0;
    if (!BinaryProtocol::TryDecode(resp_frame, consumed, resp_req, resp_payload)) { return false; }
    if (resp_payload.size() < 7) { return false; }

    uint8_t status = resp_payload[0];
    key_count = ntohl(*reinterpret_cast<uint32_t*>(resp_payload.data() + 1));

    uint16_t med_len = ntohs(*reinterpret_cast<uint16_t*>(resp_payload.data() + 5));
    if (resp_payload.size() < 7 + med_len) { return false; }

    median_key.assign(resp_frame.begin() + 7, resp_frame.begin() + 7 + med_len);
    return true;
}

bool RaftNode::TrySplitTablet(size_t idx) {
    std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
    if (idx >= tablets_.size()) { return false; }

    Tablet& t = tablets_[idx];
    uint64_t key_count = 0;
    std::string median_key;

    // 查询 DataNode 获取统计信息
    if (!QueryTabletStats(t.node_id, t.start_key, t.end_key, key_count, median_key)) {
        // 查询失败，使用本地缓存的 key_count（如果有）来判断是否需要分裂
        key_count = t.key_count;
        if (key_count < SPLIT_THRESHOLD) { return false; }

        median_key = t.start_key;
        if (!t.end_key.empty()) {
            median_key += t.end_key.substr(0, 1);
        } else {
            median_key += "m";  // 简单处理：如果没有 end_key，就在 start_key 基础上加个 'm' 作为分裂点
        }
    }

    if (key_count < SPLIT_THRESHOLD) { return false; }
    if (median_key.empty() || median_key <= t.start_key) { return false; }
    if (!t.end_key.empty() && median_key >= t.end_key) { return false; }

    // 执行分裂
    Tablet new_a = t;
    Tablet new_b = t;
    new_a.id = next_tablet_id_++;
    new_b.id = next_tablet_id_++;

    new_a.end_key = median_key;
    new_b.start_key = median_key;

    new_a.key_count = key_count / 2;  // 这里简单假设分裂后两边的 key_count 大致相等
    new_b.key_count = key_count - new_a.key_count;

    // 替换
    tablets_[idx] = new_a;
    tablets_.insert(tablets_.begin() + idx + 1, new_b);
    epoch_++;

    std::cout << "[Proxy] Split Tablet " << t.id << " at \"" << median_key << "\""
              << " -> " << new_a.id << " [" << new_a.start_key << ", " << new_a.end_key
              << ") and " << new_b.id << " [" << new_b.start_key << ", " << new_b.end_key
              << ")" << std::endl;
    
    SaveMeta();
    return true;
}

bool RaftNode::DoLoadBalance() {
    std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
    if (tablets_.empty()) { return false; }

    // 计算每个节点的负载
    std::map<uint32_t, uint64_t> node_load;
    for (const auto& t : tablets_) {
        node_load[t.node_id] += t.key_count;
    }
    if (node_load.size() < 2) { return false; }  // 不足两个节点无需负载均衡

    auto max_it = std::max_element(node_load.begin(), node_load.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    auto min_it = std::min_element(node_load.begin(), node_load.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    if (max_it->second == 0) { return false; }  // 最大负载为 0，无需均衡
    double ratio = static_cast<double>(max_it->second) / min_it->second;
    if (ratio < BALANCE_RATIO) { return false; }  // 负载差异不大，无需均衡

    // 从负载最高的节点找最大的 Tablet
    uint32_t src_node = max_it->first;
    uint32_t dst_node = min_it->first;

    size_t target_idx = tablets_.size();
    uint64_t max_count = 0;
    for (size_t i = 0 ; i < tablets_.size() ; ++i) {
        if (tablets_[i].node_id == src_node && tablets_[i].key_count > max_count) {
            max_count = tablets_[i].key_count;
            target_idx = i;
        }
    }
    if (target_idx >= tablets_.size()) { return false; }

    // 执行迁移（仅更新元数据，真实数据搬迁待 LevelDB 物理隔离后实现）
    tablets_[target_idx].node_id = dst_node;
    epoch_++;

    std::cout << "[Proxy] Move Tablet " << tablets_[target_idx].id 
              << " from Node " << src_node << " to Node " << dst_node
              << " (load " << max_it->second << " vs " << min_it->second << ")" << std::endl;
    
    SaveMeta();
    return true;
}

void RaftNode::BalancerLoop() {
    using namespace std::chrono;
    while (balancer_running_) {
        std::this_thread::sleep_for(seconds(30));  // 每 30 秒检查一次
        if (!balancer_running_) { break; }

        // 1. 尝试分裂过大的 Tablet
        {
            std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
            size_t n = tablets_.size();
            for (size_t i = 0 ; i < n ; ++i) {
                if (tablets_[i].key_count >= SPLIT_THRESHOLD) {
                    lock.unlock();
                    TrySplitTablet(i);
                    lock.lock();
                    break;  // 每次循环只分裂一个 Tablet，避免一次性修改过多
                }
            }
        }

        // 2. 尝试负载均衡
        DoLoadBalance();
    }
}

// ========== DataNode 存储与统计 =====
bool RaftNode::DataNodeGetTabletStats(const std::string& start, const std::string& end,
                                      uint64_t& key_count, std::string& median_key)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 收集落在区间内的 key
    std::vector<std::string> keys;
    // 遍历 storage_（当前为 unordered_map，无范围索引，只能全量扫描）
    // 这是性能瓶颈，后续 LevelDB 物理隔离后可用 Iterator 优化
    for (const auto& [k, v] : storage_.GetAll()) {
        if (k >= start && (end.empty() || k < end)) {
            keys.push_back(k);
        }
    }

    key_count = keys.size();
    if (key_count == 0) {
        median_key = "";
        return true;
    }

    std::sort(keys.begin(), keys.end());
    median_key = keys[keys.size() / 2];  // 中位数

    return true;
}

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
        } 
    } else {
        // DataNode 处理数据操作
        if (opcode == BinaryProtocol::OP_PUT) {
            return HandleDataNodePut(payload);
        } else if (opcode == BinaryProtocol::OP_GET) {
            return HandleDataNodeGet(payload);
        } else if (opcode == BinaryProtocol::OP_DELETE) {
            return HandleDataNodeDelete(payload);
        } else if (opcode == BinaryProtocol::OP_TABLET_STATS) {
            return HandleDataNodeTabletStats(payload);
        } 
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
    if (!GetTabletRoute(key, t)) {
        return {BinaryProtocol::ST_NO_SHARD};
    }

    // 更新访问计数（用于分裂决策，近似值）
    {
        std::unique_lock<std::shared_mutex> lock(tablet_mutex_);
        for (auto& tablet : tablets_) {
            if (tablet.id == t.id) {
                tablet.key_count++;
                break;
            }
        }
    }

    std::string addr = GetNodeAddr(t.node_id);
    // 返回：Status(1) + ShardId(1) + Epoch(4) + RouteLen(2) + Route
    std::vector<uint8_t> result;
    result.push_back(BinaryProtocol::ST_OK);
    result.push_back(static_cast<uint8_t>(t.id & 0xFF));  // 简化：返回 TabletID 低8位作为 ShardId
    uint32_t ep = htonl(epoch_);
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 4);
    uint16_t rl = htons(addr.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&rl), reinterpret_cast<uint8_t*>(&rl) + 2);
    result.insert(result.end(), addr.begin(), addr.end());

    // BinaryRpcServer 会自动打包帧头，这里只要返回payload 部分
    return result;
}

std::vector<uint8_t> RaftNode::HandleProxyShards(uint32_t req_id) {
    std::shared_lock<std::shared_mutex> lock(tablet_mutex_);
    std::string body = std::to_string(tablets_.size()) + " ";
    bool first = true;
    for (const auto& t : tablets_) {
        if (!first) body += " ";
        body += std::to_string(t.id) + ":" + GetNodeAddr(t.node_id);
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

std::vector<uint8_t> RaftNode::HandleDataNodeTabletStats(const std::vector<uint8_t>& payload) {
    // PayLoad: [1B Opcode][2B start_len][start][2B end_len][end]
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

    // 返回 [1B Status][4B key_count][2B median_len][median_key]
    std::vector<uint8_t> result{BinaryProtocol::ST_OK};
    uint32_t kc = htonl(static_cast<uint32_t>(key_count));
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&kc), reinterpret_cast<uint8_t*>(&kc) + 4);
    uint16_t ml = htons(median_key.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&ml), reinterpret_cast<uint8_t*>(&ml) + 2);
    result.insert(result.end(), median_key.begin(), median_key.end());
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

    // 启动 ApplyLoop （DataNode）
    if (IsDataNode()) {
        apply_thread_ = std::thread(&RaftNode::ApplyLoop, this);
    }

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
