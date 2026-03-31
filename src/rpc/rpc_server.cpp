#include "rpc_server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>


RpcServer::RpcServer(uint16_t port): port_(port) {}
RpcServer::~RpcServer() { Stop(); }

bool RpcServer::Start(Handler handler) {
    handler_ = handler;

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return false;
    }

    if (listen(listen_fd_, 5) < 0) {
        return false;
    }

    running_ = true;
    std::cout << "RPC Server listening on port " << port_ << std::endl;
    accept_thread_ = std::thread(&RpcServer::AcceptLoop, this);
    return true;
}

void RpcServer::Stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    // 判断线程对象是否代表一个可等待的执行线程
    if (accept_thread_.joinable()) {
        // 阻塞线程，等待被调用的线程对象关联的线程执行完毕
        accept_thread_.join();
    }
}

void RpcServer::AcceptLoop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) continue;

        // 单机版简单处理，每个连接一个线程
        std::thread([this, client_fd]() {
            HandleClient(client_fd);
        }).detach();
    }
}

void RpcServer::HandleClient(int fd) {
    char buf[1024];
    std::string line_buf;

    while (running_) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) break;
        buf[n] = '\0';
        line_buf += buf;

        // 处理完整行
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos + 1);
            line_buf.erase(0, pos + 1);

            auto cmd = TextProtocol::Parse(line);
            std::string resp;
            if (cmd) {
                resp = handler_(*cmd);
            } else {
                resp = TextProtocol::Err("BAD_CMD", "Invalid format");
            }

            send(fd, resp.c_str(), resp.size(), 0);
        }
    }

    close(fd);
}
