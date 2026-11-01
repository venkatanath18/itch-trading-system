// The Avellaneda-Stoikov formulas against hand calculations.
#include <gtest/gtest.h>

#include <cmath>

#include "strategy/avellaneda_stoikov.hpp"

using namespace ts;

// mid 100, q = 2, gamma 0.1, sigma 2, 0.5 s left, k 1.5:
//   r      = 100 - 2 * 0.1 * 4 * 0.5             = 99.6
//   spread = 0.1 * 4 * 0.5 + 20 * ln(1 + 0.1/1.5) = 0.2 + 20 * 0.0645385 = 1.490770
TEST(AvellanedaStoikov, ReservationPriceByHand) {
    as::Params p{0.1, 2.0, 1.5};
    EXPECT_NEAR(as::reservation_price(100.0, 2.0, 0.5, p), 99.6, 1e-12);
    EXPECT_NEAR(as::reservation_price(100.0, -2.0, 0.5, p), 100.4, 1e-12);
    EXPECT_NEAR(as::reservation_price(100.0, 0.0, 0.5, p), 100.0, 1e-12);
}

TEST(AvellanedaStoikov, SpreadByHand) {
    as::Params p{0.1, 2.0, 1.5};
    EXPECT_NEAR(as::total_spread(0.5, p), 0.2 + 20.0 * std::log(1.0 + 0.1 / 1.5), 1e-12);
    EXPECT_NEAR(as::total_spread(0.5, p), 1.490770, 1e-6);
}

// As gamma goes to 0 the spread tends to 2/k and the inventory skew vanishes.
TEST(AvellanedaStoikov, SmallGammaLimit) {
    as::Params p{1e-9, 0.02, 100.0};
    EXPECT_NEAR(as::total_spread(23400, p), 2.0 / 100.0, 1e-6);
    EXPECT_NEAR(as::reservation_price(50.0, 500, 23400, p), 50.0, 1e-3);
}

// More risk aversion: a larger skew per share held.
TEST(AvellanedaStoikov, GammaWidensTheSkew) {
    as::Params low{0.001, 0.01, 100.0}, high{0.01, 0.01, 100.0};
    double skew_low = 50.0 - as::reservation_price(50.0, 100, 3600, low);
    double skew_high = 50.0 - as::reservation_price(50.0, 100, 3600, high);
    EXPECT_NEAR(skew_high, 10 * skew_low, 1e-9);
}

// Quotes 98.854615 / 100.345385 round outward to whole cents: 98.85 / 100.35.
TEST(AvellanedaStoikov, QuotesRoundOutwardToTheTick) {
    as::Quotes q = as::quotes(99.6, 1.490770, 980000, 1010000, 100);
    EXPECT_EQ(q.bid, 988500u);
    EXPECT_EQ(q.ask, 1003500u);
}

TEST(AvellanedaStoikov, QuotesOnATickStayThere) {
    as::Quotes q = as::quotes(10.00, 0.02, 99000, 101000, 100);  // 9.99 and 10.01 exactly
    EXPECT_EQ(q.bid, 99900u);
    EXPECT_EQ(q.ask, 100100u);
}

// The market is 10.00 / 10.01; a reservation price far above it would put
// the bid through the ask. It is held one tick below the ask instead.
TEST(AvellanedaStoikov, QuotesNeverCrossTheMarket) {
    as::Quotes q = as::quotes(10.50, 0.02, 100000, 100100, 100);
    EXPECT_EQ(q.bid, 100000u);  // best ask - 1 tick
    EXPECT_GE(q.ask, 100000u + 100);
    q = as::quotes(9.50, 0.02, 100000, 100100, 100);
    EXPECT_EQ(q.ask, 100100u);  // best bid + 1 tick
    EXPECT_LT(q.bid, q.ask);
}

TEST(AvellanedaStoikov, SubDollarTick) {
    as::Quotes q = as::quotes(0.50, 0.0010, 4900, 5100, 1);
    EXPECT_EQ(q.bid, 4995u);
    EXPECT_EQ(q.ask, 5005u);
}
