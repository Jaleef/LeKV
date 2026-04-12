#include "raft_node.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 7) {
        std::cerr << "Usage: --id <id> --port <port> --peers <peers>" << std::endl;
        // return 0;
    }
    uint16_t node_id, port;
    std::vector<PeerInfo> peers;
    /*
    for (int i = 1 ; i < argc ; ++i) {
        if (argv[i] == "--id" && i + 1 < argc) {
            node_id = std::stoul(argv[i + 1]);
        } else if (argv[i] == "--port" && i + 1 < argc) {
            port = std::stoul(argv[i + 1]);
        } else if (argv[i] == "--peers" && i + 1 < argc) {
            std::vector<std::string> parts;
            std::stringstream ss(argv[i + 1]);
            std::string item;
            while (std::getline(ss, item, ',')) {
                parts.push_back(item);
            }

            for (const auto& part : parts) {
                ss.clear();
                ss << part;
                uint64_t peer_id;
                std::string peer_ip;
                uint16_t peer_port;

                std::getline(ss, item, ':');
                peer_id = std::stoull(item);
                std::getline(ss, item, ':');
                peer_ip = item;
                std::getline(ss, item, ':');
                peer_port = std::stoul(item);

                peers.emplace_back(peer_id, peer_ip, peer_port);
            }
        }
    }
    */
    node_id = 1;
    port = 9001;
    peers.emplace_back(1, "127.0.0.1", 9001);
    peers.emplace_back(2, "127.0.0.1", 9002);
    peers.emplace_back(3, "127.0.0.1", 9003);

    RaftNode node(node_id, port, peers);

    std::cout << "Starting LEKV..." << std::endl;
    std::cout << "Commands: PUT k v | GET k | DELETE k" << std::endl;

    node.Run();

    return 0;
}
