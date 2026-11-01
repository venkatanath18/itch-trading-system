#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace ts::itch {

// Builds ITCH 5.0 messages byte by byte, each with its 2-byte length prefix.
// For tests, scenarios and fuzz seeds; it allocates, so never on the hot path.
class Writer {
public:
    const std::vector<std::byte>& bytes() const { return buf_; }
    void clear() { buf_.clear(); }

    void system_event(Timestamp ts, char code) {
        begin('S', 0, ts, 12);
        ch(code);
    }

    void stock_directory(Locate locate, Timestamp ts, std::string_view symbol,
                         uint32_t round_lot = 100) {
        begin('R', locate, ts, 39);
        sym(symbol);
        ch('Q');  // market category
        ch('N');  // financial status
        u32(round_lot);
        ch('N');
        ch('C');
        ch('Z');
        ch(' ');
        ch('P');
        ch('N');
        ch('N');
        ch('1');
        ch('N');
        u32(0);
        ch('N');
    }

    void trading_action(Locate locate, Timestamp ts, std::string_view symbol, char state) {
        begin('H', locate, ts, 25);
        sym(symbol);
        ch(state);
        ch(' ');
        sym4("    ");
    }

    void add(Locate locate, Timestamp ts, OrderRef ref, Side side, Qty shares, Price price,
             std::string_view symbol = "TEST") {
        begin('A', locate, ts, 36);
        u64(ref);
        ch(side == Side::Buy ? 'B' : 'S');
        u32(shares);
        sym(symbol);
        u32(price);
    }

    void add_mpid(Locate locate, Timestamp ts, OrderRef ref, Side side, Qty shares, Price price,
                  std::string_view mpid, std::string_view symbol = "TEST") {
        begin('F', locate, ts, 40);
        u64(ref);
        ch(side == Side::Buy ? 'B' : 'S');
        u32(shares);
        sym(symbol);
        u32(price);
        sym4(mpid);
    }

    void executed(Locate locate, Timestamp ts, OrderRef ref, Qty shares, uint64_t match) {
        begin('E', locate, ts, 31);
        u64(ref);
        u32(shares);
        u64(match);
    }

    void executed_with_price(Locate locate, Timestamp ts, OrderRef ref, Qty shares,
                             uint64_t match, char printable, Price price) {
        begin('C', locate, ts, 36);
        u64(ref);
        u32(shares);
        u64(match);
        ch(printable);
        u32(price);
    }

    void cancel(Locate locate, Timestamp ts, OrderRef ref, Qty shares) {
        begin('X', locate, ts, 23);
        u64(ref);
        u32(shares);
    }

    void del(Locate locate, Timestamp ts, OrderRef ref) {
        begin('D', locate, ts, 19);
        u64(ref);
    }

    void replace(Locate locate, Timestamp ts, OrderRef old_ref, OrderRef new_ref, Qty shares,
                 Price price) {
        begin('U', locate, ts, 35);
        u64(old_ref);
        u64(new_ref);
        u32(shares);
        u32(price);
    }

    void trade(Locate locate, Timestamp ts, Qty shares, Price price, uint64_t match,
               std::string_view symbol = "TEST") {
        begin('P', locate, ts, 44);
        u64(0);
        ch('B');
        u32(shares);
        sym(symbol);
        u32(price);
        u64(match);
    }

    void cross_trade(Locate locate, Timestamp ts, uint64_t shares, Price price, uint64_t match,
                     char cross_type, std::string_view symbol = "TEST") {
        begin('Q', locate, ts, 40);
        u64(shares);
        sym(symbol);
        u32(price);
        u64(match);
        ch(cross_type);
    }

    // Any message as raw bytes after the header, e.g. a type we don't decode.
    void raw(char type, Locate locate, Timestamp ts, std::string_view body) {
        begin(type, locate, ts, static_cast<uint16_t>(11 + body.size()));
        for (char c : body) ch(c);
    }

private:
    void begin(char type, Locate locate, Timestamp ts, uint16_t len) {
        u16(len);
        ch(type);
        u16(locate);
        u16(0);  // tracking number
        u16(static_cast<uint16_t>(ts >> 32));
        u32(static_cast<uint32_t>(ts));
    }

    void ch(char c) { buf_.push_back(static_cast<std::byte>(c)); }
    void u16(uint16_t v) {
        for (int s = 8; s >= 0; s -= 8) buf_.push_back(static_cast<std::byte>((v >> s) & 0xFF));
    }
    void u32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) buf_.push_back(static_cast<std::byte>((v >> s) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int s = 56; s >= 0; s -= 8) buf_.push_back(static_cast<std::byte>((v >> s) & 0xFF));
    }
    void sym(std::string_view s) {
        for (size_t i = 0; i < 8; ++i) ch(i < s.size() ? s[i] : ' ');
    }
    void sym4(std::string_view s) {
        for (size_t i = 0; i < 4; ++i) ch(i < s.size() ? s[i] : ' ');
    }

    std::vector<std::byte> buf_;
};

}  // namespace ts::itch
