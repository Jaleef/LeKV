#ifndef KV_SERVER_H_
#define KV_SERVER_H_

#include <string>

#include "storage_engine.h"

class KVServer
{
public:

    bool put(const std::string& key, const std::string& value);

    bool get(const std::string& key, std::string& value);

    bool del(const std::string& key);

private:

    StorageEngine storage;
};

#endif
