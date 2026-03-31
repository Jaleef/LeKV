#include "raft_node.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // 默认端口
    uint16_t port = 9001;

    // 使用 --port 指定端口
    for (int i = 1 ; i < argc ; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    RaftNode node(port);

    std::cout << "Starting LEKV..." << std::endl;
    std::cout << "Commands: PUT k v | GET k | DELETE k" << std::endl;

    node.Run();

    return 0;
}
