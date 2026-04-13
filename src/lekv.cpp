#include "raft_node.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage:./exe 或者 ./exe <port>(端口port从9001开始)" << std::endl;
        // return 0;
    }
    uint16_t node_id, port;
    if (argc == 1) {
        port = 9001;
        node_id = 1;
    } else {
        port = std::stoul(argv[1]);
        node_id = port - 9000;
    }

    std::vector<PeerInfo> peers;

    peers.emplace_back(1, "127.0.0.1", 9001);
    peers.emplace_back(2, "127.0.0.1", 9002);
    peers.emplace_back(3, "127.0.0.1", 9003);

    RaftNode node(node_id, port, peers);

    std::cout << "Starting LEKV..." << std::endl;
    std::cout << "Commands: PUT k v | GET k | DELETE k" << std::endl;

    node.Run();

    return 0;
}
