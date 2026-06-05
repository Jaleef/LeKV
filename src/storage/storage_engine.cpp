#include "storage_engine.h"

#include <leveldb/db.h>
#include <iostream>
#include <vector>

StorageEngine::StorageEngine(const std::string& db_path) {
    leveldb::Options options;
    options.create_if_missing = true;

    leveldb::DB* raw = nullptr;
    leveldb::Status s = leveldb::DB::Open(options, db_path, &raw);
    if (!s.ok()) {
        throw std::runtime_error("LevelDB open failed: " + s.ToString());
    }
    db_.reset(raw);
}

StorageEngine::~StorageEngine() = default;

bool StorageEngine::Put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->Put(leveldb::WriteOptions(), key, value).ok();
}

std::optional<std::string> StorageEngine::Get(const std::string& key) {
    std::string value;
    leveldb::Status s = db_->Get(leveldb::ReadOptions(), key, &value);
    if (s.ok()) { return value; }
    return std::nullopt;
}

bool StorageEngine::Delete(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_->Delete(leveldb::WriteOptions(), key).ok();
}

void StorageEngine::RangeQuery(const std::string& start, const std::string& end,
                               std::function<bool(const std::string&, const std::string&)> cb) {
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(leveldb::ReadOptions()));
    for (it->Seek(start); it->Valid(); it->Next()) {
        std::string k = it->key().ToString();
        if (!end.empty() && k >= end) break;
        if (!cb(k, it->value().ToString())) break;
    }
}

bool StorageEngine::RangeStats(const std::string& start, const std::string& end,
                               uint64_t& key_count, std::string& median_key) {
    std::vector<std::string> keys;
    keys.reserve(1024);

    RangeQuery(start, end, [&](const std::string& k, const std::string&) {
        keys.push_back(k);
        return true;
    });

    key_count = keys.size();
    if (key_count == 0) {
        median_key.clear();
        return true;
    }
    median_key = keys[key_count / 2];
    return true;
}

bool StorageEngine::DeleteRange(const std::string& start, const std::string& end) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(leveldb::ReadOptions()));

    std::vector<std::string> keys_to_delete;
    keys_to_delete.reserve(1024);

    // 第一遍：收集所有需要删除的 key（避免在迭代时修改数据库）
    for (it->Seek(start); it->Valid(); it->Next()) {
        std::string k = it->key().ToString();
        if (!end.empty() && k >= end) break;
        keys_to_delete.push_back(k);
    }

    // 第二遍：批量删除
    leveldb::WriteOptions write_options;
    for (const auto& k : keys_to_delete) {
        db_->Delete(write_options, k);
    }

    return true;
}
