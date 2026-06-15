#pragma once

#include <cstdint>

namespace lekv {

// ============================================================================
// 类型标记枚举
// ============================================================================
// 每种序列化类型的唯一 ID，用于前后端协商类型信息。
// 标量类型在 payload 中直接写固定大小的二进制值；
// 字符串类型先写 4 字节长度，再写字节数据。

enum class TypeTag : uint16_t {
    // 标量整数（固定 1/2/4/8 字节）
    Int8 = 1,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,

    // 浮点类型
    Float,
    Double,

    // 其他标量
    Bool,
    Char,

    // 字符串类型
    CString,        // const char*, 以 \0 结尾的字面量或动态字符串
    StringView,     // std::string_view，仅引用，不保证生命周期
    StdString,      // std::string，深拷贝内容到队列

    // 预留扩展起始值
    UserDefined = 1000,

    // 哨兵
    Unknown = 0xFFFF
};

// ============================================================================
// 编译期类型 -> TypeTag 映射
// ============================================================================

template <typename T>
struct TypeTagOf {
    static constexpr TypeTag value = TypeTag::Unknown; // 默认未知类型
};

#define LOGGER_REGISTER_TYPE_TAG(CppType, Tag) \
    template <> \
    struct TypeTagOf<CppType> { \
        static constexpr TypeTag value = TypeTag::Tag; \
    }   
LOGGER_REGISTER_TYPE_TAG(int8_t,   Int8);
LOGGER_REGISTER_TYPE_TAG(uint8_t,  Uint8);
LOGGER_REGISTER_TYPE_TAG(int16_t,  Int16);
LOGGER_REGISTER_TYPE_TAG(uint16_t, Uint16);
LOGGER_REGISTER_TYPE_TAG(int32_t,  Int32);
LOGGER_REGISTER_TYPE_TAG(uint32_t, Uint32);
LOGGER_REGISTER_TYPE_TAG(int64_t,  Int64);
LOGGER_REGISTER_TYPE_TAG(uint64_t, Uint64);
LOGGER_REGISTER_TYPE_TAG(float,    Float);
LOGGER_REGISTER_TYPE_TAG(double,   Double);
LOGGER_REGISTER_TYPE_TAG(bool,     Bool);
LOGGER_REGISTER_TYPE_TAG(char,     Char);

#undef LOGGER_REGISTER_TYPE_TAG

// ============================================================================
// 编译期辅助
// ============================================================================

template <typename T>
inline constexpr TypeTag type_tag_v = TypeTagOf<std::remove_cvref_t<T>>::value;

// 判断一个类型是否为已注册的标量（不含字符串）
template <typename T>
inline constexpr bool is_scalar_tag_v =
    type_tag_v<T> != TypeTag::Unknown &&
    type_tag_v<T> != TypeTag::CString &&
    type_tag_v<T> != TypeTag::StringView &&
    type_tag_v<T> != TypeTag::StdString;

// ============================================================================
// 运行时 TypeTag -> 元信息（后端反序列化使用）
// ============================================================================

struct TypeTagInfo {
    uint16_t fixed_size;  // 标量类型为 sizeof，字符串/未知为 0
    bool     is_string;   // 是否为三种字符串类型之一
};

// 根据 TypeTag 查询类型信息。用于后端反序列化时知道每种类型占多少字节。
// 标量返回 fixed_size > 0；字符串返回 is_string = true，fixed_size = 0。
[[nodiscard]] TypeTagInfo query_type_tag_info(TypeTag tag) noexcept;

}   // namespace lekv
