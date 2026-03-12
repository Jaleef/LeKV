# storage/CMakeLists.txt
add_library(storage
    storage_engine.h
    storage_engine.cpp
)

target_include_directories(storage
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
