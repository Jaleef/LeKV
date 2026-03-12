#include "rpc_client.h"
#include <iostream>

int main()
{
    RpcClient client("127.0.0.1",9000);

    while (true) {
        std::string cmd;
        std::cout << "Enter command: ";
        std::getline(std::cin, cmd);
        if (cmd == "exit") {
            break;
        }
        std::cout << client.call(cmd + "\n");
    }
}
