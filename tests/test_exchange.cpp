// Hand-built scenarios for the exchange simulator: queue position, partial
// fill, cancel before fill, trade-through, and an order that arrives after the
// price has moved.
#include <gtest/gtest.h>

#include "book/sorted_levels.hpp"
#include "book_types_under_test.hpp"
#include "sim/exchange.hpp"

using namespace ts;

namespace {

constexpr Locate kStock = 1;

struct ExchangeTest : ::testing::Test {
    FastHashSorted book{test_sizing()};
    ExchangeSim<FastHashSorted> sim{book, 16};
    ResponseList out;
    uint64_t next_id = 100;

    // market messages, applied the way the system applies them
    void add(OrderRef ref, Side side, Qty shares, Price price) {
        book.add(kStock, ref, side, shares, price);
        sim.on_added(kStock, side, price, shares, 0, out);
    }
    void execute(OrderRef ref, Qty shares) {
        OrderView v;
        ASSERT_TRUE(book.find(ref, v));
        book.execute(ref, shares);
        sim.on_execution(v, shares, 0, out);
    }
    void cancel(OrderRef ref, Qty shares) {
        OrderView v;
        ASSERT_TRUE(book.find(ref, v));
        book.cancel(ref, shares);
        sim.on_removed(v, shares);
    }
    void del(OrderRef ref) {
        OrderView v;
        ASSERT_TRUE(book.find(ref, v));
        book.remove(ref);
        sim.on_removed(v, v.shares);
    }

    // our orders
    OrderIntent send_new(Side side, Price price, Qty qty) {
        OrderIntent o;
        o.type = IntentType::New;
        o.locate = kStock;
        o.side = side;
        o.price = price;
        o.qty = qty;
        o.id = ++next_id;
        sim.on_order(o, 0, out);
        return o;
    }

    Response last() const { return out[out.size() - 1]; }
    Qty filled() const {
        Qty n = 0;
        for (const Response& r : out) n += r.type == ResponseType::Fill ? r.qty : 0;
        return n;
    }
};

}  // namespace

// 500 shares rest at 10.00 when our bid arrives. Executions and a delete take
// them away; then an execution reaches an order that joined after us.
TEST_F(ExchangeTest, QueuePositionAndPartialFill) {
    add(1, Side::Buy, 300, 100000);
    add(2, Side::Buy, 200, 100000);
    add(3, Side::Sell, 500, 100100);
    OrderIntent ours = send_new(Side::Buy, 100000, 100);
    EXPECT_EQ(last().type, ResponseType::Ack);
    EXPECT_EQ(sim.shares_ahead(ours.id), 500u);

    execute(1, 300);  // the front of the queue trades
    EXPECT_EQ(sim.shares_ahead(ours.id), 200u);
    del(2);
    EXPECT_EQ(sim.shares_ahead(ours.id), 0u);
    EXPECT_EQ(filled(), 0u);  // first in line, but nothing has traded with us yet

    add(4, Side::Buy, 100, 100000);  // joins behind us
    execute(4, 60);                  // a seller reached it, so it would have reached us first
    EXPECT_EQ(last().type, ResponseType::Fill);
    EXPECT_EQ(last().qty, 60u);
    EXPECT_EQ(last().remaining, 40u);
    EXPECT_EQ(last().price, 100000u);
}

TEST_F(ExchangeTest, PartialCancelAheadMovesUsUp) {
    add(1, Side::Sell, 400, 100100);
    add(2, Side::Buy, 100, 100000);
    OrderIntent ours = send_new(Side::Sell, 100100, 100);
    EXPECT_EQ(sim.shares_ahead(ours.id), 400u);
    cancel(1, 150);
    EXPECT_EQ(sim.shares_ahead(ours.id), 250u);
    add(3, Side::Sell, 50, 100100);  // behind us
    cancel(3, 50);
    EXPECT_EQ(sim.shares_ahead(ours.id), 250u);
}

TEST_F(ExchangeTest, CancelBeforeFill) {
    add(1, Side::Buy, 300, 100000);
    add(2, Side::Sell, 300, 100100);
    OrderIntent ours = send_new(Side::Buy, 100000, 100);
    OrderIntent c = ours;
    c.type = IntentType::Cancel;
    sim.on_order(c, 0, out);
    EXPECT_EQ(last().type, ResponseType::CancelAck);
    EXPECT_EQ(last().remaining, 100u);
    add(3, Side::Buy, 100, 100000);
    execute(1, 300);
    execute(3, 100);
    EXPECT_EQ(filled(), 0u);
    EXPECT_EQ(sim.live_orders(), 0u);
}

TEST_F(ExchangeTest, CancelTooLateIsRejected) {
    add(1, Side::Buy, 100, 100000);
    add(2, Side::Sell, 100, 100100);
    OrderIntent ours = send_new(Side::Buy, 100000, 100);
    add(3, Side::Sell, 100, 100000);  // a seller at our price: we're filled
    EXPECT_EQ(filled(), 100u);
    OrderIntent c = ours;
    c.type = IntentType::Cancel;
    sim.on_order(c, 0, out);
    EXPECT_EQ(last().type, ResponseType::CancelReject);
}

// Executions on our side at a worse price than ours: the market went through
// us, and would have taken our shares first, as many as it traded.
TEST_F(ExchangeTest, TradeThroughFillsUpToTheTradedSize) {
    add(1, Side::Buy, 1000, 100000);
    add(2, Side::Buy, 500, 99900);
    add(3, Side::Sell, 500, 100100);
    send_new(Side::Buy, 100000, 200);  // 1000 shares ahead of us
    execute(1, 1000);
    execute(2, 120);  // a trade at 9.99, below our 10.00 bid
    EXPECT_EQ(filled(), 120u);
    EXPECT_EQ(last().remaining, 80u);
    execute(2, 300);  // more of the same sweep
    EXPECT_EQ(filled(), 200u);
    EXPECT_EQ(last().remaining, 0u);
}

TEST_F(ExchangeTest, HiddenTradeThroughOurPrice) {
    add(1, Side::Buy, 100, 100000);
    add(2, Side::Sell, 100, 100200);
    send_new(Side::Sell, 100200, 100);
    sim.on_hidden_trade(kStock, 100100, 500, 0, out);  // inside the spread: not through us
    EXPECT_EQ(filled(), 0u);
    sim.on_hidden_trade(kStock, 100300, 30, 0, out);   // above our ask, 30 shares
    EXPECT_EQ(filled(), 30u);
}

// The bid was decided when the best ask was 10.02; by the time it arrives the
// ask is 10.01, so a buy at 10.01 would take liquidity. Post-only: rejected.
TEST_F(ExchangeTest, OrderArrivingAfterThePriceMovedIsRejected) {
    add(1, Side::Buy, 100, 100000);
    add(2, Side::Sell, 100, 100200);
    add(3, Side::Sell, 100, 100100);  // the price moved while our order was on its way
    send_new(Side::Buy, 100100, 100);
    EXPECT_EQ(last().type, ResponseType::Reject);
    EXPECT_EQ(last().reason, RejectReason::WouldCross);
    EXPECT_EQ(sim.live_orders(), 0u);
}

TEST_F(ExchangeTest, SellerArrivingAtOurBidFillsUpToItsSize) {
    add(1, Side::Buy, 100, 99900);
    add(2, Side::Sell, 100, 100200);
    send_new(Side::Buy, 100100, 300);  // a new best bid, alone at 10.01
    add(3, Side::Sell, 120, 100100);
    EXPECT_EQ(filled(), 120u);
    EXPECT_EQ(last().remaining, 180u);
}

TEST_F(ExchangeTest, ReplaceGoesToTheBackOfTheQueue) {
    add(1, Side::Buy, 300, 100000);
    add(2, Side::Sell, 300, 100100);
    OrderIntent ours = send_new(Side::Buy, 100000, 100);
    add(3, Side::Buy, 200, 100000);  // behind us
    OrderIntent r;
    r.type = IntentType::Replace;
    r.locate = kStock;
    r.side = Side::Buy;
    r.old_id = ours.id;
    r.id = ++next_id;
    r.price = 100000;
    r.qty = 100;
    sim.on_order(r, 0, out);
    EXPECT_EQ(last().type, ResponseType::ReplaceAck);
    EXPECT_EQ(sim.shares_ahead(r.id), 500u);  // now behind order 3 as well
}

TEST_F(ExchangeTest, ReplaceOfAFilledOrderIsRejected) {
    OrderIntent r;
    r.type = IntentType::Replace;
    r.locate = kStock;
    r.old_id = 12345;
    r.id = 12346;
    r.price = 100000;
    r.qty = 100;
    sim.on_order(r, 0, out);
    EXPECT_EQ(last().type, ResponseType::ReplaceReject);
    EXPECT_EQ(last().reason, RejectReason::NotLive);
}
