#ifndef RPC_CLIENT_H_
#define RPC_CLIENT_H_

#include <string>

class RpcClient
{
public:
    RpcClient(std::string host, int port);

    std::string Call(const std::string& request);

private:
    std::string host_;

    int port_;
};

#endif //  RPC_CLIENT_H_
