#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

#include "book/sorted_levels.hpp"

namespace ts {

// The price levels of one side as a flat array of W tick slots around the
// best price, with a bitmap to find the next occupied slot quickly. Prices
// outside the window (stub quotes, orders far from the market, prices off the
// tick grid) go to an overflow SortedLevels.
//
// Correctness never depends on where the window is: best() and for_each()
// merge the window with the overflow. Moving the window only changes speed.
// It moves when a new best price falls outside it, or when it empties while
// the overflow still has levels.
template <Side S, uint32_t W>
class TickLevelsW {
    static_assert(W % 64 == 0);

public:
    static constexpr const char* name = W == 1024 ? "tick array (1024 ticks)" : "tick array";

    TickLevelsW() : slots_(W, kNil), bits_(W / 64, 0) {}

    void reserve(size_t n) { overflow_.reserve(n); }

    static bool better(Price a, Price b) { return S == Side::Buy ? a > b : a < b; }

    uint32_t find(Price p) const {
        uint32_t s = 0;
        if (window_slot(p, s)) return slots_[s];
        return overflow_.find(p);
    }

    template <class Make>
    uint32_t find_or_insert(Price p, Make make) {
        uint32_t s = 0;
        if (!window_slot(p, s)) {
            const bool new_best = count_ == 0 || better(p, price_of(best_slot_));
            if (!new_best || !on_grid(p)) return overflow_.find_or_insert(p, make);
            if (overflow_.size() > 0 && !better(p, overflow_.best_price())) return overflow_.find_or_insert(p, make);
            recenter(p);
            if (!window_slot(p, s)) return overflow_.find_or_insert(p, make);  // the window is centred on p
        }
        if (slots_[s] != kNil) return slots_[s];
        const uint32_t level = make();
        if (level == kNil) return kNil;
        place(s, level);
        return level;
    }

    void erase(Price p) {
        uint32_t s = 0;
        if (window_slot(p, s) && slots_[s] != kNil) {
            remove_slot(s);
            if (count_ == 0 && overflow_.size() > 0) recenter(overflow_.best_price());
            return;
        }
        overflow_.erase(p);
    }

    uint32_t best() const {
        if (count_ == 0) return overflow_.best();
        if (overflow_.size() == 0 || better(price_of(best_slot_), overflow_.best_price())) return slots_[best_slot_];
        return overflow_.best();
    }

    // f(level) for up to n levels, best first: a merge of the window and the overflow
    template <class F>
    void for_each(size_t n, F f) const {
        int s = count_ ? static_cast<int>(best_slot_) : -1;
        size_t k = 0;
        for (size_t done = 0; done < n; ++done) {
            const bool have_window = s >= 0;
            const bool have_overflow = k < overflow_.size();
            if (!have_window && !have_overflow) return;
            if (have_window && (!have_overflow || better(price_of(static_cast<uint32_t>(s)), overflow_.price_from_best(k)))) {
                f(slots_[static_cast<uint32_t>(s)]);
                s = next_worse(s);
            } else {
                f(overflow_.level_from_best(k++));
            }
        }
    }

    size_t size() const { return count_ + overflow_.size(); }
    uint64_t growths() const { return overflow_.growths(); }
    uint64_t recenters() const { return recenters_; }

private:
    static bool on_grid(Price p) { return p < kPriceScale || p % 100 == 0; }

    Price price_of(uint32_t s) const { return base_ + s * step_; }

    bool window_slot(Price p, uint32_t& s) const {
        if (p < base_) return false;
        uint32_t d = p - base_;
        if (step_ != 1) {
            if (d % 100 != 0) return false;
            d /= 100;
        }
        if (d >= W) return false;
        s = d;
        return true;
    }

    // slot a holds a better price than slot b
    static bool slot_better(uint32_t a, uint32_t b) { return S == Side::Buy ? a > b : a < b; }

    void place(uint32_t s, uint32_t level) {
        slots_[s] = level;
        bits_[s / 64] |= uint64_t{1} << (s % 64);
        if (count_++ == 0 || slot_better(s, best_slot_)) best_slot_ = s;
    }

    void remove_slot(uint32_t s) {
        slots_[s] = kNil;
        bits_[s / 64] &= ~(uint64_t{1} << (s % 64));
        if (--count_ > 0 && s == best_slot_) best_slot_ = static_cast<uint32_t>(next_worse(static_cast<int>(s)));
    }

    // next occupied slot on the worse side of s, or -1
    int next_worse(int s) const { return S == Side::Buy ? highest_below(s) : lowest_above(s); }

    int highest_below(int s) const {
        int i = s - 1;
        while (i >= 0) {
            const int word = i / 64;
            const uint64_t bits = bits_[static_cast<size_t>(word)] & (~uint64_t{0} >> (63 - i % 64));
            if (bits) return word * 64 + 63 - std::countl_zero(bits);
            i = word * 64 - 1;
        }
        return -1;
    }

    int lowest_above(int s) const {
        int i = s + 1;
        while (i < static_cast<int>(W)) {
            const int word = i / 64;
            const uint64_t bits = bits_[static_cast<size_t>(word)] & (~uint64_t{0} << (i % 64));
            if (bits) return word * 64 + std::countr_zero(bits);
            i = (word + 1) * 64;
        }
        return -1;
    }

    // Centre the window on price p, moving levels between window and overflow.
    void recenter(Price p) {
        ++recenters_;
        struct Moved {
            Price price;
            uint32_t level;
        };
        static thread_local std::array<Moved, W> moved;
        size_t n = 0;
        for (uint32_t s = 0; s < W; ++s) {
            if (slots_[s] != kNil) {
                moved[n++] = Moved{price_of(s), slots_[s]};
                slots_[s] = kNil;
            }
        }
        for (auto& word : bits_) word = 0;
        count_ = 0;

        step_ = p >= kPriceScale ? 100 : 1;
        const int64_t base = int64_t{p} - int64_t{W / 2} * step_;
        base_ = base < 0 ? 0 : static_cast<Price>(base);

        uint32_t s = 0;
        for (size_t i = 0; i < n; ++i) {
            if (window_slot(moved[i].price, s)) place(s, moved[i].level);
            else overflow_.insert_level(moved[i].price, moved[i].level);
        }
        overflow_.extract_if([&](Price q, uint32_t level) {
            if (!window_slot(q, s)) return false;
            place(s, level);
            return true;
        });
    }

    std::vector<uint32_t> slots_;  // level index, or kNil
    std::vector<uint64_t> bits_;   // which slots are occupied
    Price base_ = 0;               // price of slot 0
    uint32_t step_ = 1;            // price units per slot: 100 at $1 and above, else 1
    uint32_t count_ = 0;           // occupied slots
    uint32_t best_slot_ = 0;       // valid when count_ > 0
    SortedLevelsT<S, 16> overflow_;
    uint64_t recenters_ = 0;
};

template <Side S>
using TickLevels = TickLevelsW<S, 1024>;

template <Side S>
using TickLevels256 = TickLevelsW<S, 256>;

template <Side S>
using TickLevels64 = TickLevelsW<S, 64>;  // a small window, so tests move it often

}  // namespace ts
