#include <iostream>
#include <sstream>

#include "kv_server.h"
#include "rpc_server.h"

int main(int argc, char* argv[]) {
    KVServer kv;

    RpcServer server(9000);

    server.register_handler("PUT", [&kv](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd, key, value;

        ss >> cmd >> key >> value;

        kv.put(key, value);

        return std::string("OK\n");
    });

    server.register_handler("GET", [&kv](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd, key;

        ss >> cmd >> key;

        std::string value;
        if (kv.get(key, value)) {
            return value + "\n";
        }

        return std::string("NOT FOUND\n");
    });

    server.register_handler("DEL", [&kv](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd, key;

        ss >> cmd >> key;

        if (kv.del(key)) {
            return std::string("OK\n");
        }

        return std::string("NOT FOUND\n");
    });

    server.start();

    return 0;
}
