#ifndef LEKV_RAFT_NODE_H_
#define LEKV_RAFT_NODE_H_

#include "rpc_server.h"
#include "storage_engine.h"
#include "raft_types.h"
#include "rpc_client.h"

#include <json.hpp>

#include <atomic>
#include <string>
#include <map>
#include <condition_variable>
#include <shared_mutex>

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

class RaftNode {
public:
    RaftNode(uint64_t node_id, uint16_t port, const std::vector<PeerInfo>& peers);
    ~RaftNode();

    // 启动服务(阻塞直到Stop)
    void Run();
    void Stop();

private:
    // 角色判断：固定 9001 为Leader
    bool IsMaster() const { return port_ == LEADER_PORT; }
    bool IsDataNode() const { return !IsMaster(); }

    // ========== 节点地址查询 ==========
    std::string GetNodeAddr(uint32_t node_id) const;

    // ========== Tablet 路由表 ==========
    std::vector<Tablet> tablets_;              // Tablet 列表，按照 start_key 排序
    uint64_t next_tablet_id_ = 3;              // 下一个 Tablet ID，初始为 3（假设初始有 2 个 Tablet 1, 2）
    uint64_t epoch_ = 1;                       // Tablet 版本号，每次调整后递增

    size_t FindTabletIndex(const std::string& key) const;    // 二分查找 Tablet 索引
    bool GetTabletRoute(const std::string& key, Tablet& out) const;
    void BuildInitialTablets();                              // 根据 DataNode 数量创建初始区间

    // ========== Tablet 持久化（JSON） ==========
    bool LoadTablets();                                      // 从 JSON 文件加载路由表
    void SaveTablets();                                      // 保存路由表到 JSON 文件（原子写入）
    static constexpr const char* TABLET_FILE = "tablets.json";

    // ========== 自动分裂与负载均衡 ==========
    void BalancerLoop();
    bool TrySplitTablet(size_t idx);
    bool DoLoadBalance();
    bool QueryTabletStats(uint32_t node_id, const std::string& start,
                          const std::string& end, uint64_t& key_count,
                          std::string& median_key);
    void SyncAllTabletStats();
    bool MigrateTablet(size_t idx, uint32_t dst_node);
    bool ScanAndTransferData(uint32_t src_node, uint32_t dst_node,
                             const std::string& start, const std::string& end);
    bool SendDeleteRange(uint32_t node_id, const std::string& start,
                         const std::string& end);


    std::thread balancer_thread_;
    std::atomic<bool> balancer_running_{false};
    static constexpr uint64_t SPLIT_THRESHOLD = 6;
    static constexpr uint64_t BALANCE_DIFF_THRESHOLD = 7;
    static constexpr uint64_t BALANCE_LOOP_INTERVAL_SEC = 5;

    // ========== DataNode 统计与扫描 ==========
    bool DataNodeGetTabletStats(const std::string& start, const std::string& end,
                                uint64_t& key_count, std::string& median_key);

    // ========== 命令路由 ==========
    std::vector<uint8_t> HandleBinaryRequest(uint32_t req_id, const std::vector<uint8_t>& payload);

    // Master 转发逻辑
    std::vector<uint8_t> HandleMasterGetRoute(uint32_t req_id, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleMasterShards(uint32_t req_id);

    // DataNode 本地处理逻辑
    std::vector<uint8_t> HandleDataNodePut(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeGet(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeDelete(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeTabletStats(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeScanRange(uint32_t req_id, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeDeleteRange(const std::vector<uint8_t>& payload);

    // 工具函数
    void PrintRole() const;
    
    // 配置
    static constexpr uint16_t LEADER_PORT = 9001;
    uint64_t node_id_;
    uint16_t port_;
    std::vector<PeerInfo> peers_;

    // ========== Master 组件 ==========
    std::unique_ptr<BinaryRpcServer> binary_server_;
    std::map<uint32_t, std::unique_ptr<RpcClient>> node_clients_;
    
    // ========== DataNode 组件 ==========
    std::unique_ptr<StorageEngine> storage_;

    // 线程控制
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;

    // 计算负载均衡进行数据迁移的时间
    uint64_t transfer_count_ = 0;
    uint64_t transfer_time_ms_ = 0;
};

#endif //LEKV_RAFT_NODE_H
