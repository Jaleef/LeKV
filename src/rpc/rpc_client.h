#ifndef RPC_CLIENT_H_
#define RPC_CLIENT_H_

#include <string>
#include <vector>

class RpcClient
{
public:
    RpcClient() = default;
    ~RpcClient() { Close(); }

    // 连接到指定节点，超时 5 秒
    // 返回true表示连接成功，false表示连接失败
    bool Connect(const std::string& ip, uint16_t port);

    // 同步发送命令，返回响应字符串
    // 自动添加\r\n, 等待知道收到 \n 或超时
    // 如果连接已断开, 返回空字符串
    // timeout_ms: 接收超时时间(毫秒)
    std::string Send(const std::string& cmd, int timeout_ms = 3000);

    // 关闭连接
    void Close();

    // 检查连接是否有效
    bool IsConnected() const { return fd_ >= 0; }

    int GetFd() const { return fd_; }

private:
    int fd_ = -1;
};

// RawClient 原始二进制 socket 
class BinaryRpcClient {
public:
    BinaryRpcClient() = default;
    ~BinaryRpcClient() { Close(); }
    
    // 连接到指定地址
    bool Connect(const std::string& ip, uint16_t port);

    // 发送原始字节
    bool Send(const std::vector<uint8_t>& data);

    // 接收一帧（先读 4B FrameLen，再读剩余）
    bool RecvFrame(std::vector<uint8_t>& frame, int timeout_ms = 5000);

    // 关闭连接
    void Close();

    // 状态检查
    bool IsConnected() const { return fd_ >= 0; }

    // 暴露fd
    void AttachFd(int fd) { Close(); fd_ = fd; }

    int Fd() const { return fd_; }

private:
    // 确保缓冲区有 total_needed 字节
    bool RecvAll(size_t total_needed, int timeout_ms);

    int fd_ = -1;
    std::vector<uint8_t> buf_;  // 接收缓冲区
};
#endif //  RPC_CLIENT_H_
