#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "itch/messages.hpp"

namespace ts::itch {

// ---- big-endian loads ------------------------------------------------------
// Each field is copied out of the buffer with memcpy and byte-swapped. Casting
// the buffer to a packed struct would make unaligned loads, break strict
// aliasing and still leave every field big-endian.

template <class T>
inline T load_be(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        if constexpr (sizeof(T) == 2) v = static_cast<T>(__builtin_bswap16(v));
        if constexpr (sizeof(T) == 4) v = __builtin_bswap32(v);
        if constexpr (sizeof(T) == 8) v = __builtin_bswap64(v);
    }
    return v;
}

inline uint16_t load_u16(const std::byte* p) { return load_be<uint16_t>(p); }
inline uint32_t load_u32(const std::byte* p) { return load_be<uint32_t>(p); }
inline uint64_t load_u64(const std::byte* p) { return load_be<uint64_t>(p); }
inline uint64_t load_u48(const std::byte* p) {
    return (uint64_t{load_u16(p)} << 32) | load_u32(p + 2);
}
inline char load_char(const std::byte* p) { return static_cast<char>(*p); }

template <size_t N>
inline std::array<char, N> load_chars(const std::byte* p) {
    std::array<char, N> a;
    std::memcpy(a.data(), p, N);
    return a;
}

inline Symbol load_symbol(const std::byte* p) { return Symbol{load_chars<8>(p)}; }

// ---- decoders: m points at the type byte, the length is already checked ----

inline Header decode_header(const std::byte* m) {
    return Header{load_char(m), load_u16(m + 1), load_u16(m + 3), load_u48(m + 5)};
}

inline SystemEvent decode_system_event(const std::byte* m) {
    return SystemEvent{decode_header(m), load_char(m + 11)};
}

inline StockDirectory decode_stock_directory(const std::byte* m) {
    StockDirectory r;
    r.h = decode_header(m);
    r.stock = load_symbol(m + 11);
    r.market_category = load_char(m + 19);
    r.financial_status = load_char(m + 20);
    r.round_lot_size = load_u32(m + 21);
    r.round_lots_only = load_char(m + 25);
    r.issue_classification = load_char(m + 26);
    r.issue_subtype = load_chars<2>(m + 27);
    r.authenticity = load_char(m + 29);
    r.short_sale_threshold = load_char(m + 30);
    r.ipo_flag = load_char(m + 31);
    r.luld_tier = load_char(m + 32);
    r.etp_flag = load_char(m + 33);
    r.etp_leverage = load_u32(m + 34);
    r.inverse = load_char(m + 38);
    return r;
}

inline TradingAction decode_trading_action(const std::byte* m) {
    return TradingAction{decode_header(m), load_symbol(m + 11), load_char(m + 19), load_chars<4>(m + 21)};
}

inline AddOrder decode_add(const std::byte* m, bool with_mpid) {
    AddOrder r;
    r.h = decode_header(m);
    r.ref = load_u64(m + 11);
    r.side = load_char(m + 19) == 'B' ? Side::Buy : Side::Sell;
    r.shares = load_u32(m + 20);
    r.stock = load_symbol(m + 24);
    r.price = load_u32(m + 32);
    r.mpid = with_mpid ? load_chars<4>(m + 36) : std::array<char, 4>{' ', ' ', ' ', ' '};
    return r;
}

inline OrderExecuted decode_executed(const std::byte* m) {
    return OrderExecuted{decode_header(m), load_u64(m + 11), load_u32(m + 19), load_u64(m + 23)};
}

inline OrderExecutedWithPrice decode_executed_with_price(const std::byte* m) {
    return OrderExecutedWithPrice{decode_header(m), load_u64(m + 11), load_u32(m + 19),
                                  load_u64(m + 23), load_char(m + 31), load_u32(m + 32)};
}

inline OrderCancel decode_cancel(const std::byte* m) {
    return OrderCancel{decode_header(m), load_u64(m + 11), load_u32(m + 19)};
}

inline OrderDelete decode_delete(const std::byte* m) {
    return OrderDelete{decode_header(m), load_u64(m + 11)};
}

inline OrderReplace decode_replace(const std::byte* m) {
    return OrderReplace{decode_header(m), load_u64(m + 11), load_u64(m + 19), load_u32(m + 27),
                        load_u32(m + 31)};
}

inline Trade decode_trade(const std::byte* m) {
    return Trade{decode_header(m), load_u64(m + 11), load_char(m + 19), load_u32(m + 20),
                 load_symbol(m + 24),   load_u32(m + 32), load_u64(m + 36)};
}

inline CrossTrade decode_cross_trade(const std::byte* m) {
    return CrossTrade{decode_header(m), load_u64(m + 11), load_symbol(m + 19), load_u32(m + 27),
                      load_u64(m + 31), load_char(m + 39)};
}

// ---- dispatch --------------------------------------------------------------

// Handlers inherit from this and hide the functions they care about. The
// calls are resolved at compile time: no virtual functions.
struct NullHandler {
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_trading_action(const TradingAction&) {}
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_with_price(const OrderExecutedWithPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_cross_trade(const CrossTrade&) {}
    // every other type, known or not, skipped by its length
    void on_other(char /*type*/, const std::byte* /*m*/, uint16_t /*len*/) {}
};

enum class Status : uint8_t {
    Ok,
    Truncated,  // the buffer ends inside a message
    BadLength,  // the length doesn't match the type, or is shorter than a header
    BadField,   // a field holds a value the spec doesn't allow
};

inline const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Truncated: return "truncated";
        case Status::BadLength: return "bad length";
        case Status::BadField: return "bad field";
    }
    return "?";
}

inline constexpr uint16_t kHeaderLength = 11;

// Decode one message (m points at its type byte, len bytes are readable)
// and call the handler.
template <class Handler>
inline Status dispatch(const std::byte* m, uint16_t len, Handler& h) {
    const char type = load_char(m);
    switch (type) {
        case 'A':
        case 'F': {
            const bool mpid = type == 'F';
            if (len != (mpid ? 40 : 36)) return Status::BadLength;
            const char side = load_char(m + 19);
            if (side != 'B' && side != 'S') return Status::BadField;
            h.on_add(decode_add(m, mpid));
            return Status::Ok;
        }
        case 'D':
            if (len != 19) return Status::BadLength;
            h.on_delete(decode_delete(m));
            return Status::Ok;
        case 'U':
            if (len != 35) return Status::BadLength;
            h.on_replace(decode_replace(m));
            return Status::Ok;
        case 'E':
            if (len != 31) return Status::BadLength;
            h.on_executed(decode_executed(m));
            return Status::Ok;
        case 'X':
            if (len != 23) return Status::BadLength;
            h.on_cancel(decode_cancel(m));
            return Status::Ok;
        case 'C':
            if (len != 36) return Status::BadLength;
            h.on_executed_with_price(decode_executed_with_price(m));
            return Status::Ok;
        case 'P':
            if (len != 44) return Status::BadLength;
            h.on_trade(decode_trade(m));
            return Status::Ok;
        case 'Q':
            if (len != 40) return Status::BadLength;
            h.on_cross_trade(decode_cross_trade(m));
            return Status::Ok;
        case 'S':
            if (len != 12) return Status::BadLength;
            h.on_system_event(decode_system_event(m));
            return Status::Ok;
        case 'R':
            if (len != 39) return Status::BadLength;
            h.on_stock_directory(decode_stock_directory(m));
            return Status::Ok;
        case 'H':
            if (len != 25) return Status::BadLength;
            h.on_trading_action(decode_trading_action(m));
            return Status::Ok;
        default: {
            const uint16_t expected = message_length(type);
            if (expected != 0 && len != expected) return Status::BadLength;
            h.on_other(type, m, len);
            return Status::Ok;
        }
    }
}

// One framed message: a pointer to its type byte and its length.
struct RawMessage {
    const std::byte* data = nullptr;
    uint16_t len = 0;

    char type() const { return load_char(data); }
    Locate locate() const { return load_u16(data + 1); }
    Timestamp timestamp() const { return load_u48(data + 5); }
};

// Walks a buffer of length-prefixed messages without copying them.
class Cursor {
public:
    Cursor(const std::byte* data, size_t size) : data_(data), size_(size) {}

    // The next message, or false at the end of the buffer or at a framing error.
    bool next(RawMessage& out) {
        if (pos_ == size_ || status_ != Status::Ok) return false;
        if (size_ - pos_ < 2) {
            status_ = Status::Truncated;
            return false;
        }
        const uint16_t len = load_u16(data_ + pos_);
        if (len < kHeaderLength) {
            status_ = Status::BadLength;
            return false;
        }
        if (size_ - pos_ - 2 < len) {
            status_ = Status::Truncated;
            return false;
        }
        out.data = data_ + pos_ + 2;
        out.len = len;
        pos_ += 2 + size_t{len};
        return true;
    }

    Status status() const { return status_; }
    size_t offset() const { return pos_; }
    void fail(Status s) { status_ = s; }

private:
    const std::byte* data_;
    size_t size_;
    size_t pos_ = 0;
    Status status_ = Status::Ok;
};

struct ParseResult {
    uint64_t messages = 0;
    size_t offset = 0;  // bytes consumed, or where the error is
    Status status = Status::Ok;
};

// Decode every message in the buffer, stopping at the first error.
template <class Handler>
inline ParseResult parse(const std::byte* data, size_t size, Handler& h) {
    Cursor cursor(data, size);
    RawMessage m;
    ParseResult r;
    while (cursor.next(m)) {
        const Status s = dispatch(m.data, m.len, h);
        if (s != Status::Ok) {
            r.status = s;
            r.offset = cursor.offset() - m.len - 2;
            return r;
        }
        ++r.messages;
    }
    r.status = cursor.status();
    r.offset = cursor.offset();
    return r;
}

}  // namespace ts::itch
