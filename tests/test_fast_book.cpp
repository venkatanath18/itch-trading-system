// Parts of the fast book the shared suite doesn't reach.
#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "book_types_under_test.hpp"

using namespace ts;

template <class Index>
void churn_against_unordered_map() {
    Index index(1000, 0);
    std::unordered_map<OrderRef, uint32_t> model;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        OrderRef key = 1 + rng() % 3000;  // few keys, so clusters form and break up
        if (rng() % 2 && model.size() < 1000) {
            bool inserted = index.insert(key, static_cast<uint32_t>(i));
            EXPECT_EQ(inserted, model.count(key) == 0);
            if (inserted) model[key] = static_cast<uint32_t>(i);
        } else {
            auto it = model.find(key);
            uint32_t got = index.erase(key);
            EXPECT_EQ(got, it == model.end() ? kNil : it->second);
            if (it != model.end()) model.erase(it);
        }
        ASSERT_EQ(index.size(), model.size());
    }
    for (auto [k, v] : model) EXPECT_EQ(index.find(k), v);
}

TEST(HashIndex, MatchesUnorderedMapUnderChurn) { churn_against_unordered_map<HashIndex>(); }
TEST(LocalHashIndex, MatchesUnorderedMapUnderChurn) { churn_against_unordered_map<LocalHashIndex>(); }

TEST(HashIndex, RejectsZeroAndRespectsItsLimit) {
    HashIndex index(2, 0);
    EXPECT_FALSE(index.insert(0, 1));
    EXPECT_TRUE(index.insert(5, 1));
    EXPECT_TRUE(index.insert(6, 2));
    EXPECT_FALSE(index.insert(7, 3));
    EXPECT_EQ(index.erase(0), kNil);
}

TEST(FlatIndex, OutOfRangeIsRejected) {
    // volatile, so the optimiser can't fold the keys into the bounds checks
    // (GCC then warns about the out-of-range branches it has proved dead)
    volatile OrderRef just_past = 101, far_past = 1000, last = 100;
    FlatIndex index(0, 100);
    EXPECT_FALSE(index.insert(just_past, 1));
    EXPECT_TRUE(index.insert(last, 1));
    EXPECT_EQ(index.find(last), 1u);
    EXPECT_EQ(index.find(far_past), kNil);
    EXPECT_EQ(index.erase(far_past), kNil);
}

TEST(FastBook, FullOrderPoolIsAnErrorNotACrash) {
    BookSizing s = test_sizing();
    s.max_orders = 3;
    FastHashSorted book(s);
    EXPECT_TRUE(book.add(1, 1, Side::Buy, 100, 1000));
    EXPECT_TRUE(book.add(1, 2, Side::Buy, 100, 1000));
    EXPECT_TRUE(book.add(1, 3, Side::Buy, 100, 1000));
    EXPECT_FALSE(book.add(1, 4, Side::Buy, 100, 1000));
    EXPECT_EQ(book.errors(), 1u);
    book.remove(2);
    EXPECT_TRUE(book.add(1, 4, Side::Buy, 100, 1000));
    EXPECT_EQ(book.order_count(), 3u);
}

TEST(FastBook, FullLevelPoolIsAnErrorNotACrash) {
    BookSizing s = test_sizing();
    s.max_levels = 2;
    FastHashSorted book(s);
    EXPECT_TRUE(book.add(1, 1, Side::Buy, 100, 1000));
    EXPECT_TRUE(book.add(1, 2, Side::Buy, 100, 1100));
    EXPECT_FALSE(book.add(1, 3, Side::Buy, 100, 1200));
    EXPECT_FALSE(book.contains(3));
    EXPECT_TRUE(book.add(1, 3, Side::Buy, 100, 1100));  // existing level still works
}

TEST(FastBook, ArrivalSequenceGivesQueueOrder) {
    FastHashSorted book(test_sizing());
    book.add(1, 50, Side::Buy, 100, 1000);
    book.add(1, 10, Side::Buy, 100, 1000);  // lower reference number, but later
    OrderView a, b;
    ASSERT_TRUE(book.find(50, a));
    ASSERT_TRUE(book.find(10, b));
    EXPECT_LT(a.seq, b.seq);
    EXPECT_EQ(book.shares_at(1, Side::Buy, 1000), 200u);
    EXPECT_EQ(book.shares_at(1, Side::Buy, 900), 0u);
}

TEST(FastBook, NoGrowthWithinReserve) {
    BookSizing s = test_sizing();
    s.side_levels.assign(s.stocks, {64, 64});  // smaller than the usual test reserve
    FastHashSorted book(s);
    for (OrderRef r = 1; r <= 64; ++r) book.add(1, r, Side::Sell, 100, static_cast<Price>(1000 + r));
    EXPECT_EQ(book.growths(), 0u);
    book.add(1, 65, Side::Sell, 100, 2000);
    EXPECT_EQ(book.growths(), 1u);
}
