#ifndef RPC_CLIENT_H_
#define RPC_CLIENT_H_

#include <string>
#include <vector>


class RpcClient {
public:
    RpcClient() = default;
    ~RpcClient() { Close(); }
    
    // 连接到指定节点
    bool Connect(const std::string& ip, uint16_t port);

    // 发送原始字节
    bool Send(const std::vector<uint8_t>& data);

    // 接收一帧（先读 4B FrameLen，再读剩余）
    bool RecvFrame(std::vector<uint8_t>& frame, int timeout_ms = 5000);

    bool IsConnected() const { return fd_ >= 0; }

    // 暴露fd
    void AttachFd(int fd) { Close(); fd_ = fd; }

    void Close();

private:
    // 确保缓冲区有 total_needed 字节
    bool RecvAll(size_t total_needed, int timeout_ms);

    int fd_ = -1;

    std::vector<uint8_t> buf_;  // 接收缓冲区

};
#endif //  RPC_CLIENT_H_
