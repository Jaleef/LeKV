# kv/CMakeLists.txt
add_library(raftnode
    raft_node.cpp
)

target_include_directories(raftnode
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(raftnode
    PUBLIC
    storage
    rpc
)

