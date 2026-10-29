#include "packet.h"
#include <arpa/inet.h>
#include <cstring>

std::string encode_packet(PacketType type, const std::string& body) {
    std::string out;
    out.reserve(HEADER_SIZE + body.size());
    out.push_back(static_cast<char>(type));
    uint32_t len = htonl(static_cast<uint32_t>(body.size()));
    out.append(reinterpret_cast<const char*>(&len), 4);
    out += body;
    return out;
}

ParseResult try_decode(std::string& buf, Packet& out) {
    if (buf.size() < HEADER_SIZE) return ParseResult::NeedMore;
    uint32_t len;
    std::memcpy(&len, buf.data() + 1, 4);
    len = ntohl(len);
    if (len > MAX_BODY) return ParseResult::Error;
    if (buf.size() < HEADER_SIZE + len) return ParseResult::NeedMore;
    
    out.type = static_cast<PacketType>(static_cast<uint8_t>(buf[0]));
    out.body.assign(buf, HEADER_SIZE, len);
    buf.erase(0, HEADER_SIZE + len);
    return ParseResult::Ok;
}

