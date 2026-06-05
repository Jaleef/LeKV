# kv/CMakeLists.txt
add_library(raftnode
    raft_node.cpp
)

target_include_directories(raftnode
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nlohmann
)

target_link_libraries(raftnode
    PUBLIC
    storage
    rpc
)

