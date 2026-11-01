#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ts {

// Latency histogram with fixed storage, so recording never allocates.
// Values below 128 are exact; above that each power of two is split into 64
// buckets, so a reported percentile is within 1.6% of the true value.
class Histogram {
public:
    void record(uint64_t value) {
        ++counts_[bucket_of(value)];
        ++count_;
        sum_ += value;
        if (value > max_) max_ = value;
        if (value < min_) min_ = value;
    }

    uint64_t count() const { return count_; }
    uint64_t max() const { return max_; }
    uint64_t min() const { return count_ ? min_ : 0; }
    double mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

    // Smallest value v such that at least fraction p of the samples are <= v
    // (reported as the upper edge of its bucket).
    uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        auto target = static_cast<uint64_t>(p * static_cast<double>(count_));
        if (target == 0) target = 1;
        uint64_t seen = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) {
                uint64_t upper = upper_edge(i);
                return upper < max_ ? upper : max_;
            }
        }
        return max_;
    }

    void merge(const Histogram& other) {
        for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
        count_ += other.count_;
        sum_ += other.sum_;
        if (other.max_ > max_) max_ = other.max_;
        if (other.count_ && other.min_ < min_) min_ = other.min_;
    }

    void clear() { *this = Histogram{}; }

private:
    static constexpr unsigned kSubBits = 6;
    static constexpr size_t kBuckets = 128 + 57 * 64;

    static size_t bucket_of(uint64_t v) {
        if (v < 128) return static_cast<size_t>(v);
        unsigned shift = static_cast<unsigned>(std::bit_width(v)) - (kSubBits + 1);
        uint64_t top = v >> shift;  // 64..127
        return 128 + (shift - 1) * 64 + static_cast<size_t>(top - 64);
    }

    static uint64_t upper_edge(size_t bucket) {
        if (bucket < 128) return bucket;
        size_t shift = (bucket - 128) / 64 + 1;
        uint64_t top = (bucket - 128) % 64 + 64;
        return ((top + 1) << shift) - 1;
    }

    std::array<uint64_t, kBuckets> counts_{};
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
    uint64_t min_ = UINT64_MAX;
};

}  // namespace ts
