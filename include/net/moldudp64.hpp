#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "itch/parser.hpp"

// MoldUDP64, the framing Nasdaq uses to send ITCH over UDP multicast.
//
// Packet:  session (10 ASCII bytes) | sequence number of the first message (8) |
//          message count (2) | messages, each a 2-byte length and the bytes
//          count 0 = heartbeat, count 0xFFFF = end of session
// Request: session (10) | first sequence number wanted (8) | how many (2),
//          sent to the re-request server, which answers with normal packets.
namespace ts::mold {

inline constexpr size_t kHeaderBytes = 20;
inline constexpr uint16_t kEndOfSession = 0xFFFF;
inline constexpr size_t kMaxPacket = 1400;  // stays under a 1500-byte Ethernet frame

using Session = std::array<char, 10>;

struct Header {
    Session session{};
    uint64_t sequence = 0;
    uint16_t count = 0;
};

inline void put_u16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v >> 8);
    p[1] = static_cast<std::byte>(v);
}

inline void put_u64(std::byte* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>(v >> (56 - 8 * i));
}

inline void write_header(std::byte* p, const Header& h) {
    std::memcpy(p, h.session.data(), 10);
    put_u64(p + 10, h.sequence);
    put_u16(p + 18, h.count);
}

inline bool read_header(const std::byte* p, size_t n, Header& h) {
    if (n < kHeaderBytes) return false;
    std::memcpy(h.session.data(), p, 10);
    h.sequence = itch::load_u64(p + 10);
    h.count = itch::load_u16(p + 18);
    return true;
}

// Walks the messages of one packet; false at the end or if the packet is cut short.
class PacketReader {
public:
    PacketReader(const std::byte* data, size_t size) : data_(data), size_(size), pos_(kHeaderBytes) {}

    bool next(const std::byte*& message, uint16_t& len) {
        if (size_ - pos_ < 2) return false;
        len = itch::load_u16(data_ + pos_);
        if (len == 0 || size_ - pos_ - 2 < len) return false;
        message = data_ + pos_ + 2;
        pos_ += 2 + size_t{len};
        return true;
    }

private:
    const std::byte* data_;
    size_t size_;
    size_t pos_;
};

// Fills one packet buffer with length-prefixed messages.
class PacketBuilder {
public:
    explicit PacketBuilder(const Session& session) { header_.session = session; }

    void start(uint64_t sequence) {
        header_.sequence = sequence;
        header_.count = 0;
        used_ = kHeaderBytes;
    }

    // False when the message doesn't fit; the packet is then full.
    bool add(const std::byte* message, uint16_t len) {
        if (used_ + 2 + len > kMaxPacket || header_.count == kEndOfSession - 1) return false;
        put_u16(buf_.data() + used_, len);
        std::memcpy(buf_.data() + used_ + 2, message, len);
        used_ += 2 + size_t{len};
        ++header_.count;
        return true;
    }

    // The finished packet.
    const std::byte* data() {
        write_header(buf_.data(), header_);
        return buf_.data();
    }
    size_t size() const { return used_; }
    uint16_t count() const { return header_.count; }
    uint64_t sequence() const { return header_.sequence; }

private:
    Header header_;
    std::array<std::byte, kMaxPacket> buf_{};
    size_t used_ = kHeaderBytes;
};

inline Session make_session(const char* name) {
    Session s;
    s.fill(' ');
    for (size_t i = 0; i < s.size() && name[i] != '\0'; ++i) s[i] = name[i];
    return s;
}

}  // namespace ts::mold
