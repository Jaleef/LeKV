#include "storage_engine.h"

bool StorageEngine::Put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = value;
    return true;
}

std::optional<std::string> StorageEngine::Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);

    if (it != data_.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

bool StorageEngine::Delete(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(key);
    return true;
}
