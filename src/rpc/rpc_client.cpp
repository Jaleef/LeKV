#include "rpc_client.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

RpcClient::RpcClient(std::string host, int port) {
    this->host_ = host;
    this->port_ = port;
}

std::string RpcClient::Call(const std::string& request) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        return "";
    }

    send(sock, request.c_str(), request.size(), 0);

    char buffer[1024] = {0};

    read(sock, buffer, sizeof(buffer));

    close(sock);

    return std::string(buffer);
}
