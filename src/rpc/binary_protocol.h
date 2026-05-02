#pragma once
#include <cstdint>
#include <vector>
#include <string>

#include <arpa/inet.h>

namespace lekv {

class BinaryProtocol {
public:
    static constexpr uint8_t MAGIC   = 0x4C;
    static constexpr uint8_t VERSION = 0x01;

    // Opcode
    static constexpr uint8_t OP_GET_ROUTE = 0x01;
    static constexpr uint8_t OP_GET       = 0x02;
    static constexpr uint8_t OP_PUT       = 0x03;
    static constexpr uint8_t OP_DELETE    = 0x04;
    static constexpr uint8_t OP_PING      = 0x05;
    static constexpr uint8_t OP_SHARDS    = 0x06;

    // Status
    static constexpr uint8_t ST_OK             = 0x00;
    static constexpr uint8_t ST_NOT_FOUND      = 0x01;
    static constexpr uint8_t ST_NOT_MY_SHARD   = 0x02;
    static constexpr uint8_t ST_BAD_REQUEST    = 0x03;
    static constexpr uint8_t ST_TIMEOUT        = 0x04;
    static constexpr uint8_t ST_NO_SHARD       = 0x05;
    static constexpr uint8_t ST_KEY_TOO_LONG   = 0x06;
    static constexpr uint8_t ST_VALUE_TOO_LONG = 0x07;
    
    //编码请求帧 （GET_ROUTE)
    static std::vector<uint8_t> EncodeGetRoute(uint32_t req_id, const std::string& key);

    //编码请求帧 （GET/PUT/DELETE)
    static std::vector<uint8_t> EncodeRequest(uint32_t req_id, uint8_t opcode, const std::string& key, const std::string& value = "");

    // 编码通用响应帧
    static std::vector<uint8_t> EncodeResponse(uint32_t req_id, uint8_t status, const std::string& value = "");

    // 编码路由查询响应（GET_ROUTE)
    static std::vector<uint8_t> EncodeRouteResponse(uint32_t req_id, uint8_t status, uint8_t shard_id = 0, uint32_t epoch, const std::string& route);

    // 从接收缓冲区尝试解码一帧
    // consumed: 成功时返回消费的字节数；失败时返回 0（数据不足）或 1（Magic 错误需跳过）
    // 返回 true 表示成功解码出一帧，out_payload 为纯 Payload（不含 Header）
    static bool TryDecode(const std::vector<uint8_t>& buf, size_t& consumed, uint32_t& out_req_id, std::vector<uint8_t>& out_payload);

};
}   // namespace lekv
