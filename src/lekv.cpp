#include "raft_node.h"

#include <iostream>
#include <string>

#include <csignal>

// 全局标志，必须是静态存储期，以便在信号处理程序中访问
static std::atomic<bool> g_stop{false};

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage:./exe 或者 ./exe <port>(端口port从9001开始)" << std::endl;
        return 0;
    }

    // 注册信号处理函数
    std::signal(SIGINT, [](int signal) {
        if (signal == SIGINT) {
            std::cout << "Received SIGINT, shutting down..." << std::endl;
            g_stop.store(true); // 设置标志以通知主循环退出
        }
    });

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

    std::thread stop_thread([&node]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "Stopping LEKV..." << std::endl;
        node.Stop();
    });

    node.Run();

    if (stop_thread.joinable()) {
        stop_thread.join();
    }

    return 0;
}
