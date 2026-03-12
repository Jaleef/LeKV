#include "rpc_client.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

RpcClient::RpcClient(std::string host, int port) {
    this->host = host;
    this->port = port;
}

std::string RpcClient::call(const std::string& request) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    connect(sock, (sockaddr*)&addr, sizeof(addr));

    send(sock, request.c_str(), request.size(), 0);

    char buffer[1024] = {0};

    read(sock, buffer, sizeof(buffer));

    close(sock);

    return std::string(buffer);
}
