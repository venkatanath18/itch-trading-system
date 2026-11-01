// Every risk rule at its limit and one step past it, and the kill switch.
#include <gtest/gtest.h>

#include "risk/risk_gate.hpp"

using namespace ts;

namespace {

// Nothing but RiskGate can put an order on the wire.
template <class G>
concept CanSendDirectly = requires(G g, OrderIntent o) { g.send(o, Timestamp{}); };
static_assert(!CanSendDirectly<OrderGateway>, "the gateway must only be reachable through RiskGate");

constexpr Locate kStock = 1;

struct RiskTest : ::testing::Test {
    RiskLimits limits;
    Signals signals{4};
    Portfolio portfolio{4};
    OrderGateway gateway{0};
    std::unique_ptr<RiskGate> gate;
    uint64_t next_id = 0;

    RiskTest() {
        limits.max_order_shares = 500;
        limits.max_position = 1000;
        limits.collar_bps = 100;  // 1%
        limits.max_open_orders = 3;
        limits.max_messages_per_second = 5;
        limits.kill_switch = true;
        limits.loss_limit = 100 * int64_t{kPriceScale};  // $100
        limits.adverse_ticks = 5;
        market(999900, 1000100);  // 99.99 / 100.01, mid 100.00
        gate = std::make_unique<RiskGate>(limits, gateway, portfolio, signals, std::vector<Locate>{kStock});
    }

    void market(Price bid, Price ask, Timestamp t = 0) { signals.on_top(kStock, TopOfBook{bid, 500, ask, 500}, t); }

    OrderIntent order(Side side, Price price, Qty qty) {
        OrderIntent o;
        o.type = IntentType::New;
        o.locate = kStock;
        o.side = side;
        o.price = price;
        o.qty = qty;
        o.id = ++next_id;
        return o;
    }

    void fill(Side side, Price price, Qty qty) {
        Response r;
        r.type = ResponseType::Fill;
        r.locate = kStock;
        r.side = side;
        r.price = price;
        r.qty = qty;
        portfolio.on_fill(r);
    }

    void rebuild() { gate = std::make_unique<RiskGate>(limits, gateway, portfolio, signals, std::vector<Locate>{kStock}); }
};

}  // namespace

TEST_F(RiskTest, OrderSizeAtTheLimitAndPastIt) {
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 500), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 501), 0), Verdict::OrderSize);
}

// mid 100.00, collar 1%: 101.00 is exactly 1% away, 101.01 is past it
TEST_F(RiskTest, PriceCollarAtTheLimitAndPastIt) {
    EXPECT_EQ(gate->submit(order(Side::Sell, 1010000, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 1010100, 100), 0), Verdict::Collar);
    EXPECT_EQ(gate->submit(order(Side::Buy, 990000, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Buy, 989900, 100), 0), Verdict::Collar);
}

TEST_F(RiskTest, OpenOrdersAtTheLimitAndPastIt) {
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Buy, 999800, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000100, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000200, 100), 0), Verdict::OpenOrders);
}

// Position limit counts the position plus every open order on the same side.
TEST_F(RiskTest, PositionAtTheLimitAndPastIt) {
    fill(Side::Buy, 1000000, 400);
    rebuild();
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 300), 0), Verdict::Accepted);  // 400 + 300
    EXPECT_EQ(gate->submit(order(Side::Buy, 999800, 300), 0), Verdict::Accepted);  // 400 + 600 = 1000
    EXPECT_EQ(gate->submit(order(Side::Buy, 999700, 1), 0), Verdict::Position);    // 1001
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000100, 500), 0), Verdict::Accepted); // reduces
}

TEST_F(RiskTest, PositionLimitOnTheShortSide) {
    limits.max_open_orders = 10;
    rebuild();
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000100, 500), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000200, 500), 0), Verdict::Accepted);  // -1000
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000300, 1), 0), Verdict::Position);
}

// 5 messages per second: the 6th inside the same second is refused; one
// second after the first, there is room again.
TEST_F(RiskTest, MessageRateAtTheLimitAndPastIt) {
    limits.max_open_orders = 100;
    limits.max_position = 100000;
    rebuild();
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 10), 100), Verdict::Accepted);
    }
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 10), 999'999'999), Verdict::MessageRate);
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 10), 1'000'000'100), Verdict::Accepted);
}

TEST_F(RiskTest, CancelsAlwaysPassButCount) {
    limits.max_messages_per_second = 1;
    rebuild();
    OrderIntent o = order(Side::Buy, 999900, 100);
    EXPECT_EQ(gate->submit(o, 0), Verdict::Accepted);
    OrderIntent c = o;
    c.type = IntentType::Cancel;
    EXPECT_EQ(gate->submit(c, 1), Verdict::Accepted);          // over the rate, still allowed
    EXPECT_EQ(gate->submit(c, 2), Verdict::UnknownOrder);      // already being cancelled
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 100), 3), Verdict::MessageRate);
}

// Long 100 bought at 100.00, mid now 99.10: equity -$90 against a peak of 0,
// so z = $100 - $90 = $10 = 100,000 price units. A buy of 100 at 99.09 has a
// worst case of 5 ticks * $0.01 * 200 shares = $10: exactly affordable. 101
// shares is $10.05: not. A sell that brings the position back to 0 always passes.
TEST_F(RiskTest, LossBudgetAtTheLimitAndPastIt) {
    limits.max_order_shares = 1000;
    rebuild();
    gate->mark(0);  // flat: equity 0, peak 0
    fill(Side::Buy, 1000000, 100);
    market(990900, 991100);  // 99.09 / 99.11
    gate->mark(1);
    EXPECT_EQ(gate->budget(), 100'000);
    EXPECT_FALSE(gate->killed());
    EXPECT_EQ(gate->submit(order(Side::Buy, 990900, 101), 2), Verdict::LossBudget);
    EXPECT_EQ(gate->submit(order(Side::Buy, 990900, 100), 2), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 991100, 100), 2), Verdict::Accepted);
}

TEST_F(RiskTest, KillSwitchFiresOnceAndNothingIsSentAfter) {
    gate->mark(0);
    EXPECT_EQ(gate->submit(order(Side::Buy, 999900, 100), 0), Verdict::Accepted);
    EXPECT_EQ(gate->submit(order(Side::Sell, 1000100, 100), 0), Verdict::Accepted);
    fill(Side::Buy, 1000000, 500);
    market(979900, 980100);  // mid 98.00: a $1,000 loss on 500 shares, past L = $100
    const uint64_t sent_before = gateway.sent();

    EXPECT_TRUE(gate->mark(1));   // fires now
    EXPECT_TRUE(gate->killed());
    EXPECT_EQ(gate->kill_cancels(), 2u);
    EXPECT_EQ(gateway.sent(), sent_before + 2);  // one cancel per open order

    market(970000, 970200, 2);
    EXPECT_FALSE(gate->mark(2));  // never fires twice
    EXPECT_EQ(gate->submit(order(Side::Sell, 970200, 100), 3), Verdict::Killed);
    EXPECT_EQ(gate->submit(order(Side::Buy, 970000, 100), 3), Verdict::Killed);
    OrderIntent again;
    again.type = IntentType::Cancel;
    again.locate = kStock;
    again.id = 1;
    EXPECT_EQ(gate->submit(again, 3), Verdict::UnknownOrder);  // its cancel is already on the way
    EXPECT_EQ(gateway.sent(), sent_before + 2);
}

TEST_F(RiskTest, WithoutTheKillSwitchThereIsNoBudget) {
    limits.kill_switch = false;
    rebuild();
    gate->mark(0);
    fill(Side::Buy, 1000000, 500);
    market(979900, 980100);
    EXPECT_FALSE(gate->mark(1));
    EXPECT_FALSE(gate->killed());
    EXPECT_LT(gate->budget(), 0);
    EXPECT_EQ(gate->submit(order(Side::Buy, 979900, 100), 2), Verdict::Accepted);
}

TEST_F(RiskTest, ResponsesCloseOpenOrders) {
    OrderIntent o = order(Side::Buy, 999900, 100);
    gate->submit(o, 0);
    EXPECT_EQ(gate->open_orders(), 1u);
    Response f;
    f.type = ResponseType::Fill;
    f.id = o.id;
    f.qty = 40;
    f.remaining = 60;
    gate->on_response(f);
    EXPECT_EQ(gate->open_orders(), 1u);
    f.qty = 60;
    f.remaining = 0;
    gate->on_response(f);
    EXPECT_EQ(gate->open_orders(), 0u);
}
