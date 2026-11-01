#pragma once

#include <bit>
#include <cstdint>
#include <vector>

#include "common/types.hpp"

namespace ts {

inline constexpr uint32_t kNil = 0xFFFFFFFFu;

// Order reference number -> index in the order pool.
// Two designs with the same interface; the measurements in results/book.md
// decide between them.

// Open addressing with linear probing, sized up front to at most half full.
// Reference number 0 marks an empty slot (ITCH never gives an order ref 0).
// Deletion shifts the rest of the cluster back instead of leaving tombstones,
// so probe lengths don't grow over the day.
// Spread = true: Fibonacci hashing scatters the keys over the table.
// Spread = false: slot = ref mod size, so refs that arrive together sit
// together in memory, like in the flat array, at the risk of long clusters.
template <bool Spread>
class HashIndexT {
public:
    static constexpr const char* name = Spread ? "hash table" : "hash table (ref mod size)";

    HashIndexT(size_t max_orders, uint64_t /*max_ref*/) {
        size_t capacity = std::bit_ceil(2 * max_orders);
        slots_.assign(capacity, Slot{0, kNil});
        mask_ = capacity - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(capacity));
        max_size_ = max_orders;
    }

    uint32_t find(OrderRef key) const {
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.key == key) return s.value;
            if (s.key == 0) return kNil;
        }
    }

    // False if the key is 0, already present, or the table is at its size limit.
    bool insert(OrderRef key, uint32_t value) {
        if (key == 0 || size_ >= max_size_) return false;
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return false;
            if (slots_[i].key == 0) {
                slots_[i] = Slot{key, value};
                ++size_;
                return true;
            }
        }
    }

    // Removes the key and returns its value, or kNil if it isn't there.
    uint32_t erase(OrderRef key) {
        if (key == 0) return kNil;
        size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0) return kNil;
            i = (i + 1) & mask_;
        }
        const uint32_t value = slots_[i].value;
        size_t hole = i;
        for (size_t j = (i + 1) & mask_; slots_[j].key != 0; j = (j + 1) & mask_) {
            // slot j may move into the hole if its probe path passes through it
            const size_t from_home = (j - home(slots_[j].key)) & mask_;
            const size_t from_hole = (j - hole) & mask_;
            if (from_home >= from_hole) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = Slot{0, kNil};
        --size_;
        return value;
    }

    size_t size() const { return size_; }
    size_t memory_bytes() const { return slots_.size() * sizeof(Slot); }

private:
    struct Slot {
        OrderRef key;
        uint32_t value;
    };

    size_t home(OrderRef key) const {
        if constexpr (Spread) return static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
        else return static_cast<size_t>(key) & mask_;
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    unsigned shift_ = 0;
    size_t size_ = 0;
    size_t max_size_ = 0;
};

using HashIndex = HashIndexT<true>;
using LocalHashIndex = HashIndexT<false>;

// One slot per possible reference number. Needs the day's largest reference
// number up front (from stage 1), and 4 bytes for every number up to it.
class FlatIndex {
public:
    static constexpr const char* name = "flat array";

    FlatIndex(size_t /*max_orders*/, uint64_t max_ref) : slots_(max_ref + 1, kNil) {}

    uint32_t find(OrderRef key) const { return key < slots_.size() ? slots_[key] : kNil; }

    bool insert(OrderRef key, uint32_t value) {
        if (key == 0 || key >= slots_.size() || slots_[key] != kNil) return false;
        slots_[key] = value;
        ++size_;
        return true;
    }

    uint32_t erase(OrderRef key) {
        if (key >= slots_.size()) return kNil;
        const uint32_t value = slots_[key];
        if (value != kNil) --size_;
        slots_[key] = kNil;
        return value;
    }

    size_t size() const { return size_; }
    size_t memory_bytes() const { return slots_.size() * sizeof(uint32_t); }

private:
    std::vector<uint32_t> slots_;
    size_t size_ = 0;
};

}  // namespace ts
