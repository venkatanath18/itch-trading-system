// Random streams of book messages applied to the reference book and to each
// fast book; the full state must agree after every message.
#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "book_types_under_test.hpp"

using namespace ts;

namespace {

template <class Book>
class RandomBookTest : public ::testing::Test {};

using FastTypes = ::testing::Types<FastHashSorted, FastFlatSorted, FastLocalHashSorted, FastHashTick, FastHashTick64>;
TYPED_TEST_SUITE(RandomBookTest, FastTypes);

template <class A, class B>
void expect_same_side(const A& a, const B& b, Locate locate, Side side, uint64_t step) {
    std::vector<LevelInfo> la(4096), lb(4096);
    la.resize(a.levels(locate, side, la.size(), la.data()));
    lb.resize(b.levels(locate, side, lb.size(), lb.data()));
    ASSERT_EQ(la, lb) << "step " << step << " locate " << locate << " side " << int(side);
}

TYPED_TEST(RandomBookTest, AgreesWithReferenceAfterEveryMessage) {
    std::mt19937_64 rng(12345);
    ReferenceBook ref(16);
    auto fast = make_book<TypeParam>();
    std::vector<OrderRef> live;
    OrderRef next_ref = 1;

    auto pick = [&](uint64_t n) { return static_cast<uint64_t>(rng() % n); };
    for (uint64_t step = 0; step < 60'000; ++step) {
        const Locate locate = static_cast<Locate>(1 + pick(3));
        const uint64_t kind = pick(100);
        if (live.empty() || kind < 40) {
            // prices around $10 in cents, with occasional far-away stub quotes and sub-dollar prices
            Price price = static_cast<Price>(100000 + 100 * pick(60));
            if (pick(50) == 0) price = static_cast<Price>(100 + pick(9900));
            if (pick(80) == 0) price = 1999999900;
            const Side side = pick(2) ? Side::Buy : Side::Sell;
            const Qty shares = static_cast<Qty>(1 + pick(500));
            OrderRef r = next_ref;
            next_ref += 1 + pick(3);  // gaps, like ITCH
            ref.add(locate, r, side, shares, price);
            fast->add(locate, r, side, shares, price);
            live.push_back(r);
        } else {
            const size_t i = static_cast<size_t>(pick(live.size()));
            const OrderRef r = live[i];
            const Qty have = ref.order_shares(r);
            if (kind < 55) {
                const Qty n = static_cast<Qty>(1 + pick(have));
                ref.execute(r, n);
                fast->execute(r, n);
            } else if (kind < 65) {
                const Qty n = static_cast<Qty>(1 + pick(have));
                ref.cancel(r, n);
                fast->cancel(r, n);
            } else if (kind < 85) {
                ref.remove(r);
                fast->remove(r);
            } else {
                const Price price = static_cast<Price>(100000 + 100 * pick(60));
                const Qty shares = static_cast<Qty>(1 + pick(500));
                ref.replace(r, next_ref, shares, price);
                fast->replace(r, next_ref, shares, price);
                live.push_back(next_ref++);
            }
            if (!ref.contains(r)) {
                live[i] = live.back();
                live.pop_back();
            }
        }
        ASSERT_EQ(ref.top(locate), fast->top(locate)) << "step " << step;
        if (step % 97 == 0) {
            for (Locate l = 1; l <= 3; ++l) {
                expect_same_side(ref, *fast, l, Side::Buy, step);
                expect_same_side(ref, *fast, l, Side::Sell, step);
            }
        }
    }
    EXPECT_EQ(ref.order_count(), fast->order_count());
    EXPECT_EQ(ref.errors(), 0u);
    EXPECT_EQ(fast->errors(), 0u);
}

}  // namespace
