#include <iostream>

#include "storage_engine.h"
#include "kv_server.h"

int main(int argc, char* argv[]) {
    std::cout << "kv server启动" << std::endl;

    StorageEngine sto = StorageEngine();
    sto.put("key1", "value1");
    std::string value;
    sto.get("key1", value);
    
    KVServer server = KVServer();
    server.put("key2", "value2");
    server.get("key2", value);
    
    std::cout << "key1: " << value << std::endl;
    std::cout << "key2: " << value << std::endl;

    return 0;
}
