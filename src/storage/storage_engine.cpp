#include "storage_engine.h"

bool StorageEngine::Put(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mtx_);

    kv_store_[key] = value;

    return true;
}

bool StorageEngine::Get(const std::string& key, std::string& value)
{
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = kv_store_.find(key);

    if (it == kv_store_.end())
        return false;

    value = it->second;

    return true;
}

bool StorageEngine::Del(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = kv_store_.find(key);

    if (it == kv_store_.end())
        return false;

    kv_store_.erase(it);

    return true;
}
