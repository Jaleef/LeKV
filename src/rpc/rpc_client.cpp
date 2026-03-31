#include "rpc_client.h"

#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>

bool RpcClient::Connect(const std::string& ip, uint16_t port) {
    Close();

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { return false; }

    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 设置非阻塞 connect
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int ret = ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr));

    if (ret < 0 && errno == EINPROGRESS) {
        // 非阻塞 connect 建立中，等待 5 秒
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(fd_, &fdset);
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        ret = select(fd_ + 1, nullptr, &fdset, nullptr, &tv);
        if (ret > 0) {
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                Close();
                return false;
            }
        } else {
            Close();
            return false;
        }
    } else if (ret < 0) {
        Close();
        return false;
    }

    // 恢复阻塞模式
    fcntl(fd_, F_SETFL, flags);

    // 设置 Send 方法的读写超时
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return true;
}

std::string RpcClient::Send(const std::string& cmd, int timeout_ms) {
    if (fd_ < 0) { return ""; }

    // 确保命令以 \r\n 结尾
    std::string data = cmd;
    if (data.size() < 2 || data.substr(data.size() - 2) != "\r\n") {
        data += "\r\n";
    }

    // 设置本次调用的超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 发送数据
    ssize_t sent = ::send(fd_, data.c_str(), data.size(), 0);
    if (sent != static_cast<ssize_t>(data.size())) {
        // 发送失败, 连接可能已断开
        return "";
    }

    // 接收响应, 直到遇到 \n
    char buf[4096];
    std::string response;
    while (true) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            response += buf;
            // 检查是否收到完整行
            if (response.find('\n') != std::string::npos) {
                break;
            }
        } else if (n == 0) {
            // 对端关闭连接
            Close();
            return "";
        } else {
            // n < 0 检查错误
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 超时，返回已接收的数据
                break;
            }
            // 其他错误，关闭连接
            Close();
            return "";
        }
    }

    return response;
}

void RpcClient::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}