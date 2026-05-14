#include "binary_protocol.h"
#include "rpc_client.h"
#include "common/common.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>

using namespace lekv;

class LekvCli {
public:
    bool Init(const std::string& proxy_addr);
    void Run();

private:
    bool Execute(const std::string& line);

    // 路由层：查缓存或问 Proxy
    bool DoGetRoute(const std::string& key, uint8_t& shard_id, 
        uint32_t& epoch, std::string& addr);

    // 数据层：直连 DataNode
    bool DoDataNodeRequest(const std::string& addr, uint8_t opcode, 
        const std::string& key, const std::string& value, 
        uint8_t& status, std::string& result);

    // 工具函数
    uint32_t NextReqId() { return req_id_++; }
    uint32_t CalcShard(const std::string& key) const;
    bool SendAndRecv(BinaryRpcClient& client, const std::vector<uint8_t>& req, 
        uint32_t& resp_req_id, std::vector<uint8_t>& payload);

    std::string proxy_ip_;
    uint16_t proxy_port_ = 0;
    BinaryRpcClient proxy_conn_;    // 与 Proxy 的长连接

    uint32_t req_id_ = 1;
    uint32_t shard_count_ = 0;
    
    struct RouteInfo {
        std::string ip;
        uint16_t port;
        uint32_t epoch;
    };
    std::map<uint32_t, RouteInfo> route_cache_;
};

// ========== 初始化连接 Proxy 并拉取全局路由 ==========
bool LekvCli::Init(const std::string& proxy_addr) {
    // 解析 proxy_addr
    size_t colon = proxy_addr.find(':');
    if (colon == std::string::npos) {
        std::cerr << "Invalid proxy address，expect ip:port" << std::endl;
        return false;
    }
    proxy_ip_ = proxy_addr.substr(0, colon);
    proxy_port_ = static_cast<uint16_t>(std::stoi(proxy_addr.substr(colon + 1)));

    if (!proxy_conn_.Connect(proxy_ip_, proxy_port_)) {
        std::cerr << "Failed to connect to proxy at " << proxy_addr << std::endl;
        return false;
    }

    // 发送 SHARDS 请求拉取全量路由表
    uint32_t rid = NextReqId();
    auto req = BinaryProtocol::EncodeRequest(rid, BinaryProtocol::OP_SHARDS, "");
    if (!proxy_conn_.Send(req)) {
        std::cerr << "Failed to send SHARDS request to proxy" << std::endl;
        return false;
    }

    uint32_t resp_req_id;
    std::vector<uint8_t> payload;
    if (!SendAndRecv(proxy_conn_, req, resp_req_id, payload)) {
        std::cerr << "Failed to receive SHARDS response from proxy" << std::endl;
        return false;
    }
    if (resp_req_id != rid || payload.empty()) {
        std::cerr << "Invalid SHARDS response" << std::endl;
        return false;
    }

    uint8_t status = payload[0];
    if (status != BinaryProtocol::ST_OK) {
        std::cerr << "SHARDS failed, status=" << (int)status << std::endl;
        return false;
    }

    uint32_t route_len = ntohl(*reinterpret_cast<uint32_t*>(payload.data() + 1));

    // 解析路由表 "2 0:127.0.1:9002 1:127.0.1:9003"
    std::string body(payload.begin() + 5, payload.end());
    std::istringstream iss(body);
    iss >> shard_count_;
    std::string segment;
    while (iss >> segment) {
        size_t p1 = segment.find(':');
        size_t p2 = segment.find(':', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        uint32_t sid = static_cast<uint32_t>(std::stoul(segment.substr(0, p1)));
        std::string ip = segment.substr(p1 + 1, p2 - p1 - 1);
        uint16_t port = static_cast<uint16_t>(std::stoul(segment.substr(p2 + 1)));
        route_cache_[sid] = {ip, port, 0};

    }

    std::cout << "[Client] Connect to proxy " << proxy_addr << ". Loaded " << route_cache_.size() << " shards." << std::endl;
    return true;
}

uint32_t LekvCli::CalcShard(const std::string& key) const {
    return std::hash<std::string>{}(key) % shard_count_;
}

// ========== 发送并接收一帧 ==========
bool LekvCli::SendAndRecv(BinaryRpcClient& client, const std::vector<uint8_t>& req, 
    uint32_t& resp_req_id, std::vector<uint8_t>& payload) {
    if (!client.Send(req)) return false;

    std::vector<uint8_t> frame;
    if (!client.RecvFrame(frame, 5000)) return false;

    size_t consumed = 0;
    if (!BinaryProtocol::TryDecode(frame, consumed, resp_req_id, payload)) {
        std::cerr << "Failed to decode response frame" << std::endl;
        return false;
    }
    return true;
}

// ========== RTT 1: 获取路由（缓存优先）==========
bool LekvCli::DoGetRoute(const std::string& key, uint8_t& shard_id, 
    uint32_t& epoch, std::string& addr) {
    uint32_t sid = CalcShard(key);
    auto it = route_cache_.find(sid);
    if (it != route_cache_.end()) {
        shard_id = static_cast<uint8_t>(sid);
        epoch = it->second.epoch;
        addr = it->second.ip + ":" + std::to_string(it->second.port);
        return true;
    }

    // 缓存未命中，发送 GET_ROUTE 请求
    if (!proxy_conn_.IsConnected()) {
        if (!proxy_conn_.Connect(proxy_ip_, proxy_port_)) {
            std::cerr << "Failed to connect to proxy at " << proxy_ip_ << ":" << proxy_port_ << std::endl;
            return false;
        }
    }

    uint32_t rid = NextReqId();
    auto req = BinaryProtocol::EncodeGetRoute(rid, key);
    if (!proxy_conn_.Send(req)) return false;

    uint32_t resp_req_id;
    std::vector<uint8_t> payload;
    if (!SendAndRecv(proxy_conn_, req, resp_req_id, payload)) return false;
    if (resp_req_id != rid || payload.empty()) return false;

    uint8_t status = payload[0];
    if (status != BinaryProtocol::ST_OK) return false;
    if (payload.size() < 8) // [1B status] [1B shard_id] [4B epoch] [2B route_len]
        return false;
    
    shard_id = payload[1];
    epoch = ntohl(*reinterpret_cast<uint32_t*>(payload.data() + 2));
    uint16_t route_len = ntohs(*reinterpret_cast<uint16_t*>(payload.data() + 6));
    if (payload.size() < 8 + route_len) return false;

    addr.assign(payload.begin() + 8, payload.begin() + 8 + route_len);

    // 更新缓存
    size_t colon = addr.find(':');
    if (colon != std::string::npos) {
        route_cache_[sid] = {
            addr.substr(0, colon),
            static_cast<uint16_t>(std::stoi(addr.substr(colon + 1))),
            epoch
        };
    }
    return true;
}

// ========== RTT 2: 直连 DataNode 执行命令 ==========
bool LekvCli::DoDataNodeRequest(const std::string& addr, uint8_t opcode, 
    const std::string& key, const std::string& value, 
    uint8_t& status, std::string& result) {

    size_t colon = addr.find(':');
    if (colon == std::string::npos) {
        status = BinaryProtocol::ST_BAD_REQUEST;
        result = "Invalid addr format";
        return false;
    }
    std::string ip = addr.substr(0, colon);
    uint16_t port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
    
    BinaryRpcClient dn;
    if (!dn.Connect(ip, port)) {
        status = BinaryProtocol::ST_TIMEOUT;
        result = "Connect to " + addr + " failed";
        return false;
    }

    uint32_t rid = NextReqId();
    auto req = BinaryProtocol::EncodeRequest(rid, opcode, key, value);
    if (!dn.Send(req)) {
        status = BinaryProtocol::ST_TIMEOUT;
        result = "Send failed";
        dn.Close();
        return false;
    }

    uint32_t resp_req_id;
    std::vector<uint8_t> payload;
    if (!SendAndRecv(dn, req, resp_req_id, payload)) {
        status = BinaryProtocol::ST_TIMEOUT;
        result = "Recv timeout from " + addr;
        dn.Close();
        return false;
    }
    dn.Close();

    if (payload.empty()) {
        status = BinaryProtocol::ST_BAD_REQUEST;
        result = "Empty payload";
        return false;
    }

    status = payload[0];
    if (payload.size() >= 5) {
        uint32_t val_len = ntohl(*reinterpret_cast<uint32_t*>(payload.data() + 1));
        if (val_len > 0 && payload.size() >= 5 + val_len) {
            result.assign(payload.begin() + 5, payload.begin() + 5 + val_len);
        } else {
            result.clear();
        }
    } else {
        result.clear();
    }

    // 如果 DataNode 返回的 NOT_MY_SHARD，说明缓存过期，清除该 shard 的缓存
    if (status == BinaryProtocol::ST_NOT_MY_SHARD) {
        route_cache_.erase(CalcShard(key));
    }

    return true;
}

// ========== 交互式命令行 ==========
bool LekvCli::Execute(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return true;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (cmd == "exit" || cmd == "quit") {
        std::cout << "Bye!" << std::endl;
        exit(0);
    }
    if (cmd == "help") {
        std::cout << "Commands:\n  put <key> <value>\n  get <key>\n  delete <key>\n  shards   -- show cached routes\n  exit / quit\n";
        return true;
    }
    if (cmd == "shards") {
        std::cout << "Shard Count: " << shard_count_ << std::endl;
        for (const auto& kv : route_cache_) {
            const auto& info = kv.second;
            std::cout << "Shard " << kv.first << "->" << info.ip << ":" << info.port << " (epoch " << info.epoch << ")\n";
        }
        return true;
    }

    // 解析 key 和 value
    std::string key;
    iss >> key;
    if (key.empty()) {
        std::cerr << "Error: key is required" << std::endl;
        return false;
    }
    std::string value;
    if (cmd == "put") {
        std::getline(iss, value);
        size_t pos = value.find_first_not_of(' ');
        if (pos != std::string::npos) value = value.substr(pos);
    }

    uint8_t opcode = (cmd == "get") ? BinaryProtocol::OP_GET :
                     (cmd == "put") ? BinaryProtocol::OP_PUT :
                     (cmd == "delete" || cmd == "del") ? BinaryProtocol::OP_DELETE :
                     BinaryProtocol::OP_GET;
    
    // RTT 1: 获取路由 （查缓存或问 Proxy）
    uint8_t shard_id;
    uint32_t epoch;
    std::string addr;
    if (!DoGetRoute(key, shard_id, epoch, addr)) {
        std::cerr << "Failed to get route for key: " << key << std::endl;
        return false;
    }

    // RTT 2: 直连 DataNode 执行命令
    uint8_t status;
    std::string result;
    bool ok = DoDataNodeRequest(addr, opcode, key, value, status, result);

    // 如果 NO_MY_SHARD，刷新缓存后重试一次
    if (ok && status == BinaryProtocol::ST_NOT_MY_SHARD) {
        std::cout << "[Client] Route stale (NOT_MY_SHARD), refreshing..." << std::endl;
        if (!DoGetRoute(key, shard_id, epoch, addr)) {
            std::cerr << "Failed to refresh route for key: " << key << std::endl;
            return false;
        }
        ok = DoDataNodeRequest(addr, opcode, key, value, status, result);
    }

    if (!ok) {
        std::cerr << "Error: " << result << std::endl;  
        return false;
    }

    switch (status) {
        case BinaryProtocol::ST_OK:
            if (cmd == "get") {
                std::cout << (result.empty() ? "(empty)" : result) << std::endl;
            } else {
                std::cout << "OK" << std::endl;
            }
        case BinaryProtocol::ST_NOT_FOUND:
            std::cout << "Key not found" << std::endl;
            break;
        default:
            std::cerr << "Error: status = " << (int)status << (result.empty() ? "" : " " + result) << std::endl;
            break;
    }
    return true;
}

void LekvCli::Run() {
    std::cout << "Type 'help' for usage." << std::endl;
    std::string line;
    while (true) {
        std::cout << "lekv> ";
        if (!std::getline(std::cin, line)) break;
        if (!Execute(line)) {
        }
    }
}

// ========== 程序入口 ==========
int main(int argc, char** argv) {
    std::string proxy_addr = "127.0.0.1:9001";
    for (int i = 1 ; i < argc ; ++i) {
        std::string arg = argv[i];
        if (arg == "--proxy" && i + 1 < argc) proxy_addr = argv[++i];
    }

    LekvCli cli;
    if (!cli.Init(proxy_addr)) return 1;
    cli.Run();
    return 0;
}
