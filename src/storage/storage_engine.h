#ifndef STORAGE_ENGINE_H_
#define STORAGE_ENGINE_H_

#include <string>
#include <unordered_map>
#include <mutex>

class StorageEngine
{
public:
    bool Put(const std::string& key, const std::string& value);

    bool Get(const std::string& key, std::string& value);

    bool Del(const std::string& key);

private:
    std::unordered_map<std::string, std::string> kv_store_;

    std::mutex mtx_;
};

#endif
