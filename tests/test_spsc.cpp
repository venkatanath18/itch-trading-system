#include <gtest/gtest.h>

#include <thread>

#include "infra/spsc_queue.hpp"

using namespace ts;

TEST(SpscQueue, FullAndEmpty) {
    SpscQueue<int, 4> q;
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

// Two threads, a million values: everything arrives, once, in order.
// Under ThreadSanitizer this also checks the acquire/release pairing.
TEST(SpscQueue, TwoThreadsKeepOrder) {
    auto q = std::make_unique<SpscQueue<uint64_t, 1024>>();
    constexpr uint64_t kCount = 1'000'000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount;) {
            if (q->try_push(i)) ++i;
        }
    });
    uint64_t expected = 0;
    while (expected < kCount) {
        uint64_t v;
        if (q->try_pop(v)) {
            ASSERT_EQ(v, expected);
            ++expected;
        }
    }
    producer.join();
    EXPECT_TRUE(q->empty());
}
