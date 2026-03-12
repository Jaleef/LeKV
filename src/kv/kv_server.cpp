#include "kv_server.h"

bool KVServer::put(const std::string& key, const std::string& value)
{
    return storage.put(key, value);
}

bool KVServer::get(const std::string& key, std::string& value)
{
    return storage.get(key, value);
}

bool KVServer::del(const std::string& key)
{
    return storage.del(key);
}
