#ifndef RPC_SERVER_H_
#define RPC_SERVER_H_

#include <string>
#include <functional>
#include <unordered_map>

class RpcServer
{
public:
    RpcServer(int port);

    void register_handler(const std::string& cmd,
                          std::function<std::string(const std::string&)> handler);
    
    void start();

private:
    int port;

    std::unordered_map<std::string, std::function<std::string(const std::string&)>> handlers;

    void handle_client(int client_socket);
};

#endif // RPC_SERVER_H_
