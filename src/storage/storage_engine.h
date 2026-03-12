#ifndef STORAGE_ENGINE_H_
#define STORAGE_ENGINE_H_

#include <string>
#include <unordered_map>
#include <mutex>

class StorageEngine
{
public:
    bool put(const std::string& key, const std::string& value);

    bool get(const std::string& key, std::string& value);

    bool del(const std::string& key);

private:
    std::unordered_map<std::string, std::string> kv_store;

    std::mutex mtx;
};

#endif
