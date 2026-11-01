#pragma once

#include <cstdint>

namespace ts {

using Price = uint32_t;      // 4 implied decimals, as in ITCH: $12.3400 is 123400
using Qty = uint32_t;        // shares
using OrderRef = uint64_t;   // unique within the day
using Timestamp = uint64_t;  // nanoseconds since midnight
using Locate = uint16_t;     // stock locate code, the index of the stock's book

enum class Side : uint8_t { Buy = 0, Sell = 1 };

inline constexpr Price kPriceScale = 10000;  // $1.0000
inline constexpr Price kMaxPrice = 0xFFFFFFFFu;
inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;

// Reg NMS rule 612: quotes at $1.00 or more are in whole cents.
inline constexpr Price tick_size(Price price) { return price >= kPriceScale ? 100 : 1; }

inline constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

inline constexpr Timestamp hms(unsigned h, unsigned m, unsigned s) {
    return (Timestamp{h} * 3600 + Timestamp{m} * 60 + s) * kNanosPerSecond;
}

}  // namespace ts
