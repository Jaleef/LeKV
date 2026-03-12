#include "rpc_server.h"

#include <iostream>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>



RpcServer::RpcServer(int port) {
    this->port = port;
}

void RpcServer::register_handler(const std::string& cmd, std::function<std::string(const std::string&)> handler) {
    handlers[cmd] = handler;
}

void RpcServer::start() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));

    listen(server_fd, 10);

    std::cout << "RPC Server listening on port " << port << std::endl;

    while (true) {
        int client_sock = accept(server_fd, nullptr, nullptr);

        handle_client(client_sock);

        close(client_sock);
    }
}

void RpcServer::handle_client(int client_sock) {
    char buffer[1024] = {0};

    read(client_sock, buffer, sizeof(buffer));

    std::string request(buffer);

    std::stringstream ss(request);

    std::string cmd;

    ss >> cmd;

    if (handlers.count(cmd) == 0) {
        std::string resp = "UNKNOWN CMD\n";
    
        write(client_sock, resp.c_str(), resp.size());
    
        return;
    }

    std::string resp = handlers[cmd](request);

    write(client_sock, resp.c_str(), resp.size());
}
