include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(storage)
add_subdirectory(kv)
add_subdirectory(rpc)
add_subdirectory(common)

add_executable(lekv lekv.cpp)
target_link_libraries(lekv
    PRIVATE
    raftnode
)

add_executable(lekv_cli lekv_cli.cpp)
target_link_libraries(lekv_cli
    PRIVATE
    rpc
)
