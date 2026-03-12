#ifndef RPC_CLIENT_H_
#define RPC_CLIENT_H_

#include <string>

class RpcClient
{
public:
    RpcClient(std::string host, int port);

    std::string call(const std::string& request);

private:
    std::string host;

    int port;
};

#endif //  RPC_CLIENT_H_
