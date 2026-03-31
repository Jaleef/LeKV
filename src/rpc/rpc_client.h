#ifndef RPC_CLIENT_H_
#define RPC_CLIENT_H_

#include <string>

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

private:
    int fd_ = -1;
};

#endif //  RPC_CLIENT_H_
