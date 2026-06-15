#pragma once

#include "type_tag.h"

#include <cstddef>
#include <cstdint>

namespace lekv {

// ============================================================================
// 日志级别
// ============================================================================

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,  // 仅用于级别过滤，不产生实际日志
};

// ============================================================================
// 日志记录头部（固定大小，前端写入、后端读取）
// ============================================================================
// RecordHeader 之后紧跟 Payload，整体构成一条完整的 LogRecord。
//
// Record 总大小 = sizeof(RecordHeader) + payload_size
//
// Payload 布局（由 serializer 写入、backend 解析）：
//   [arg_count  : uint8_t              ]  参数个数 N
//   [type_tags  : TypeTag[N]           ]  每个参数的类型标记
//   [arg_data   : 按 type_tags 顺序排列 ]  标量直接写值，字符串先写长度再写数据

struct RecordHeader {
    uint64_t timestamp_ns;    // std::chrono::steady_clock 纳秒时间戳
    uint32_t thread_id;       // 线程标识（自增编号）
    uint32_t line;            // __LINE__
    const char* file;         // __FILE__ 字符串字面量指针
    const char* func;         // __FUNCTION__ 字符串字面量指针
    const char* format_str;   // 格式字符串字面量指针
    LogLevel level;
    uint16_t payload_size;    // Payload 部分的字节数
};

static_assert(sizeof(RecordHeader) == 48, "RecordHeader 大小预期为 48 字节（考虑对齐）");

// ============================================================================
// Payload 布局辅助（前后端共享的对齐约定）
// ============================================================================

// Payload 起始偏移（紧接在 Header 之后）
inline constexpr size_t payload_offset() {
    return sizeof(RecordHeader);
}

// 给定 RecordHeader 指针，获取 Payload 起始地址
template <typename T>
inline uint8_t* payload_ptr(T* header) {
    return reinterpret_cast<uint8_t*>(header) + sizeof(RecordHeader);
}

template <typename T>
inline const uint8_t* payload_ptr(const T* header) {
    return reinterpret_cast<const uint8_t*>(header) + sizeof(RecordHeader);
}

} // namespace lekv
