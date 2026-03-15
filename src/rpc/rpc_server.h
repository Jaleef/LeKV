#ifndef RPC_SERVER_H_
#define RPC_SERVER_H_

#include <string>
#include <functional>
#include <unordered_map>

class RpcServer
{
public:
    using Handler = std::function<std::string(const std::string&)>;

    RpcServer(int port);

    void RegisterHandler(const std::string& cmd, Handler handler);
    
    void Start();

private:
    int port_;

    std::unordered_map<std::string, Handler> handlers_;
};

#endif // RPC_SERVER_H_
