#include "rpc_client.h"

#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <thread>

void RpcClient::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RpcClient::Connect(const std::string& ip, uint16_t port) {
    Close();
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { return false; }

    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    
    struct timeval tv{5, 0};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return ::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0;
}

bool RpcClient::Send(const std::vector<uint8_t>& data) {
    if (fd_ < 0) return false;
    const uint8_t* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t n = ::send(fd_, p, left, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        if (n == 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

bool RpcClient::RecvAll(size_t total_needed, int timeout_ms) {
    if (fd_ < 0) return false;
    while (buf_.size() < total_needed) {
        char tmp[4096];
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf_.insert(buf_.end(), tmp, tmp + n);
        } else if (n == 0) {
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                timeout_ms -= 10;
                if (timeout_ms <= 0) return false;
                continue;
            }
            return false;
        }
    }

    return true;
}

bool RpcClient::RecvFrame(std::vector<uint8_t>& frame, int timeout_ms) {
    if (!RecvAll(4, timeout_ms)) return false;

    uint32_t frame_len = ntohl(*reinterpret_cast<uint32_t*>(buf_.data()));
    if (frame_len < 10) return false;   // 最小帧头 4 + 1 + 1 + 4 = 10

    if (!RecvAll(frame_len, timeout_ms)) return false;

    frame.assign(buf_.begin(), buf_.begin() + frame_len);
    buf_.erase(buf_.begin(), buf_.begin() + frame_len);
    return true;
}
