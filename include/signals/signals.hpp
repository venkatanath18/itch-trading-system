#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "book/book_types.hpp"

namespace ts {

// Order-flow imbalance of one top-of-book change (Cont, Kukanov & Stoikov 2014):
//   e_n = 1{Pb_n >= Pb_n-1} qb_n - 1{Pb_n <= Pb_n-1} qb_n-1
//       - 1{Pa_n <= Pa_n-1} qa_n + 1{Pa_n >= Pa_n-1} qa_n-1
// Positive: buying pressure (bids added or asks taken away).
inline int64_t ofi_contribution(const TopOfBook& prev, const TopOfBook& now) {
    int64_t e = 0;
    if (now.bid >= prev.bid) e += static_cast<int64_t>(now.bid_shares);
    if (now.bid <= prev.bid) e -= static_cast<int64_t>(prev.bid_shares);
    if (now.ask <= prev.ask) e -= static_cast<int64_t>(now.ask_shares);
    if (now.ask >= prev.ask) e += static_cast<int64_t>(prev.ask_shares);
    return e;
}

// A sum over a rolling time window, kept as B buckets of fixed width: the
// window is the current bucket and the B-1 before it. Constant time per
// update, fixed memory. Feed time only moves forward.
template <size_t B>
class RollingSum {
public:
    explicit RollingSum(Timestamp bucket_width) : width_(bucket_width) {}

    void add(Timestamp t, int64_t v) {
        advance(t);
        buckets_[current_ % B] += v;
        total_ += v;
    }

    int64_t sum(Timestamp t) {
        advance(t);
        return total_;
    }

    // The window sum as of time t, without moving the window.
    int64_t sum_at(Timestamp t) const {
        const uint64_t id = t / width_;
        if (id <= current_) return total_;
        if (id - current_ >= B) return 0;
        int64_t dropped = 0;
        for (uint64_t i = 1; i <= id - current_; ++i) dropped += buckets_[(current_ + i) % B];
        return total_ - dropped;
    }

    Timestamp bucket_width() const { return width_; }

private:
    void advance(Timestamp t) {
        const uint64_t id = t / width_;
        if (id <= current_) return;
        const uint64_t steps = std::min<uint64_t>(id - current_, B);
        for (uint64_t i = 1; i <= steps; ++i) {
            int64_t& b = buckets_[(current_ + i) % B];
            total_ -= b;
            b = 0;
        }
        current_ = id;
    }

    std::array<int64_t, B> buckets_{};
    int64_t total_ = 0;
    uint64_t current_ = 0;
    Timestamp width_;
};

struct StockSignals {
    TopOfBook top;               // after the latest change
    int64_t last_e = 0;          // OFI of the latest change (0 if a side was empty)
    int64_t ofi_total = 0;       // since the start of the day
    RollingSum<10> ofi_1s{kNanosPerSecond / 10};
    RollingSum<10> ofi_10s{kNanosPerSecond};
    Timestamp last_change = 0;
    uint64_t changes = 0;
    uint64_t last_mid2 = 0;      // twice the mid at the last two-sided, uncrossed top (0 = none yet)

    // twice the mid-price, so it stays an integer (only meaningful when two-sided)
    uint64_t mid2() const { return uint64_t{top.bid} + top.ask; }
};

// Best bid and ask, their sizes, the mid and OFI for every stock, updated on
// every change of the top of the book. Fixed memory, no allocation after
// construction.
class Signals {
public:
    explicit Signals(size_t stocks) : stocks_(stocks) {}

    // Call after a book message with the touched stock's top of book.
    // Returns true if the top changed.
    bool on_top(Locate locate, const TopOfBook& now, Timestamp ts) {
        StockSignals& s = stocks_[locate];
        if (now == s.top) return false;
        s.last_e = 0;
        if (now.two_sided() && s.top.two_sided()) {
            s.last_e = ofi_contribution(s.top, now);
            s.ofi_total += s.last_e;
            s.ofi_1s.add(ts, s.last_e);
            s.ofi_10s.add(ts, s.last_e);
        }
        s.top = now;
        if (now.two_sided() && now.bid < now.ask) s.last_mid2 = s.mid2();
        s.last_change = ts;
        ++s.changes;
        return true;
    }

    StockSignals& stock(Locate locate) { return stocks_[locate]; }
    const StockSignals& stock(Locate locate) const { return stocks_[locate]; }

private:
    std::vector<StockSignals> stocks_;
};

}  // namespace ts
