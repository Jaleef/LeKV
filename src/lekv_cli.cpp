#include "binary_protocol.h"
#include "rpc_client.h"

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>

using namespace lekv;

struct CachedTablet {
    uint64_t id;
    std::string start;
    std::string end;
    std::string addr;
};

class LekvCli {
public:
    bool Init(const std::string& proxy_addr);
    void Run();

private:
    bool Execute(const std::string& line);

    bool RefreshTablets();   // 从 Proxy 拉取全量路由表

    // 路由层：查缓存或问 Proxy
    bool DoGetRoute(const std::string& key, std::string& addr);

    // 数据层：直连 DataNode
    bool DoDataNodeRequest(const std::string& addr, uint8_t opcode, 
        const std::string& key, const std::string& value, 
        uint8_t& status, std::string& result);

    // 工具函数
    uint32_t NextReqId() { return req_id_++; }
    bool SendAndRecv(RpcClient& client, const std::vector<uint8_t>& req, 
        uint32_t& resp_req_id, std::vector<uint8_t>& payload);

    std::string proxy_ip_;
    uint16_t proxy_port_ = 0;
    RpcClient proxy_conn_;    // 与 Proxy 的长连接

    uint32_t req_id_ = 1;
    uint32_t shard_count_ = 0;
    std::vector<CachedTablet> local_tablets_; // 本地 Tablet tree 副本
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

    if (!RefreshTablets()) { return false; }

    std::cout << "[Client] Connect to proxy " << proxy_addr << ". Loaded " << local_tablets_.size() << " tablets." << std::endl;

    return true;
}

bool LekvCli::RefreshTablets() {
    if (!proxy_conn_.IsConnected()) {
        if (!proxy_conn_.Connect(proxy_ip_, proxy_port_)) { return false; }
    }

    uint32_t rid = NextReqId();
    auto req = BinaryProtocol::EncodeRequest(rid, BinaryProtocol::OP_SHARDS, "");

    uint32_t resp_req_id;
    std::vector<uint8_t> payload;
    if (!SendAndRecv(proxy_conn_, req, resp_req_id, payload)) {
        proxy_conn_.Close();
        if (!proxy_conn_.Connect(proxy_ip_, proxy_port_)) { return false; }
        if (!SendAndRecv(proxy_conn_, req, resp_req_id, payload)) { return false; }
    }

    if (resp_req_id != rid || payload.empty() || payload[0] != BinaryProtocol::ST_OK) { return false; }

    // 解析 body: "2 1001::m:127.0.0.1:9002 1002:m::127.0.0.1:9003"
    std::string body(payload.begin() + 5, payload.end());
    std::istringstream iss(body);
    size_t count;
    iss >> count;
    
    local_tablets_.clear();
    std::string segment;
    while (iss >> segment) {
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            size_t c = segment.find(':', pos);
            if (c == std::string::npos) {
                parts.push_back(segment.substr(pos));
                break;
            }
            parts.push_back(segment.substr(pos, c - pos));
            pos = c + 1;
        }
        if (parts.size() >= 4) {
            CachedTablet ct;
            ct.id = std::stoull(parts[0]);
            ct.start = parts[1];
            ct.end = parts[2];
            ct.addr = parts[3] + ":" + parts[4];
            local_tablets_.push_back(ct);
        }
    }
    
    std::sort(local_tablets_.begin(), local_tablets_.end(),
              [](const auto& a, const auto& b) { return a.start < b.start; });
    return !local_tablets_.empty();
}

// ========== 发送并接收一帧 ==========
bool LekvCli::SendAndRecv(RpcClient& client, const std::vector<uint8_t>& req, 
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
bool LekvCli::DoGetRoute(const std::string& key, std::string& addr) {
    if (local_tablets_.empty() && !RefreshTablets()) { return false; }

    // 二分查找（与 Proxy 相同逻辑）
    size_t left = 0, right = local_tablets_.size();
    while (left < right) {
        size_t mid =left + (right - left) / 2;
        if (local_tablets_[mid].start <= key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    if (left == 0) { return false; }
    const auto& t = local_tablets_[left - 1];
    if (!t.end.empty() && key >= t.end) { return false; }

    addr = t.addr;
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
    
    RpcClient dn;
    if (!dn.Connect(ip, port)) {
        status = BinaryProtocol::ST_TIMEOUT;
        result = "Connect to " + addr + " failed";
        return false;
    }

    uint32_t rid = NextReqId();
    auto req = BinaryProtocol::EncodeRequest(rid, opcode, key, value);

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
        std::cout << "[Client] NOT_MY_SHARD, refreshing tablets..." << std::endl;
        local_tablets_.clear();  // 强制下次刷新全量路由表
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
        std::cout << "Commands:\n  put <key> <value>\n  get <key>\n  delete <key>\n  tablets\n  exit / quit\n";
        return true;
    }
    if (cmd == "tablets") {
        for (const auto& t : local_tablets_) {
            std::cout << " Tablet " << t.id << " [" << t.start 
                      << ","  << t.end << ") -> " << t.addr << std::endl;
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
    std::string addr;
    if (!DoGetRoute(key, addr)) {
        std::cerr << "Failed to get route for key: " << key << std::endl;
        return false;
    }

    // RTT 2: 直连 DataNode 执行命令
    uint8_t status;
    std::string result;
    if (!DoDataNodeRequest(addr, opcode, key, value, status, result)) {
        std::cerr << "Error: " << result << std::endl;
        return false;
    }

    // 如果 NO_MY_SHARD，刷新缓存后重试一次
    if (status == BinaryProtocol::ST_NOT_MY_SHARD) {
        std::cout << "[Client] Route stale (NOT_MY_SHARD), refreshing..." << std::endl;
        if (!DoGetRoute(key, addr) || !DoDataNodeRequest(addr, opcode, key, value, status, result)) {
            std::cerr << "Error: retry failed" << key << std::endl;
            return false;
        }
    }

    switch (status) {
        case BinaryProtocol::ST_OK:
            if (cmd == "get") {
                std::cout << (result.empty() ? "(empty)" : result) << std::endl;
            } else {
                std::cout << "OK" << std::endl;
            }
            break;
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
