#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const char* ip = "127.0.0.1";
    int port = 8001;

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        return -1;
    }

    std::cout << "Connected to server\n";

    while (true) {

        std::cout << "kv> ";

        std::string cmd;
        std::getline(std::cin, cmd);

        if (cmd == "quit" || cmd == "exit")
            break;

        send(sock, cmd.c_str(), cmd.size(), 0);

        char buffer[1024] = {0};

        int n = recv(sock, buffer, sizeof(buffer), 0);

        if (n <= 0) {
            std::cout << "server closed\n";
            break;
        }

        std::cout << buffer << std::endl;
    }

    close(sock);
    return 0;
}