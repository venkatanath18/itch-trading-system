#pragma once

#include <vector>

#include "common/types.hpp"

// The earlier code was written without this project's warning flags.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "legacy/order_book.hpp"
#pragma GCC diagnostic pop

// Adapter from ITCH book messages to the earlier order book: one
// legacy::Orderbook per stock, prices as double dollars, which is what it
// takes. Executions and partial cancels become volume updates, deletes become
// cancels, and a replace is a cancel plus a new order on the same side.
class LegacyBooks {
public:
    explicit LegacyBooks(size_t stocks) : books_(stocks) {}

    void add(ts::Locate locate, ts::OrderRef ref, ts::Side side, ts::Qty shares, ts::Price price) {
        books_[locate].place_order(ref, to_double(price), shares,
                                   side == ts::Side::Buy ? legacy::Ordertype::BUY : legacy::Ordertype::SELL);
    }

    void reduce(ts::Locate locate, ts::OrderRef ref, ts::Qty shares) {
        auto order = books_[locate].find_order(ref);
        if (!order) return;
        if (shares >= order->volume) books_[locate].cancel_order(ref);
        else books_[locate].update_order_by_volume(ref, order->volume - shares);
    }

    void remove(ts::Locate locate, ts::OrderRef ref) { books_[locate].cancel_order(ref); }

    void replace(ts::Locate locate, ts::OrderRef old_ref, ts::OrderRef new_ref, ts::Qty shares, ts::Price price) {
        auto order = books_[locate].find_order(old_ref);
        if (!order) return;
        const legacy::Ordertype type = order->ordertype;
        books_[locate].cancel_order(old_ref);
        books_[locate].place_order(new_ref, to_double(price), shares, type);
    }

private:
    static double to_double(ts::Price p) { return static_cast<double>(p) / ts::kPriceScale; }
    std::vector<legacy::Orderbook> books_;
};
