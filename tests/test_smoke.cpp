#include <gtest/gtest.h>

#include "common/histogram.hpp"

TEST(Smoke, Empty) {}

TEST(Histogram, ExactBelow128AndCloseAbove) {
    ts::Histogram h;
    for (uint64_t v = 1; v <= 100; ++v) h.record(v);
    EXPECT_EQ(h.count(), 100u);
    EXPECT_EQ(h.percentile(0.50), 50u);
    EXPECT_EQ(h.percentile(0.99), 99u);
    EXPECT_EQ(h.max(), 100u);

    ts::Histogram big;
    big.record(1'000'000);
    uint64_t p = big.percentile(0.5);
    EXPECT_GE(p, 1'000'000u);
    EXPECT_LE(p, 1'000'000u + 1'000'000u / 64);
}
