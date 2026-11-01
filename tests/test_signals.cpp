#include <gtest/gtest.h>

#include "signals/signals.hpp"

using namespace ts;

namespace {
TopOfBook tob(Price bid, uint64_t bq, Price ask, uint64_t aq) { return TopOfBook{bid, bq, ask, aq}; }
}  // namespace

// The four worked rows of p1-order-flow docs/MATH_NOTES.md section 8,
// starting from a bid of 5 shares at 100 and an ask of 7 shares at 102.
TEST(Ofi, BidQueueGrowsFrom5To8) {
    EXPECT_EQ(ofi_contribution(tob(100, 5, 102, 7), tob(100, 8, 102, 7)), 3);
}

TEST(Ofi, BidPriceRisesNewQueueOf2) {
    EXPECT_EQ(ofi_contribution(tob(100, 5, 102, 7), tob(101, 2, 102, 7)), 2);
}

TEST(Ofi, BestBidWipedOutNewBidLower) {
    EXPECT_EQ(ofi_contribution(tob(100, 5, 102, 7), tob(99, 4, 102, 7)), -5);
}

TEST(Ofi, AskQueueEatenFrom7To1) {
    EXPECT_EQ(ofi_contribution(tob(100, 5, 102, 7), tob(100, 5, 102, 1)), 6);
}

// Two more, for the ask price moving.
TEST(Ofi, AskPriceDown) {
    EXPECT_EQ(ofi_contribution(tob(100, 50, 102, 40), tob(100, 50, 101, 25)), -25);
}

TEST(Ofi, AskPriceUp) {
    EXPECT_EQ(ofi_contribution(tob(100, 50, 102, 40), tob(100, 50, 103, 60)), 40);
}

TEST(Ofi, NoChangeIsZero) {
    EXPECT_EQ(ofi_contribution(tob(100, 50, 102, 40), tob(100, 50, 102, 40)), 0);
}

TEST(RollingSum, KeepsTheLastBBuckets) {
    RollingSum<3> r(10);  // buckets of 10 ns, window of 3 buckets
    r.add(5, 1);          // bucket 0
    r.add(15, 2);         // bucket 1
    r.add(25, 4);         // bucket 2
    EXPECT_EQ(r.sum(29), 7);
    EXPECT_EQ(r.sum(30), 6);   // bucket 0 dropped
    r.add(41, 8);              // bucket 4: bucket 1 dropped too
    EXPECT_EQ(r.sum(41), 12);
    EXPECT_EQ(r.sum(1000), 0); // a long gap clears everything
}

TEST(Signals, UpdatesOnlyOnChangesAndOnlyWhenTwoSided) {
    Signals s(4);
    EXPECT_TRUE(s.on_top(1, tob(100, 50, 0, 0), 1));    // one-sided: a change, but no OFI
    EXPECT_EQ(s.stock(1).last_e, 0);
    EXPECT_TRUE(s.on_top(1, tob(100, 50, 102, 40), 2)); // previous was one-sided: no OFI
    EXPECT_EQ(s.stock(1).last_e, 0);
    EXPECT_FALSE(s.on_top(1, tob(100, 50, 102, 40), 3)); // nothing changed
    EXPECT_TRUE(s.on_top(1, tob(100, 80, 102, 40), 4));
    EXPECT_EQ(s.stock(1).last_e, 30);
    EXPECT_EQ(s.stock(1).ofi_total, 30);
    EXPECT_EQ(s.stock(1).ofi_1s.sum(4), 30);
    EXPECT_EQ(s.stock(1).changes, 3u);
    EXPECT_EQ(s.stock(1).mid2(), 202u);
}
