#include "raft_node.h"
#include "common.h"

#include <iostream>

#include <csignal>

// 全局标志，必须是静态存储期，以便在信号处理程序中访问
static std::atomic<bool> g_stop{false};

void sigint_handler(int signal) {
    std::cout << "Received SIGINT, shutting down..." << std::endl;
    g_stop.store(true); // 设置标志以通知主循环退出
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage:./exe 或者 ./exe <port>(端口port从9001开始)" << std::endl;
        return 0;
    }

    if (std::signal(SIGINT, sigint_handler) == SIG_ERR) {
        std::cerr << "设置系统信号出错" << std::endl;
        return 1;
    }

    uint16_t port;
    uint64_t node_id;
    try {
        // 根据参数设置端口，Master 节点默认必须使用9001端口
        if (argc == 1) {
            port = 9001;
        } else {
            port = std::stoul(argv[1]);
        }
    } catch (const std::invalid_argument& e) {
        std::cerr << "无效的端口号: " << argv[1] << std::endl;
        return 1;
    } catch (const std::out_of_range& e) {
        std::cerr << "端口号超出范围: " << argv[1] << std::endl;
        return 1;
    }

    std::vector<PeerInfo> peers;
    try {
        PeerInfo master;
        std::vector<PeerInfo> followers;
        read_master_config(master);
        read_followers_config(followers);
        peers.push_back(master);
        peers.insert(peers.end(), followers.begin(), followers.end());
    
        node_id = find_self_node_id(peers, port);
    } catch (const json::parse_error& e) {
        std::cerr << "JSON 解析错误: " << e.what() << std::endl;
        return 1;
    } catch (const std::runtime_error& e) {
        std::cerr << "运行时错误: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        return 1;
    }

    RaftNode node(node_id, port, peers);

    std::cout << "Starting LEKV..." << std::endl;
    std::cout << "Commands: PUT k v | GET k | DELETE k" << std::endl;

    std::thread stop_thread([&node]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        node.Stop();
    });

    node.Run();

    if (stop_thread.joinable()) {
        stop_thread.join();
    }

    return 0;
}
