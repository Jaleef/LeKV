# rpc/CMakeLists.txt
add_library(rpc
    text_protocol.cpp
    rpc_server.cpp
    rpc_client.cpp
)

target_include_directories(rpc
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
