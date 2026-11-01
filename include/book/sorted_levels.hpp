#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "book/order_index.hpp"
#include "common/types.hpp"

namespace ts {

// The price levels of one side of one book, as a vector sorted so that the
// best price is at the back. Most activity is at or near the best price, so
// searches start from the back and inserts move only the few better levels.
// Each entry points at the level's slot in the book's level pool.
// Searches walk the Linear levels nearest the best one by one, then switch
// to binary search (Linear = max: walk all the way, the first version).
template <Side S, size_t Linear>
class SortedLevelsT {
public:
    static constexpr const char* name = Linear == 16 ? "sorted vector" : "sorted vector, linear search";

    void reserve(size_t n) { entries_.reserve(n); }

    // a is a better price than b for this side
    static bool better(Price a, Price b) { return S == Side::Buy ? a > b : a < b; }

    uint32_t find(Price p) const {
        const size_t i = position(p);
        return i > 0 && entries_[i - 1].price == p ? entries_[i - 1].level : kNil;
    }

    // The level at price p, creating it with make() if there is none.
    // make() returns kNil when the level pool is full.
    template <class Make>
    uint32_t find_or_insert(Price p, Make make) {
        const size_t i = position(p);
        if (i > 0 && entries_[i - 1].price == p) return entries_[i - 1].level;
        const uint32_t level = make();
        if (level == kNil) return kNil;
        if (entries_.size() == entries_.capacity()) ++growths_;
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), Entry{p, level});
        return level;
    }

    void erase(Price p) {
        const size_t i = position(p);
        if (i > 0 && entries_[i - 1].price == p) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }

    uint32_t best() const { return entries_.empty() ? kNil : entries_.back().level; }
    Price best_price() const { return entries_.back().price; }  // only when size() > 0

    // Put an existing level in its place (used when levels move between containers).
    void insert_level(Price p, uint32_t level) {
        const size_t i = position(p);
        if (entries_.size() == entries_.capacity()) ++growths_;
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), Entry{p, level});
    }

    // Remove the entries for which take(price, level) returns true, keeping the order.
    template <class Take>
    void extract_if(Take take) {
        size_t kept = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (!take(entries_[i].price, entries_[i].level)) entries_[kept++] = entries_[i];
        }
        entries_.resize(kept);
    }

    // The k-th best entry (k = 0 is the best).
    Price price_from_best(size_t k) const { return entries_[entries_.size() - 1 - k].price; }
    uint32_t level_from_best(size_t k) const { return entries_[entries_.size() - 1 - k].level; }

    // f(level) for up to n levels, best first
    template <class F>
    void for_each(size_t n, F f) const {
        for (size_t k = 0; k < n && k < entries_.size(); ++k) f(entries_[entries_.size() - 1 - k].level);
    }

    size_t size() const { return entries_.size(); }
    uint64_t growths() const { return growths_; }

private:
    struct Entry {
        Price price;
        uint32_t level;
    };

    // Index of the first entry better than p: everything before it is worse
    // than p or equal to it. A linear walk from the best end covers the levels
    // where most activity is; past them, binary search, so an order far from
    // the market (a stub quote thousands of levels deep) costs a dozen steps.
    size_t position(Price p) const {
        size_t i = entries_.size();
        const size_t stop = i > Linear ? i - Linear : 0;
        while (i > stop && better(entries_[i - 1].price, p)) --i;
        if (i > stop || stop == 0) return i;
        size_t lo = 0, hi = stop;  // the answer is in [0, stop]
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (better(entries_[mid].price, p)) hi = mid;
            else lo = mid + 1;
        }
        return lo;
    }
    std::vector<Entry> entries_;
    uint64_t growths_ = 0;
};

template <Side S>
using SortedLevels = SortedLevelsT<S, 16>;

template <Side S>
using LinearLevels = SortedLevelsT<S, std::numeric_limits<size_t>::max()>;

}  // namespace ts
