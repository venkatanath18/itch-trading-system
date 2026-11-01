#pragma once

#include <cmath>
#include <cstdint>

#include "common/types.hpp"

// Avellaneda & Stoikov (2008). Units: prices in dollars, inventory q in shares,
// time in seconds.
//   gamma  risk aversion, per dollar
//   sigma  volatility of the mid-price, dollars per square root of a second
//   k      how fast the arrival of market orders falls with distance from the
//          mid: rate(delta) = A exp(-k delta), k per dollar
namespace ts::as {

struct Params {
    double gamma = 0;
    double sigma = 0;
    double k = 0;
};

// r = s - q gamma sigma^2 (T - t)
inline double reservation_price(double mid, double q, double time_left, const Params& p) {
    return mid - q * p.gamma * p.sigma * p.sigma * time_left;
}

// delta_ask + delta_bid = gamma sigma^2 (T - t) + (2 / gamma) ln(1 + gamma / k)
inline double total_spread(double time_left, const Params& p) {
    return p.gamma * p.sigma * p.sigma * time_left + (2.0 / p.gamma) * std::log1p(p.gamma / p.k);
}

struct Quotes {
    Price bid = 0;  // 0 = no bid
    Price ask = 0;  // 0 = no ask
};

// r -/+ half the spread, rounded away from r to the tick (so rounding never
// narrows the spread), then kept off the other side of the market:
// bid <= best ask - tick and ask >= best bid + tick. Prices in ITCH units.
inline Quotes quotes(double reservation, double spread, Price best_bid, Price best_ask, Price tick) {
    const double units = static_cast<double>(kPriceScale);
    const double t = static_cast<double>(tick);
    const double bid_ticks = std::floor((reservation - spread / 2) * units / t + 1e-9);
    const double ask_ticks = std::ceil((reservation + spread / 2) * units / t - 1e-9);
    Quotes q;
    if (bid_ticks >= 1) {
        q.bid = static_cast<Price>(bid_ticks * t);
        if (best_ask > tick && q.bid > best_ask - tick) q.bid = best_ask - tick;
    }
    if (ask_ticks >= 1 && ask_ticks * t < 4e9) {
        q.ask = static_cast<Price>(ask_ticks * t);
        if (best_bid > 0 && q.ask < best_bid + tick) q.ask = best_bid + tick;
    }
    if (q.bid != 0 && q.ask != 0 && q.bid >= q.ask) q.bid = q.ask - tick;  // can't happen with spread > 0
    return q;
}

}  // namespace ts::as
