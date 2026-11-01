#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "common/types.hpp"

// Decoded Nasdaq TotalView-ITCH 5.0 messages. Every message starts with the
// same 11 bytes: type (1), stock locate (2), tracking number (2) and a 6-byte
// timestamp in nanoseconds since midnight. Offsets below are from the spec.
namespace ts::itch {

struct Symbol {
    std::array<char, 8> chars{};

    // The symbol without the trailing spaces ITCH pads it with.
    std::string_view view() const {
        size_t n = chars.size();
        while (n > 0 && chars[n - 1] == ' ') --n;
        return {chars.data(), n};
    }
};

struct Header {
    char type = 0;
    Locate locate = 0;
    uint16_t tracking = 0;
    Timestamp timestamp = 0;
};

// 'S', 12 bytes
struct SystemEvent {
    Header h;
    char event_code = 0;  // O S Q M E C, see session.hpp
};

// 'R', 39 bytes
struct StockDirectory {
    Header h;
    Symbol stock;
    char market_category = 0;   // Q, G, S = Nasdaq tiers; N = NYSE; A, P, Z, V = other exchanges
    char financial_status = 0;
    uint32_t round_lot_size = 0;
    char round_lots_only = 0;
    char issue_classification = 0;
    std::array<char, 2> issue_subtype{};
    char authenticity = 0;      // P = live, T = test
    char short_sale_threshold = 0;
    char ipo_flag = 0;
    char luld_tier = 0;
    char etp_flag = 0;
    uint32_t etp_leverage = 0;
    char inverse = 0;
};

// 'H', 25 bytes
struct TradingAction {
    Header h;
    Symbol stock;
    char state = 0;  // H halted, P paused, Q quotation only, T trading
    std::array<char, 4> reason{};
};

// 'A' (36 bytes) and 'F' (40 bytes, adds the market participant)
struct AddOrder {
    Header h;
    OrderRef ref = 0;
    Side side = Side::Buy;
    Qty shares = 0;
    Symbol stock;
    Price price = 0;
    std::array<char, 4> mpid{};  // spaces for 'A'
};

// 'E', 31 bytes: executed at the order's own price
struct OrderExecuted {
    Header h;
    OrderRef ref = 0;
    Qty shares = 0;
    uint64_t match = 0;
};

// 'C', 36 bytes: executed at a price that can differ from the order's
struct OrderExecutedWithPrice {
    Header h;
    OrderRef ref = 0;
    Qty shares = 0;
    uint64_t match = 0;
    char printable = 0;  // N = don't count in volume (it's in a cross print)
    Price price = 0;
};

// 'X', 23 bytes: part of an order cancelled
struct OrderCancel {
    Header h;
    OrderRef ref = 0;
    Qty shares = 0;  // shares removed, not shares left
};

// 'D', 19 bytes
struct OrderDelete {
    Header h;
    OrderRef ref = 0;
};

// 'U', 35 bytes: no side and no stock; both come from the original order
struct OrderReplace {
    Header h;
    OrderRef old_ref = 0;
    OrderRef new_ref = 0;
    Qty shares = 0;
    Price price = 0;
};

// 'P', 44 bytes: an execution against a hidden order
struct Trade {
    Header h;
    OrderRef ref = 0;  // zero since 2014
    char side = 0;     // always 'B' since 2014, so the side is unknown
    Qty shares = 0;
    Symbol stock;
    Price price = 0;
    uint64_t match = 0;
};

// 'Q', 40 bytes: the opening, closing, IPO or halt cross
struct CrossTrade {
    Header h;
    uint64_t shares = 0;
    Symbol stock;
    Price price = 0;
    uint64_t match = 0;
    char cross_type = 0;  // O opening, C closing, H halt/IPO, I intraday
};

// Length of each message type in bytes (without the 2-byte length prefix),
// or 0 for a type this spec version doesn't define.
constexpr uint16_t message_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

constexpr const char* message_name(char type) {
    switch (type) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order";
        case 'F': return "Add Order with MPID";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed with Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'N': return "Retail Price Improvement Indicator";
        case 'O': return "Direct Listing Price Discovery";
        default: return "unknown";
    }
}

}  // namespace ts::itch
