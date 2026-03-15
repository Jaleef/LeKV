#include <iostream>
#include <sstream>

#include "kv_server.h"
#include "rpc_server.h"

int main(int argc, char* argv[]) {
    KVServer kv(8001);

    kv.Start();

    return 0;
}
