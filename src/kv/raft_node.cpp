#include "raft_node.h"

#include <iostream>
#include <bits/this_thread_sleep.h>

#include "rpc/text_protocol.h"

RaftNode::RaftNode(uint16_t port): port_(port), rpc_server_(port) {}

void RaftNode::Run() {
    // 绑定命令处理器
    auto handler = [this](const Command& cmd) -> std::string {
        return this->HandleCommand(cmd);
    };

    if (!rpc_server_.Start(handler)) {
        std::cerr << "Failed to start server on port " << port_ << std::endl;
        return;
    }

    std::cout << "KV Server started on port " + port_ << std::endl;
    std::cout << "Test with telnet localhost " << port_ << std::endl;

    running_ = true;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void RaftNode::Stop() {
    running_ = false;
    rpc_server_.Stop();
}

std::string RaftNode::HandleCommand(const Command& cmd) {
    if (cmd.name == "PUT") {
        if (cmd.args.size() < 2) {
            return TextProtocol::Err("ARGS", "Usage: PUT <key> <value>");
        }
        return HandlePut(cmd.args[0], cmd.args[1]);
    } else if (cmd.name == "GET") {
        if (cmd.args.empty()) {
            return TextProtocol::Err("ARGS", "Usage: GET <key>");
        }
        return HandleGet(cmd.args[0]);
    } else if (cmd.name == "DELETE" || cmd.name == "DEL") {
        if (cmd.args.empty()) {
            return TextProtocol::Err("ARGS", "Usage: DELETE <key> or DEL <key>");
        }
        return HandleDelete(cmd.args[0]);
    } else if (cmd.name == "EXIT") {
        rpc_server_.Stop();
        Stop();
        return TextProtocol::Ok();
    } else if (cmd.name == "VOTE" || cmd.name == "APPEND") {
        return TextProtocol::Err("NOT_IMPL", "VOTE or APPEND does no implement");
    }
    return TextProtocol::Err("UNKNOWN", "Unknown command: " + cmd.name);
}

std::string RaftNode::HandlePut(const std::string& key, const std::string& value) {
    storage_.Put(key, value);
    return TextProtocol::Ok();
}

std::string RaftNode::HandleGet(const std::string& key) {
    auto val = storage_.Get(key);
    if (val.has_value()) {
        return TextProtocol::Ok(val.value());
    }
    return TextProtocol::Err("NOT_FOUND", "Key not found");
}

std::string RaftNode::HandleDelete(const std::string& key) {
    storage_.Delete(key);
    return TextProtocol::Ok();
}

std::string RaftNode::HandleVoteRequest(const Command& cmd) {
    // TODO: Phase 2 实现投票逻辑
    return TextProtocol::Err("NOT_IMPL", "Vote not implemented in single mode");
}

std::string RaftNode::HandleAppendRequest(const Command& cmd) {
    // TODO: Phase 2 实现日志复制
    return TextProtocol::Err("NOT_IMPL", "Append not implemented in single mode");
}