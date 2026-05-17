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
#include <shared_mutex>

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

    // ========== 节点地址查询 ==========
    std::string GetNodeAddr(uint32_t node_id) const;

    // ========== Tablet 路由表 ==========
    mutable std::shared_mutex tablet_mutex_;    // 读写锁：读多写少
    std::vector<Tablet> tablets_;               // Tablet 列表，按照 start_key 排序
    uint64_t next_tablet_id_ = 1;               // Tablet ID 生成器
    uint64_t epoch_ = 0;                        // Tablet 配置版本号

    size_t FindTabletIndex(const std::string& key) const;  // 二分查找 Tablet 索引
    bool GetTabletRoute(const std::string& key, Tablet& out) const;

    // 初始化：加载 metadata 或创建初始 Tablet
    void InitTablets();
    void BuildInitialTablets();  // 根据 DataNode 数量创建初始区间
    void LoadMeta();
    void SaveMeta();

    // ========== 自动分裂与负载均衡 ==========
    void BalancerLoop();
    bool TrySplitTablet(size_t idx);                    // 尝试分裂指定 Tablet
    bool DoLoadBalance();                               // 执行负载均衡：统计负载，迁移 Tablet
    bool QueryTabletStats(uint32_t node_id, const std::string& start,
                          const std::string& end, uint64_t& key_count,
                          std::string& median_key);     // 向 DataNode 查询 Tablet 统计信息
    void SyncAllTabletStats();                        // 同步所有 Tablet 统计信息（分裂和负载均衡前调用）

    std::thread balancer_thread_;
    std::atomic<bool> balancer_running_{false};
    static constexpr uint64_t SPLIT_THRESHOLD = 10;     // 测试阈值：10 条记录后尝试分裂
    static constexpr double BALANCE_RATIO = 1.2;        // 负载均衡触发比率：1.2 倍

    // ========== DataNode 本地存储 ==========
    void ApplyLoop();       // DataNode 用于 WAL 恢复到 Storage
    void ApplyLogEntry(const LogEntry& entry);

    // ========== DataNode 统计接口（供 Proxy 查询） ==========
    bool DataNodeGetTabletStats(const std::string& start, const std::string& end,
                                uint64_t& key_count, std::string& median_key);

    // ========== 命令路由 ==========
    std::vector<uint8_t> HandleBinaryRequest(uint32_t req_id, const std::vector<uint8_t>& payload);

    // Proxy 转发逻辑
    std::vector<uint8_t> HandleProxyGetRoute(uint32_t req_id, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleProxyShards(uint32_t req_id);

    // DataNode 本地处理逻辑
    std::vector<uint8_t> HandleDataNodePut(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeGet(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeDelete(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeTabletStats(const std::vector<uint8_t>& payload);
    
    // 工具函数
    void PrintRole() const;
    
    // ========== WAL 持久化操作（DataNode 专用）==========
    bool InitWAL();                             // 初始化 WAL 文件
    void AppendToWAL(const LogEntry& entry);    // 追加单条到 WAL
    void RestoreFromWAL();                      // 从 WAL 恢复日志
    uint64_t GetLastLogIndex() const;

    // 配置
    static constexpr uint16_t LEADER_PORT = 9001;
    uint64_t node_id_;
    uint16_t port_;
    std::vector<PeerInfo> peers_;

    // ========== Proxy 组件 ==========
    std::unique_ptr<BinaryRpcServer> binary_server_;
    std::map<uint32_t, PeerInfo> shard_map_;
    std::map<uint32_t, std::unique_ptr<BinaryRpcClient>> node_clients_;
    
    // ========== DataNode 组件 ==========
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
