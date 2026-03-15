#include "rpc_server.h"

#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <thread>


RpcServer::RpcServer(int port): port_(port) {}

void RpcServer::RegisterHandler(const std::string& name, Handler handler) {
    handlers_[name] = handler;
}

void RpcServer::Start() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));

    listen(server_fd, 10);

    std::cout << "RPC Server listening on port " << port_ << std::endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        std::thread([this, client_fd]() {
            char buffer[1024] = {0};

            while (true) {
                memset(buffer, 0, sizeof(buffer));

                int n = recv(client_fd, buffer, sizeof(buffer), 0);

                if (n <= 0) {
                    std::cout << "连接关闭" << std::endl;
                    break;
                }

                std::string req(buffer, n);

                std::cout << req << std::endl;

                std::stringstream ss(req);

                std::string cmd;

                ss >> cmd;

                std::string resp = "UNKNOWN CMD";

                if (handlers_.count(cmd)) {
                    resp = handlers_[cmd](req);
                }

                send(client_fd, resp.c_str(), resp.size(), 0);
            }

            close(client_fd);

        }).detach();
    }
}