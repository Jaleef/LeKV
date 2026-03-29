#ifndef STORAGE_ENGINE_H_
#define STORAGE_ENGINE_H_

#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>

class StorageEngine {
public:
    StorageEngine() = default;
    ~StorageEngine() = default;

    // 禁止拷贝
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    // 基础 KV 操作
    bool Put(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key);
    bool Delete(const std::string& key);

private:
    std::unordered_map<std::string, std::string> data_;

    std::mutex mutex_;
};

#endif
