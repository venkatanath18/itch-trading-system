#pragma once

#include <array>
#include <cstddef>

namespace ts {

// A fixed-capacity FIFO for one thread (the sequencer's in-flight queues).
template <class T, size_t N>
class RingQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v) {
        if (tail_ - head_ == N) return false;
        items_[tail_++ & (N - 1)] = v;
        return true;
    }
    bool empty() const { return head_ == tail_; }
    size_t size() const { return tail_ - head_; }
    const T& front() const { return items_[head_ & (N - 1)]; }
    void pop() { ++head_; }

private:
    std::array<T, N> items_{};
    size_t head_ = 0;
    size_t tail_ = 0;
};

}  // namespace ts
