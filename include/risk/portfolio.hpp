#pragma once

#include <cstdint>
#include <vector>

#include "sim/events.hpp"

namespace ts {

// Money is in ITCH price units ($0.0001) times shares.
struct Holding {
    int64_t position = 0;  // shares, negative when short
    int64_t cash = 0;
    uint64_t bought = 0;   // shares
    uint64_t sold = 0;
    uint64_t fills = 0;
    int64_t max_abs_position = 0;
};

// Positions and cash, from the fills the strategy has been told about.
class Portfolio {
public:
    explicit Portfolio(size_t stocks) : holdings_(stocks) {}

    void on_fill(const Response& r) {
        Holding& h = holdings_[r.locate];
        const int64_t qty = r.qty;
        const int64_t notional = qty * static_cast<int64_t>(r.price);
        if (r.side == Side::Buy) {
            h.position += qty;
            h.cash -= notional;
            h.bought += r.qty;
        } else {
            h.position -= qty;
            h.cash += notional;
            h.sold += r.qty;
        }
        ++h.fills;
        const int64_t abs_position = h.position < 0 ? -h.position : h.position;
        if (abs_position > h.max_abs_position) h.max_abs_position = abs_position;
    }

    const Holding& holding(Locate l) const { return holdings_[l]; }
    int64_t position(Locate l) const { return holdings_[l].position; }

private:
    std::vector<Holding> holdings_;
};

}  // namespace ts
