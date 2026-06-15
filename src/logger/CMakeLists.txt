# src/logger/CMakeLists.txt
add_library(logger 
    spsc_queue.cpp    
    type_tag.cpp
)

target_include_directories(logger
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(logger
    PUBLIC
    common
)
