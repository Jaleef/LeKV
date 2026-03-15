#include "kv_server.h"

#include "sstream"

KVServer::KVServer(int port): port_(port), server_(port) {}

void KVServer::Start() {
    server_.RegisterHandler("PUT", [&](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd ;
        std::string key, value;

        ss >> cmd >> key >> value;

        storage_.Put(key, value);

        return std::string("OK\n");
    });

    server_.RegisterHandler("GET", [&](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd, key;

        ss >> cmd >> key;

        std::string value;
        if (storage_.Get(key, value)) {
            return value + "\n";
        }

        return std::string("NOT FOUND\n");
    });

    server_.RegisterHandler("DEL", [&](const std::string& req) {
        std::stringstream ss(req);

        std::string cmd, key;

        ss >> cmd >> key;

        if (storage_.Del(key)) {
            return std::string("OK\n");
        }

        return std::string("NOT FOUND\n");
    });

    server_.Start();
}