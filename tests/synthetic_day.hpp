#pragma once

#include <random>
#include <vector>

#include "book/reference_book.hpp"
#include "itch/writer.hpp"
#include "sim/system.hpp"

// A small, valid ITCH day for one stock (locate 1, around $10.00), from 9:30
// to about 9:50 between the session events. A reference book follows the
// stream so that new orders never cross the other side, executions take the
// front of the best level, and some hidden trades go through the best price.
inline std::vector<std::byte> synthetic_day(uint64_t seed, int events = 20000) {
    using namespace ts;
    itch::Writer w;
    ReferenceBook book(4);
    std::mt19937_64 rng(seed);
    auto pick = [&](uint64_t n) { return rng() % n; };
    std::vector<OrderRef> live;
    OrderRef next_ref = 1;
    Timestamp t = hms(9, 30, 0) + 1000;

    w.system_event(hms(3, 0, 0), 'O');
    w.stock_directory(1, hms(3, 0, 1), "TEST");
    w.trading_action(1, hms(3, 0, 2), "TEST", 'T');
    w.system_event(hms(4, 0, 0), 'S');
    w.system_event(hms(9, 30, 0), 'Q');

    auto add = [&](Side side, Price price, Qty shares) {
        w.add(1, t, next_ref, side, shares, price, "TEST");
        book.add(1, next_ref, side, shares, price);
        live.push_back(next_ref++);
    };
    auto forget = [&](size_t i) {
        live[i] = live.back();
        live.pop_back();
    };
    for (Price k = 0; k < 5; ++k) {
        add(Side::Buy, 100000 - 100 * k, 300);
        add(Side::Sell, 100100 + 100 * k, 300);
    }

    for (int i = 0; i < events; ++i) {
        t += 1 + pick(100'000'000);
        const TopOfBook top = book.top(1);
        const Price bid = top.bid_shares ? top.bid : (top.ask_shares ? top.ask - 100 : 100000);
        const Price ask = top.ask_shares ? top.ask : bid + 100;
        const uint64_t kind = pick(100);
        if (live.empty() || kind < 42) {
            // anywhere from one tick inside the spread to three ticks behind the best
            const Side side = pick(2) ? Side::Buy : Side::Sell;
            const Price offset = static_cast<Price>(100 * pick(4));
            Price price = side == Side::Buy ? bid + 100 - offset : ask - 100 + offset;
            if (side == Side::Buy && price >= ask) price = ask - 100;
            if (side == Side::Sell && price <= bid) price = bid + 100;
            add(side, price, static_cast<Qty>(100 * (1 + pick(5))));
        } else if (kind < 62) {
            // an aggressive order takes shares from the front of the best level
            const Side side = pick(2) ? Side::Buy : Side::Sell;
            const Price best = side == Side::Buy ? top.bid : top.ask;
            if ((side == Side::Buy ? top.bid_shares : top.ask_shares) == 0) continue;
            OrderRef front = 0;
            Qty have = 0;
            book.for_each_order(1, side, best, [&](OrderRef r, Qty q) {
                if (front == 0) {
                    front = r;
                    have = q;
                }
            });
            const Qty n = static_cast<Qty>(1 + pick(have));
            w.executed(1, t, front, n, static_cast<uint64_t>(i));
            book.execute(front, n);
            if (!book.contains(front)) {
                for (size_t k = 0; k < live.size(); ++k) {
                    if (live[k] == front) {
                        forget(k);
                        break;
                    }
                }
            }
        } else if (kind < 74) {
            const size_t k = static_cast<size_t>(pick(live.size()));
            const Qty n = static_cast<Qty>(1 + pick(book.order_shares(live[k])));
            w.cancel(1, t, live[k], n);
            book.cancel(live[k], n);
            if (!book.contains(live[k])) forget(k);
        } else if (kind < 90) {
            const size_t k = static_cast<size_t>(pick(live.size()));
            w.del(1, t, live[k]);
            book.remove(live[k]);
            forget(k);
        } else if (kind < 97) {
            const size_t k = static_cast<size_t>(pick(live.size()));
            Locate l = 0;
            Side side = Side::Buy;
            Price old_price = 0;
            if (!book.find(live[k], l, side, old_price)) continue;
            const Price offset = static_cast<Price>(100 * pick(3));
            const Price price = side == Side::Buy ? bid - offset : ask + offset;
            const Qty shares = static_cast<Qty>(100 * (1 + pick(3)));
            w.replace(1, t, live[k], next_ref, shares, price);
            book.replace(live[k], next_ref, shares, price);
            live[k] = next_ref++;
        } else {
            // a hidden trade, sometimes one tick through the best price
            const Price through = pick(2) ? bid - 100 : ask + 100;
            w.trade(1, t, static_cast<Qty>(50 + pick(150)), through, static_cast<uint64_t>(i), "TEST");
        }
    }
    w.system_event(hms(16, 0, 0), 'M');
    w.system_event(hms(20, 0, 0), 'E');
    w.system_event(hms(20, 5, 0), 'C');
    return w.bytes();
}

inline ts::SystemConfig synthetic_config(ts::Timestamp latency = 0) {
    ts::SystemConfig cfg;
    ts::MarketMakerConfig m;
    m.locate = 1;
    m.as = ts::as::Params{0.002, 0.002, 300.0};
    cfg.makers.push_back(m);
    cfg.latency = latency;
    cfg.risk.loss_limit = 1000 * int64_t{ts::kPriceScale};
    return cfg;
}
