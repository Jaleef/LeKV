#ifndef LEKV_RAFT_NODE_H_
#define LEKV_RAFT_NODE_H_

#include "rpc_server.h"
#include "storage_engine.h"
#include "raft_types.h"
#include "rpc_client.h"

#include <atomic>
#include <string>
#include <map>
#include <set>
#include <condition_variable>
#include <fstream>


class RaftNode {
public:
    RaftNode(uint64_t node_id, uint16_t port, const std::vector<PeerInfo>& peers);
    ~RaftNode();

    // 启动服务(阻塞直到Stop)
    void Run();
    void Stop();

private:
    // 角色判断：固定 9001 为Leader
    bool IsLeader() const { return port_ == LEADER_PORT; }

    // 核心线程
    void LeaderLoop();      // Leader: 发送心跳 / 复制日志
    void ApplyLoop();       // 所有节点: 应用已提交日志到状态机

    // 命令路由
    std::string HandleCommand(const Command& cmd);
    std::string HandleClientPut(const std::string& key, const std::string& value);
    std::string HandleClientGet(const std::string& key);
    std::string HandleAppendEntries(const Command& cmd);        // Follower 处理

    // 日志管理
    void ApplyLogEntry(const LogEntry& entry);
    void ReplicateLog(uint64_t peer_id, bool heartbeat);        // 向单个节点发送日志
    void AdvanceCommitIndex();                                  // 检查半数以上确认

    // 工具函数
    uint64_t GetLastLogIndex() const;
    uint64_t GetLastLogTerm() const;
    std::string GetLeaderAddr() const { return "127.0.0.1:" + std::to_string(LEADER_PORT); }
    void PrintRole() const;

    // WAL 持久化操作
    bool InitWAL();                             // 初始化 WAL 文件
    void AppendToWAL(const LogEntry& entry);    // 追加单条到 WAL
    void RestoreFromWAL();                      // 从 WAL 恢复日志
    void MaybeFsync();                          // 批量刷盘

    // 配置
    static constexpr uint16_t LEADER_PORT = 9001;
    uint64_t node_id_;
    uint16_t port_;
    std::vector<PeerInfo> peers_;
    size_t quorum_size_;        // 多数派阈值: (peers.size() / 2) + 1;

    // 组件
    StorageEngine storage_;
    RpcServer rpc_server_;
    std::map<uint64_t, std::unique_ptr<RpcClient>> peer_clients_;

    // Raft状态
    std::mutex mutex_;
    uint64_t current_term_ = 1;     // 固定从 1 开始，Leader固定，term简单递增
    std::vector<LogEntry> log_;     // 日志条目, index 从 1 开始, log_[0] 是占位符

    // 易失性状态(所有节点)
    uint64_t commit_index_ = 0;     // 已知的最高提交索引
    uint64_t last_applied_ = 0;     // 以应用到状态机的最高索引

    // Leader 特有状态 (仅 9001 使用)
    std::map<uint64_t, uint64_t> next_index_;       // 每个节点的下一个发送索引
    std::map<uint64_t, uint64_t> match_index_;      // 每个节点已复制的最高索引
    std::set<uint64_t> pending_commits_;            // 等待确认提交的日志索引集合

    // 线程控制
    std::atomic<bool> running_{false};
    std::thread leader_thread_;
    std::thread apply_thread_;
    std::condition_variable cv_;

    // WAL 成员变量
    std::ofstream wal_file_;         // WAL 文件句柄
    std::string wal_filename_;       // WAL 文件名
    std::mutex wal_mutex_;           // WAL 文件访问锁
    uint64_t wal_last_fsync_index_ = 0;                 // 上次刷盘的日志索引
    static constexpr uint64_t WAL_FSYNC_INTERVAL = 10;  // 每10条日志刷盘一次
};

#endif //LEKV_RAFT_NODE_H
