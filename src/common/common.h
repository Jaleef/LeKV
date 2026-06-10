#pragma once

#include "json.hpp"

#include <fstream>

using json = nlohmann::json;

constexpr uint64_t MASTER_ID = 1;
constexpr uint16_t MASTER_PORT = 9001;
inline uint64_t FOLLOWER_ID_START = 2;

struct PeerInfo {
    uint64_t id;
    std::string ip;
    uint16_t port;

    PeerInfo() = default;
    PeerInfo(uint64_t id, std::string ip, uint16_t port) : id(id), ip(ip), port(port) {}
};

inline void read_master_config(PeerInfo &master_info) {
    std::ifstream file("../../src/config.json");
    if (!file.is_open())
    {
        throw std::runtime_error("配置文件 config.json 打开失败");
    }
    json j = json::parse(file);

    std::string master_ip = j["MASTER_CONFIG"]["IP"];
    uint16_t master_port = j["MASTER_CONFIG"]["PORT"];
    if (master_port != MASTER_PORT) {
        throw std::runtime_error("Master端口必须为 " + std::to_string(MASTER_PORT));
    }

    master_info = PeerInfo(MASTER_ID, master_ip, master_port);
}

inline void read_followers_config(std::vector<PeerInfo> &followers) {
    std::ifstream file("../../src/config.json");
    if (!file.is_open()) {
        throw std::runtime_error("配置文件 config.json 打开失败");
    }
    json j = json::parse(file);

    for (const auto &follower : j["FOLLOWER_CONFIG"]) {
        std::string ip = follower["IP"];
        uint16_t port = follower["PORT"];
        if (port == MASTER_PORT) {
            throw std::runtime_error("Follower端口不能与Master默认端口相同");
        }
        followers.emplace_back(FOLLOWER_ID_START++, ip, port);
    }
}

inline uint64_t find_self_node_id(const std::vector<PeerInfo> &peers, const uint16_t &self_port) {
    for (const auto &peer : peers) {
        if (peer.port == self_port) {
            return peer.id;
        }
    }
    throw std::runtime_error("未找到匹配的节点ID");
}

// Tablet 定义
struct Tablet {
    uint64_t id;
    std::string start_key;  // 起始键（包含）
    std::string end_key;    // 结束键（不包含）
    uint32_t node_id;       // 负责该 Tablet 的节点 ID;
    uint64_t key_count;     // Tablet 中的键值对数量，用于负载均衡和迁移决策
};

// Tablet JSON 序列化（nlohmann/json ADL 自动匹配）
inline void to_json(nlohmann::json& j, const Tablet& t) {
    j = nlohmann::json{{"id", t.id}, {"start_key", t.start_key},
                       {"end_key", t.end_key}, {"node_id", t.node_id},
                       {"key_count", t.key_count}};
}

inline void from_json(const nlohmann::json& j, Tablet& t) {
    j.at("id").get_to(t.id);
    j.at("start_key").get_to(t.start_key);
    j.at("end_key").get_to(t.end_key);
    j.at("node_id").get_to(t.node_id);
    j.at("key_count").get_to(t.key_count);
}
