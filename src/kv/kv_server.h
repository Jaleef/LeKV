#ifndef KV_SERVER_H_
#define KV_SERVER_H_

#include "storage_engine.h"
#include "rpc_server.h"


class KVServer
{
public:
    KVServer(int port);

    void Start();

private:
    int port_;

    StorageEngine storage_;

    RpcServer server_;
};

#endif
