#include "binary_protocol.h"

using namespace lekv;

std::vector<uint8_t> BinaryProtocol::EncodeGetRoute(uint32_t req_id, const std::string& key) {
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint32_t payload_len = 1 + 2 + key_len;
    uint32_t frame_len = 100 + payload_len;

    std::vector<uint8_t> buf;
    buf.reserve(frame_len);

    uint32_t fl = htonl(frame_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&fl), reinterpret_cast<uint8_t*>(&fl) + 4);
    buf.push_back(MAGIC);
    buf.push_back(VERSION);
    uint32_t rid = htonl(req_id);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rid), reinterpret_cast<uint8_t*>(&rid) + 4);

    buf.push_back(OP_GET_ROUTE);
    uint16_t kl = htons(key_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&kl), reinterpret_cast<uint8_t*>(&kl) + 2);
    buf.insert(buf.end(), key.begin(), key.end());
    return buf;
}

std::vector<uint8_t> BinaryProtocol::EncodeRouteResponse(uint32_t req_id, uint8_t status, uint8_t shard_id, uint32_t epoch, const std::string& route) {
    uint16_t route_len = static_cast<uint16_t>(route.size());
    uint32_t payload_len = 1 + 1 + 4 + 2 + route_len;
    uint32_t frame_len = 10 + payload_len;

    std::vector<uint8_t> buf;
    buf.reserve(frame_len);

    uint32_t fl = htonl(frame_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&fl), reinterpret_cast<uint8_t*>(&fl) + 4);
    buf.push_back(MAGIC);
    buf.push_back(VERSION);
    uint32_t rid = htonl(req_id);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rid), reinterpret_cast<uint8_t*>(&rid) + 4);

    buf.push_back(status);
    buf.push_back(shard_id);
    uint32_t ep = htonl(epoch);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&ep), reinterpret_cast<uint8_t*>(&ep) + 4);
    uint16_t rl = htons(route_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rl), reinterpret_cast<uint8_t*>(&rl) + 2);
    buf.insert(buf.end(), route.begin(), route.end());
    return buf;
}

std::vector<uint8_t> BinaryProtocol::EncodeRequest(uint32_t req_id, uint8_t opcode, const std::string& key, const std::string& value) {
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());
    uint32_t payload_len = 1 + 2 + 4 + key_len + val_len;
    uint32_t frame_len = 10 + payload_len;

    std::vector<uint8_t> buf;
    buf.reserve(frame_len);

    uint32_t fl = htonl(frame_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&fl), reinterpret_cast<uint8_t*>(&fl) + 4);
    buf.push_back(MAGIC);
    buf.push_back(VERSION);
    uint32_t rid = htonl(req_id);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rid), reinterpret_cast<uint8_t*>(&rid) + 4);

    buf.push_back(opcode);
    uint16_t kl = htons(key_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&kl), reinterpret_cast<uint8_t*>(&kl) + 2);
    uint32_t vl = htonl(val_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&vl), reinterpret_cast<uint8_t*>(&vl) + 4);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.insert(buf.end(), value.begin(), value.end());
    return buf;
}

std::vector<uint8_t> BinaryProtocol::EncodeResponse(uint32_t req_id, uint8_t status, const std::string& value) {
    uint32_t val_len = static_cast<uint32_t>(value.size());
    uint32_t payload_len = 1 + 4 + val_len;
    uint32_t frame_len = 10 + payload_len;

    std::vector<uint8_t> buf;
    buf.reserve(frame_len);

    uint32_t fl = htonl(frame_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&fl), reinterpret_cast<uint8_t*>(&fl) + 4);
    buf.push_back(MAGIC);
    buf.push_back(VERSION);
    uint32_t rid = htonl(req_id);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&rid), reinterpret_cast<uint8_t*>(&rid) + 4);

    buf.push_back(status);
    uint32_t vl = htonl(val_len);
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&vl), reinterpret_cast<uint8_t*>(&vl) + 4);
    buf.insert(buf.end(), value.begin(), value.end());
    return buf;
}

bool BinaryProtocol::TryDecode(const std::vector<uint8_t>& buf, size_t& consumed, uint32_t& out_req_id, std::vector<uint8_t>& out_payload) {
    consumed = 0;
    if (buf.size() < 10) return false;

    uint32_t frame_len = ntohl(*reinterpret_cast<const uint32_t*>(buf.data()));
    if (buf.size() < frame_len) return false;

    if (buf[4] != MAGIC) { 
        consumed = 1;
        return false;
    }
    if (buf[5] != VERSION) {
        consumed = 1;
        return false;
    }

    out_req_id = ntohl(*reinterpret_cast<const uint32_t*>(buf.data() + 6));
    size_t payload_len = frame_len - 10;
    out_payload.assign(buf.begin() + 10, buf.begin() + 10 + payload_len);
    consumed = frame_len;
    return true;
}
