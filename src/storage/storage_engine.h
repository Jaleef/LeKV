#ifndef STORAGE_ENGINE_H_
#define STORAGE_ENGINE_H_

#include <string>
#include <mutex>
#include <optional>
#include <memory>
#include <functional>

namespace leveldb {
class DB;
}

class StorageEngine {
public:
    explicit StorageEngine(const std::string& db_path);
    ~StorageEngine();

    // 禁止拷贝
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    // 基础 KV 操作
    bool Put(const std::string& key, const std::string& value);
    std::optional<std::string> Get(const std::string& key);
    bool Delete(const std::string& key);

    // 范围查询：迭代[start, end)区间，key 天然有序
    void RangeQuery(const std::string& start, const std::string& end,
                    std::function<bool(const std::string& key, const std::string& value)> callback);
    
    // 范围统计：返回 key 数量 + 中位数 key
    bool RangeStats(const std::string& start, const std::string& end,
                    uint64_t& key_count, std::string& median_key);
private:
    std::unique_ptr<leveldb::DB> db_;
    std::mutex mutex_;
};

#endif
