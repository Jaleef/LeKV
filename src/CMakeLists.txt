include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(storage)
add_subdirectory(kv)
add_subdirectory(rpc)

add_executable(lekv lekv.cpp)
target_link_libraries(lekv
    PRIVATE
    raftnode
)
