# src/storage/CMakeLists.txt

# LevelDB 源码路径 （src
set(LEVELDB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/leveldb)

# LevelDB 源文件（手动列出，排除不需要的）
set(LEVELDB_SOURCES
    ${LEVELDB_ROOT}/db/builder.cc
    ${LEVELDB_ROOT}/db/db_impl.cc
    ${LEVELDB_ROOT}/db/db_iter.cc
    ${LEVELDB_ROOT}/db/dbformat.cc
    ${LEVELDB_ROOT}/db/dumpfile.cc
    ${LEVELDB_ROOT}/db/filename.cc
    ${LEVELDB_ROOT}/db/log_reader.cc
    ${LEVELDB_ROOT}/db/log_writer.cc
    ${LEVELDB_ROOT}/db/memtable.cc
    ${LEVELDB_ROOT}/db/repair.cc
    ${LEVELDB_ROOT}/db/table_cache.cc
    ${LEVELDB_ROOT}/db/version_edit.cc
    ${LEVELDB_ROOT}/db/version_set.cc
    ${LEVELDB_ROOT}/db/write_batch.cc
    ${LEVELDB_ROOT}/table/block.cc
    ${LEVELDB_ROOT}/table/block_builder.cc
    ${LEVELDB_ROOT}/table/filter_block.cc
    ${LEVELDB_ROOT}/table/format.cc
    ${LEVELDB_ROOT}/table/iterator.cc
    ${LEVELDB_ROOT}/table/merger.cc
    ${LEVELDB_ROOT}/table/table.cc
    ${LEVELDB_ROOT}/table/table_builder.cc
    ${LEVELDB_ROOT}/table/two_level_iterator.cc
    ${LEVELDB_ROOT}/util/arena.cc
    ${LEVELDB_ROOT}/util/bloom.cc
    ${LEVELDB_ROOT}/util/cache.cc
    ${LEVELDB_ROOT}/util/coding.cc
    ${LEVELDB_ROOT}/util/comparator.cc
    ${LEVELDB_ROOT}/util/crc32c.cc
    ${LEVELDB_ROOT}/util/env.cc
    ${LEVELDB_ROOT}/util/env_posix.cc
    ${LEVELDB_ROOT}/util/filter_policy.cc
    ${LEVELDB_ROOT}/util/hash.cc
    ${LEVELDB_ROOT}/util/histogram.cc
    ${LEVELDB_ROOT}/util/logging.cc
    ${LEVELDB_ROOT}/util/options.cc
    ${LEVELDB_ROOT}/util/status.cc
)

# 编译 LevelDB 为静态库
add_library(leveldb
    STATIC
    ${LEVELDB_SOURCES}
)

target_include_directories(leveldb
    PUBLIC
        ${LEVELDB_ROOT}/include
    PRIVATE
        ${LEVELDB_ROOT}
)

target_compile_definitions(leveldb PRIVATE LEVELDB_PLATFORM_POSIX)

# LevelDB 依赖 pthread
find_package(Threads REQUIRED)
target_link_libraries(leveldb PUBLIC Threads::Threads)

add_library(storage
    STATIC
    storage_engine.cpp
)

target_include_directories(storage
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(storage
    PUBLIC
    leveldb
)
