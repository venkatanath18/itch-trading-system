#pragma once

#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/book_types.hpp"

namespace ts {

// The slow book: standard containers only, written to be obviously correct.
// Every fast component is checked against it.
//   orders:  one std::unordered_map for the whole day (reference numbers are
//            unique across stocks), pointing at each order's place in its queue
//   levels:  per stock, std::map from price to level (bids highest first,
//            asks lowest first); each level is a FIFO std::list of orders
class ReferenceBook {
public:
    explicit ReferenceBook(size_t stocks = 65536) : books_(stocks) {}

    bool add(Locate locate, OrderRef ref, Side side, Qty shares, Price price) {
        if (shares == 0 || orders_.count(ref) != 0) {
            ++errors_;
            return false;
        }
        Level& level = level_at(locate, side, price);
        level.fifo.push_back({ref, shares});
        level.shares += shares;
        orders_.emplace(ref, Where{locate, side, price, std::prev(level.fifo.end())});
        return true;
    }

    // E and C: shares traded; the order leaves the book when none are left
    bool execute(OrderRef ref, Qty shares) { return reduce(ref, shares); }

    // X: some shares cancelled; the order keeps its place in the queue
    bool cancel(OrderRef ref, Qty shares) { return reduce(ref, shares); }

    // D
    bool remove(OrderRef ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++errors_;
            return false;
        }
        erase(it);
        return true;
    }

    // U: the old order leaves, and a new one joins the back of the queue at
    // the new price, on the same side (the message carries no side)
    bool replace(OrderRef old_ref, OrderRef new_ref, Qty shares, Price price) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            ++errors_;
            return false;
        }
        const Locate locate = it->second.locate;
        const Side side = it->second.side;
        erase(it);
        return add(locate, new_ref, side, shares, price);
    }

    // The best n levels of one side, best first. Returns how many were written.
    size_t levels(Locate locate, Side side, size_t n, LevelInfo* out) const {
        const Book& b = books_[locate];
        return side == Side::Buy ? copy_levels(b.bids, n, out) : copy_levels(b.asks, n, out);
    }

    TopOfBook top(Locate locate) const {
        const Book& b = books_[locate];
        TopOfBook t;
        if (!b.bids.empty()) {
            t.bid = b.bids.begin()->first;
            t.bid_shares = b.bids.begin()->second.shares;
        }
        if (!b.asks.empty()) {
            t.ask = b.asks.begin()->first;
            t.ask_shares = b.asks.begin()->second.shares;
        }
        return t;
    }

    bool contains(OrderRef ref) const { return orders_.count(ref) != 0; }

    // Where a live order rests.
    bool find(OrderRef ref, Locate& locate, Side& side, Price& price) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        locate = it->second.locate;
        side = it->second.side;
        price = it->second.price;
        return true;
    }

    Qty order_shares(OrderRef ref) const {
        auto it = orders_.find(ref);
        return it == orders_.end() ? 0 : it->second.it->shares;
    }

    size_t order_count() const { return orders_.size(); }
    uint64_t errors() const { return errors_; }
    size_t level_count(Locate locate, Side side) const {
        return side == Side::Buy ? books_[locate].bids.size() : books_[locate].asks.size();
    }

    // Invariants of one level: it isn't empty, its total is the sum of its
    // orders, every order has shares and points back here. A level that
    // doesn't exist passes.
    bool check_level(Locate locate, Side side, Price price, std::string* why) const {
        const Book& b = books_[locate];
        const Level* level = nullptr;
        if (side == Side::Buy) {
            auto it = b.bids.find(price);
            if (it != b.bids.end()) level = &it->second;
        } else {
            auto it = b.asks.find(price);
            if (it != b.asks.end()) level = &it->second;
        }
        if (level == nullptr) return true;
        if (level->fifo.empty()) return fail(why, "empty level left in the book");
        uint64_t sum = 0;
        for (const Order& o : level->fifo) {
            if (o.shares == 0) return fail(why, "order with zero shares in the book");
            auto where = orders_.find(o.ref);
            if (where == orders_.end() || where->second.price != price || where->second.side != side ||
                where->second.locate != locate) {
                return fail(why, "order map disagrees with the level");
            }
            sum += o.shares;
        }
        if (sum != level->shares) return fail(why, "level total differs from the sum of its orders");
        return true;
    }

    // Every level of every stock, and the order count. Slow: for samples.
    bool check_all(std::string* why) const {
        size_t counted = 0;
        for (size_t locate = 0; locate < books_.size(); ++locate) {
            const Book& b = books_[locate];
            for (const auto& [price, level] : b.bids) {
                if (!check_level(static_cast<Locate>(locate), Side::Buy, price, why)) return false;
                counted += level.fifo.size();
            }
            for (const auto& [price, level] : b.asks) {
                if (!check_level(static_cast<Locate>(locate), Side::Sell, price, why)) return false;
                counted += level.fifo.size();
            }
        }
        if (counted != orders_.size()) return fail(why, "orders in levels differ from orders in the map");
        return true;
    }

    // Calls f(ref, shares) for each order of a level, front of the queue first.
    template <class F>
    void for_each_order(Locate locate, Side side, Price price, F f) const {
        const Book& b = books_[locate];
        auto visit = [&](const auto& side_map) {
            auto it = side_map.find(price);
            if (it == side_map.end()) return;
            for (const Order& o : it->second.fifo) f(o.ref, o.shares);
        };
        if (side == Side::Buy) visit(b.bids);
        else visit(b.asks);
    }

private:
    struct Order {
        OrderRef ref;
        Qty shares;
    };
    struct Level {
        std::list<Order> fifo;
        uint64_t shares = 0;
    };
    struct Book {
        std::map<Price, Level, std::greater<Price>> bids;  // best (highest) first
        std::map<Price, Level> asks;                       // best (lowest) first
    };
    struct Where {
        Locate locate;
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };
    using OrderMap = std::unordered_map<OrderRef, Where>;

    Level& level_at(Locate locate, Side side, Price price) {
        Book& b = books_[locate];
        return side == Side::Buy ? b.bids[price] : b.asks[price];
    }

    bool reduce(OrderRef ref, Qty shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++errors_;
            return false;
        }
        Order& o = *it->second.it;
        if (shares >= o.shares) {
            if (shares > o.shares) ++errors_;
            erase(it);
        } else {
            o.shares -= shares;
            level_at(it->second.locate, it->second.side, it->second.price).shares -= shares;
        }
        return true;
    }

    void erase(OrderMap::iterator it) {
        const Where& w = it->second;
        Book& b = books_[w.locate];
        if (w.side == Side::Buy) {
            erase_from(b.bids, w);
        } else {
            erase_from(b.asks, w);
        }
        orders_.erase(it);
    }

    template <class SideMap>
    static void erase_from(SideMap& side_map, const Where& w) {
        auto level = side_map.find(w.price);
        level->second.shares -= w.it->shares;
        level->second.fifo.erase(w.it);
        if (level->second.fifo.empty()) side_map.erase(level);
    }

    template <class SideMap>
    static size_t copy_levels(const SideMap& side_map, size_t n, LevelInfo* out) {
        size_t i = 0;
        for (auto it = side_map.begin(); it != side_map.end() && i < n; ++it, ++i) {
            out[i] = LevelInfo{it->first, it->second.shares, static_cast<uint32_t>(it->second.fifo.size())};
        }
        return i;
    }

    static bool fail(std::string* why, const char* what) {
        if (why) *why = what;
        return false;
    }

    std::vector<Book> books_;
    OrderMap orders_;
    uint64_t errors_ = 0;
};

}  // namespace ts
