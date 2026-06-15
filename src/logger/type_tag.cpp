#include "type_tag.h"

namespace lekv {

TypeTagInfo query_type_tag_info(TypeTag tag) noexcept {
    switch (tag) {
        // 1 字节
        case TypeTag::Int8:    return {1, false};
        case TypeTag::Uint8:   return {1, false};
        case TypeTag::Bool:    return {1, false};
        case TypeTag::Char:    return {1, false};

        // 2 字节
        case TypeTag::Int16:   return {2, false};
        case TypeTag::Uint16:  return {2, false};

        // 4 字节
        case TypeTag::Int32:   return {4, false};
        case TypeTag::Uint32:  return {4, false};
        case TypeTag::Float:   return {4, false};

        // 8 字节
        case TypeTag::Int64:   return {8, false};
        case TypeTag::Uint64:  return {8, false};
        case TypeTag::Double:  return {8, false};

        // 字符串（变长）
        case TypeTag::CString:
        case TypeTag::StringView:
        case TypeTag::StdString:
            return {0, true};

        default:
            return {0, false};
    }
}

} // namespace lekv
