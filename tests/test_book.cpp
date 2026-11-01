// One test suite for every book implementation (typed tests).
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "book/reference_book.hpp"
#include "book_types_under_test.hpp"

using namespace ts;

namespace {

constexpr Locate kStock = 7;
constexpr Locate kOther = 8;

template <class Book>
class BookTest : public ::testing::Test {
protected:
    std::unique_ptr<Book> book = make_book<Book>();

    std::vector<LevelInfo> levels(Side side, size_t n = 10, Locate locate = kStock) {
        std::vector<LevelInfo> out(n);
        out.resize(book->levels(locate, side, n, out.data()));
        return out;
    }

    std::vector<std::pair<OrderRef, Qty>> queue(Side side, Price price, Locate locate = kStock) {
        std::vector<std::pair<OrderRef, Qty>> out;
        book->for_each_order(locate, side, price, [&](OrderRef ref, Qty shares) { out.push_back({ref, shares}); });
        return out;
    }
};

TYPED_TEST_SUITE(BookTest, BookTypes);

TYPED_TEST(BookTest, AddSortsLevelsBestFirst) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 1000000);
    b.add(kStock, 2, Side::Buy, 200, 1000100);
    b.add(kStock, 3, Side::Buy, 300, 999900);
    b.add(kStock, 4, Side::Sell, 50, 1000300);
    b.add(kStock, 5, Side::Sell, 60, 1000200);

    EXPECT_EQ(this->levels(Side::Buy), (std::vector<LevelInfo>{
        {1000100, 200, 1}, {1000000, 100, 1}, {999900, 300, 1}}));
    EXPECT_EQ(this->levels(Side::Sell), (std::vector<LevelInfo>{{1000200, 60, 1}, {1000300, 50, 1}}));
    EXPECT_EQ(b.top(kStock), (TopOfBook{1000100, 200, 1000200, 60}));
    EXPECT_EQ(b.order_count(), 5u);
}

TYPED_TEST(BookTest, SamePriceQueuesInArrivalOrder) {
    auto& b = *this->book;
    b.add(kStock, 10, Side::Buy, 100, 500);
    b.add(kStock, 11, Side::Buy, 200, 500);
    b.add(kStock, 12, Side::Buy, 300, 500);
    EXPECT_EQ(this->levels(Side::Buy), (std::vector<LevelInfo>{{500, 600, 3}}));
    EXPECT_EQ(this->queue(Side::Buy, 500), (std::vector<std::pair<OrderRef, Qty>>{{10, 100}, {11, 200}, {12, 300}}));
}

TYPED_TEST(BookTest, PartialExecutionKeepsTheOrder) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Sell, 100, 2000);
    b.add(kStock, 2, Side::Sell, 100, 2000);
    b.execute(1, 30);
    EXPECT_EQ(b.order_shares(1), 70u);
    EXPECT_EQ(this->levels(Side::Sell), (std::vector<LevelInfo>{{2000, 170, 2}}));
    EXPECT_EQ(this->queue(Side::Sell, 2000), (std::vector<std::pair<OrderRef, Qty>>{{1, 70}, {2, 100}}));
}

TYPED_TEST(BookTest, ExecutionThatEmptiesAnOrderRemovesIt) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 900);
    b.add(kStock, 2, Side::Buy, 100, 800);
    b.execute(1, 100);
    EXPECT_FALSE(b.contains(1));
    EXPECT_EQ(this->levels(Side::Buy), (std::vector<LevelInfo>{{800, 100, 1}}));
    EXPECT_EQ(b.top(kStock).bid, 800u);
    EXPECT_EQ(b.order_count(), 1u);
    EXPECT_EQ(b.errors(), 0u);
}

TYPED_TEST(BookTest, ExecutionLargerThanTheOrderRemovesItAndCountsAnError) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 900);
    b.execute(1, 150);
    EXPECT_FALSE(b.contains(1));
    EXPECT_TRUE(this->levels(Side::Buy).empty());
    EXPECT_EQ(b.errors(), 1u);
}

// C carries its own price, but the order rests at its own price: the book
// takes the shares off the order's level.
TYPED_TEST(BookTest, ExecutionWithPriceUsesTheOrdersLevel) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Sell, 100, 3000);
    BookFeed<TypeParam> feed(b);
    itch::OrderExecutedWithPrice c;
    c.ref = 1;
    c.shares = 40;
    c.price = 2950;
    feed.on_executed_with_price(c);
    EXPECT_EQ(this->levels(Side::Sell), (std::vector<LevelInfo>{{3000, 60, 1}}));
}

TYPED_TEST(BookTest, PartialCancelKeepsQueuePosition) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 500, 100);
    b.add(kStock, 2, Side::Buy, 500, 100);
    b.cancel(1, 400);
    EXPECT_EQ(this->queue(Side::Buy, 100), (std::vector<std::pair<OrderRef, Qty>>{{1, 100}, {2, 500}}));
    EXPECT_EQ(this->levels(Side::Buy), (std::vector<LevelInfo>{{100, 600, 2}}));
}

TYPED_TEST(BookTest, CancelOfAllSharesRemovesTheOrder) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 500, 100);
    b.cancel(1, 500);
    EXPECT_FALSE(b.contains(1));
    EXPECT_TRUE(this->levels(Side::Buy).empty());
}

TYPED_TEST(BookTest, DeleteRemovesOnlyThatOrder) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Sell, 100, 700);
    b.add(kStock, 2, Side::Sell, 200, 700);
    b.add(kStock, 3, Side::Sell, 300, 700);
    b.remove(2);
    EXPECT_EQ(this->queue(Side::Sell, 700), (std::vector<std::pair<OrderRef, Qty>>{{1, 100}, {3, 300}}));
    b.remove(1);
    b.remove(3);
    EXPECT_TRUE(this->levels(Side::Sell).empty());
    EXPECT_EQ(b.top(kStock), TopOfBook{});
}

// U has no side field: the new order takes the side of the old one.
TYPED_TEST(BookTest, ReplaceKeepsTheSideOfTheOriginalOrder) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Sell, 100, 5000);
    b.replace(1, 2, 300, 5100);
    EXPECT_FALSE(b.contains(1));
    EXPECT_EQ(b.order_shares(2), 300u);
    EXPECT_TRUE(this->levels(Side::Buy).empty());
    EXPECT_EQ(this->levels(Side::Sell), (std::vector<LevelInfo>{{5100, 300, 1}}));
}

// Even at the same price, a replaced order goes to the back of the queue.
TYPED_TEST(BookTest, ReplaceLosesQueuePosition) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 5000);
    b.add(kStock, 2, Side::Buy, 100, 5000);
    b.replace(1, 3, 100, 5000);
    EXPECT_EQ(this->queue(Side::Buy, 5000), (std::vector<std::pair<OrderRef, Qty>>{{2, 100}, {3, 100}}));
}

TYPED_TEST(BookTest, UnknownReferencesAreCountedAndChangeNothing) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 5000);
    b.execute(99, 10);
    b.cancel(99, 10);
    b.remove(99);
    b.replace(99, 100, 10, 5000);
    EXPECT_EQ(b.errors(), 4u);
    EXPECT_FALSE(b.contains(100));
    EXPECT_EQ(this->levels(Side::Buy), (std::vector<LevelInfo>{{5000, 100, 1}}));
}

TYPED_TEST(BookTest, DuplicateReferenceIsRejected) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 5000);
    b.add(kStock, 1, Side::Sell, 100, 6000);
    EXPECT_EQ(b.errors(), 1u);
    EXPECT_TRUE(this->levels(Side::Sell).empty());
}

TYPED_TEST(BookTest, StocksAreIndependent) {
    auto& b = *this->book;
    b.add(kStock, 1, Side::Buy, 100, 5000);
    b.add(kOther, 2, Side::Buy, 100, 5000);
    b.remove(1);
    EXPECT_TRUE(this->levels(Side::Buy).empty());
    EXPECT_EQ(this->levels(Side::Buy, 10, kOther), (std::vector<LevelInfo>{{5000, 100, 1}}));
}

TYPED_TEST(BookTest, LevelsReturnsAtMostWhatExists) {
    auto& b = *this->book;
    for (OrderRef r = 1; r <= 5; ++r) b.add(kStock, r, Side::Sell, 100, static_cast<Price>(1000 + r * 100));
    EXPECT_EQ(this->levels(Side::Sell, 3).size(), 3u);
    EXPECT_EQ(this->levels(Side::Sell, 10).size(), 5u);
    EXPECT_EQ(this->levels(Side::Sell, 3)[2].price, 1300u);
}

// Many levels inserted in scrambled order, then removed, against a simple model.
TYPED_TEST(BookTest, ManyLevelsStaySorted) {
    auto& b = *this->book;
    std::vector<Price> prices;
    for (Price i = 0; i < 300; ++i) prices.push_back(1'000'000 + ((i * 7919) % 300) * 100);
    OrderRef ref = 1;
    for (Price p : prices) {
        b.add(kStock, ref++, Side::Buy, 100, p);
        b.add(kStock, ref++, Side::Sell, 100, p + 40'000);
    }
    auto bids = this->levels(Side::Buy, 400);
    auto asks = this->levels(Side::Sell, 400);
    ASSERT_EQ(bids.size(), 300u);
    ASSERT_EQ(asks.size(), 300u);
    for (size_t i = 1; i < bids.size(); ++i) EXPECT_GT(bids[i - 1].price, bids[i].price);
    for (size_t i = 1; i < asks.size(); ++i) EXPECT_LT(asks[i - 1].price, asks[i].price);
    for (OrderRef r = 1; r < ref; r += 2) b.remove(r);
    EXPECT_TRUE(this->levels(Side::Buy).empty());
    EXPECT_EQ(this->levels(Side::Sell, 400).size(), 300u);
}

}  // namespace
