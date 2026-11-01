#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace ts {

// 128 bytes: the cache line on Apple Silicon, and two lines on x86, where the
// adjacent-line prefetcher pulls lines in pairs.
inline constexpr size_t kCacheLine = 128;

// Single-producer single-consumer ring buffer.
//
// The producer writes a slot, then publishes it with a release store to tail_;
// the consumer's acquire load of tail_ makes the slot's contents visible
// before it reads them. The same pairing on head_ tells the producer a slot
// is free again. Each side keeps a private copy of the other side's index and
// reloads it only when the queue looks full (or empty), so in the common case
// neither touches the other's cache line. Indices only grow; slot = index % N.
template <class T, size_t N>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    bool try_push(const T& v) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == N) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == N) return false;
        }
        items_[tail & (N - 1)] = v;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;
        }
        v = items_[head & (N - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }

private:
    alignas(kCacheLine) std::atomic<size_t> tail_{0};  // producer writes
    size_t head_cache_ = 0;                            // producer's copy of head_
    alignas(kCacheLine) std::atomic<size_t> head_{0};  // consumer writes
    size_t tail_cache_ = 0;                            // consumer's copy of tail_
    alignas(kCacheLine) std::array<T, N> items_{};
};

}  // namespace ts
