# rpc/CMakeLists.txt
add_library(rpc_client rpc_client.cpp)
target_include_directories(rpc_client
    PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(rpc_server rpc_server.cpp)
target_include_directories(rpc_server
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
