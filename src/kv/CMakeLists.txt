# kv/CMakeLists.txt
add_library(kv_server
    kv_server.h
    kv_server.cpp
)

target_include_directories(kv_server
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
    ${CMAKE_SOURCE_DIR}/storage
)

target_link_libraries(kv_server
    PUBLIC
    storage
)

