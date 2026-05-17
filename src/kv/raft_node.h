#ifndef LEKV_RAFT_NODE_H_
#define LEKV_RAFT_NODE_H_

#include "rpc_server.h"
#include "storage_engine.h"
#include "raft_types.h"
#include "rpc_client.h"

#include <atomic>
#include <string>
#include <map>
#include <condition_variable>

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
    std::vector<Tablet> tablets_;               // Tablet 列表，按照 start_key 排序
    size_t FindTabletIndex(const std::string& key) const;  // 二分查找 Tablet 索引
    bool GetTabletRoute(const std::string& key, Tablet& out) const;
    void BuildInitialTablets();  // 根据 DataNode 数量创建初始区间

    // ========== 命令路由 ==========
    std::vector<uint8_t> HandleBinaryRequest(uint32_t req_id, const std::vector<uint8_t>& payload);

    // Proxy 转发逻辑
    std::vector<uint8_t> HandleProxyGetRoute(uint32_t req_id, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleProxyShards(uint32_t req_id);

    // DataNode 本地处理逻辑
    std::vector<uint8_t> HandleDataNodePut(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeGet(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> HandleDataNodeDelete(const std::vector<uint8_t>& payload);
    
    // 工具函数
    void PrintRole() const;
    
    // 配置
    static constexpr uint16_t LEADER_PORT = 9001;
    uint64_t node_id_;
    uint16_t port_;
    std::vector<PeerInfo> peers_;

    // ========== Proxy 组件 ==========
    std::unique_ptr<BinaryRpcServer> binary_server_;
    std::map<uint32_t, std::unique_ptr<BinaryRpcClient>> node_clients_;
    
    // ========== DataNode 组件 ==========
    StorageEngine storage_;

    // 线程控制
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

#endif //LEKV_RAFT_NODE_H
