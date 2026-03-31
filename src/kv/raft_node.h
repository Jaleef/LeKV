#ifndef LEKV_RAFT_NODE_H_
#define LEKV_RAFT_NODE_H_

#include "rpc_server.h"
#include "storage_engine.h"

#include <atomic>
#include <string>

class RaftNode {
public:
    RaftNode(uint16_t port);

    // 启动服务(阻塞直到Stop)
    void Run();
    void Stop();

    // 后期多节点系统使用
    void SetLeader(bool leader) { is_leader_ = leader; }

private:
    // 命令路由
    std::string HandleCommand(const Command& cmd);

    std::string HandleExit(const std::string cmd);

    // 单机 KV 处理
    std::string HandlePut(const std::string& key, const std::string& value);
    std::string HandleGet(const std::string& key);
    std::string HandleDelete(const std::string& key);

    // 后期分布式 RPC 处理
    std::string HandleVoteRequest(const Command& cmd);
    std::string HandleAppendRequest(const Command& cmd);

    // 组件
    StorageEngine storage_;
    RpcServer rpc_server_;
    uint16_t port_;

    // Raft状态 (预留)
    std::atomic<bool> is_leader_{true};
    std::atomic<bool> running_{false};
};

#endif //LEKV_RAFT_NODE_H
