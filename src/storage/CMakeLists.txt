# storage/CMakeLists.txt
add_library(storage
    STATIC
    storage_engine.cpp
)

target_include_directories(storage
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
