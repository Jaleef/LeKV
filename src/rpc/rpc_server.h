#ifndef RPC_SERVER_H_
#define RPC_SERVER_H_

#include "text_protocol.h"
#include "binary_protocol.h"

#include <string>
#include <functional>
#include <atomic>
#include <thread>

using namespace lekv;

class RpcServer
{
public:
    using Handler = std::function<std::string(const Command& cmd)>;

    explicit RpcServer(uint16_t port);
    ~RpcServer();

    bool Start(Handler handler);

    void Stop();

private:
    void AcceptLoop();
    void HandleClient(int client_fd);

    uint16_t port_;
    int listen_fd_ = 1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    Handler handler_;
};

using BinaryHandler = std::function<std::vector<uint8_t>(uint32_t req_id, const std::vector<uint8_t>& payload)>;

class BinaryRpcServer: public RpcServer {
public:
    explicit BinaryRpcServer(uint16_t port);
    ~BinaryRpcServer();

    bool Start(BinaryHandler handler);

private:
    void AcceptLoop();
    void HandleClient(int client_fd);

    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    BinaryHandler handler_;
};

#endif // RPC_SERVER_H_
