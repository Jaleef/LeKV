# client/CMakeLists.txt
add_library(client
    kv_client.cpp
    kv_client.h
)

target_include_directories(client
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)


add_executable(client_app kv_client.cpp)

