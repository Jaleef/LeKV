#ifndef LEKV_RAFT_TYPES_H
#define LEKV_RAFT_TYPES_H
#include <cstdint>
#include <string>

struct LogEntry {
    uint64_t term;          // 任期号
    uint64_t index;         // 日志索引(从1开始)
    std::string command;    // 命令内容

    std::string Serialize() const {
        return std::to_string(term) + "|" + std::to_string(index) + "|" + command;
    }

    bool Deserialize(const std::string& str) {
        size_t p1 = str.find('|');
        size_t p2 = str.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) { return false; }
        term = std::stoull(str.substr(0, p1));
        index = std::stoull(str.substr(p1 + 1, p2 - p1 - 1));
        command = str.substr(p2 + 1);
        return true;
    }
};

struct PeerInfo {
    uint64_t id;
    std::string ip;
    uint16_t port;

    PeerInfo() = default;
    PeerInfo(uint64_t id, std::string ip, uint16_t port) : id(id), ip(ip), port(port) {}
};

enum RaftState {
    Leader,
    Follower,
    Candidate,
};
#endif //LEKV_RAFT_TYPES_H
