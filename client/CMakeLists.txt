cmake_minimum_required(VERSION 3.16)
project(ledb_client)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(../src/rpc rpc_client_build)
add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME}
    PRIVATE rpc_client
)

