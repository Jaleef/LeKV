#ifndef RPC_SERVER_H_
#define RPC_SERVER_H_

#include "text_protocol.h"
#include "binary_protocol.h"

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

using namespace lekv;

class RpcServer {
public:
    explicit RpcServer(uint16_t port);

    // 使用虚析构函数确保派生类的资源能够正确释放
    virtual ~RpcServer();

    void Stop();

protected:
    bool ServerStart();
    void AcceptLoop();

    // 强制子类实现
    virtual void HandleClient(int client_fd) = 0;

protected:
    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    //管理所有工作线程
    std::vector<std::thread> worker_threads_;
    
    // 防止并发加入线程
    std::mutex worker_mutex_;

    // 管理所有socket连接
    std::vector<int> client_fds_;

    // 防止并发修改client_fds_
    std::mutex client_mutex_;
};

using Handler = std::function<std::string(const Command& cmd)>;

class TextRpcServer: public RpcServer {
    public:
    explicit TextRpcServer(uint16_t port);
    
    ~TextRpcServer() override = default;
    
    bool Start(Handler handler);
    
    protected:
    void HandleClient(int client_fd) override;
    
    private:
    Handler handler_;
};

using BinaryHandler = std::function<std::vector<uint8_t>(uint32_t req_id, const std::vector<uint8_t>& payload)>;

class BinaryRpcServer: public RpcServer {
public:
    explicit BinaryRpcServer(uint16_t port);
    ~BinaryRpcServer() override = default;

    bool Start(BinaryHandler handler);

protected:
    void HandleClient(int client_fd) override;

    BinaryHandler binary_handler_;
};

#endif // RPC_SERVER_H_
