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
    bool IsProxy() const { return port_ == LEADER_PORT; }
    bool IsDataNode() const { return !IsProxy(); }

    // 分片路由（Proxy专用）
    static constexpr uint32_t kShardCount = 2;  // 目前 2 个 DataNode（9002，9003）
    void BuildShardMap();                       // 根据 peers 自动构建 shard_map_
    uint32_t GetShardId(const std::string& key) const;
    RpcClient* GetShardClient(uint32_t shard_id);
    std::string ForwardToShard(uint32_t shard_id, const std::string& cmd);

    // DataNode 本地存储
    void ApplyLoop();       // DataNode 用于 WAL 恢复到 Storage
    void ApplyLogEntry(const LogEntry& entry);

    // 命令路由
    std::string HandleCommand(const Command& cmd);
    std::string HandleClientPut(const std::string& key, const std::string& value);
    std::string HandleClientGet(const std::string& key);
    std::string HandleAppendEntries(const Command& cmd);        // Follower 处理

    // Proxy 转发逻辑
    std::string ProxyPut(const std::string& key, const std::string& value);
    std::string ProxyGet(const std::string& key);
    std::string ProxyDelete(const std::string& key);

    // DataNode 本地处理逻辑
    std::string DataNodePut(const std::string& key, const std::string& value);
    std::string DataNodeGet(const std::string& key);
    std::string DataNodeDelete(const std::string& key);
    
    // 工具函数
    uint64_t GetLastLogIndex() const;
    bool EndsWith(const std::string& str, const std::string& suffix);
    std::string GetLeaderAddr() const { return "127.0.0.1:" + std::to_string(LEADER_PORT); }
    void PrintRole() const;

    // WAL 持久化操作
    bool InitWAL();                             // 初始化 WAL 文件
    void AppendToWAL(const LogEntry& entry);    // 追加单条到 WAL
    void RestoreFromWAL();                      // 从 WAL 恢复日志

    // 配置
    static constexpr uint16_t LEADER_PORT = 9001;
    uint64_t node_id_;
    uint16_t port_;
    std::vector<PeerInfo> peers_;

    // Proxy 组件
    RpcServer rpc_server_;
    std::map<uint32_t, PeerInfo> shard_map_;
    std::map<uint64_t, std::unique_ptr<RpcClient>> shard_clients_;
    
    // DataNode 本地处理逻辑
    StorageEngine storage_;
    std::vector<LogEntry> log_;     // 本地操作日志（仅用于 WAL 恢复，无 Raft 复制含义）
    std::ofstream wal_file_;         // WAL 文件句柄
    std::string wal_filename_;       // WAL 文件名
    std::mutex wal_mutex_;           // WAL 文件访问锁
    uint64_t wal_last_fsync_index_ = 0;                 // 上次刷盘的日志索引
    static constexpr uint64_t WAL_FSYNC_INTERVAL = 10;  // 每10条日志刷盘一次

    // 线程控制
    std::atomic<bool> running_{false};
    std::thread apply_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t last_applied_ = 0;
};

#endif //LEKV_RAFT_NODE_H
