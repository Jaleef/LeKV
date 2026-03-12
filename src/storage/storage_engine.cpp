#include "storage_engine.h"

bool StorageEngine::put(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mtx);

    kv_store[key] = value;

    return true;
}

bool StorageEngine::get(const std::string& key, std::string& value)
{
    std::lock_guard<std::mutex> lock(mtx);

    auto it = kv_store.find(key);

    if (it == kv_store.end())
        return false;

    value = it->second;

    return true;
}

bool StorageEngine::del(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mtx);

    auto it = kv_store.find(key);

    if (it == kv_store.end())
        return false;

    kv_store.erase(it);

    return true;
}
