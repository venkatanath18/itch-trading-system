#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "book/book_types.hpp"
#include "book/order_index.hpp"

namespace ts {

// How big to make everything at start-up, from the stage 1 measurements.
struct BookSizing {
    size_t stocks = 65536;
    size_t max_orders = 1 << 20;
    size_t max_levels = 1 << 18;
    uint64_t max_ref = 1 << 20;                          // for FlatIndex
    std::vector<std::array<uint32_t, 2>> side_levels;    // per stock: bid and ask levels to reserve

    // Reads data/derived/<day>.sizing.csv and <day>.global.txt written by
    // day_summary, adding headroom. Throws if they can't be read.
    static BookSizing from_profile(const std::string& sizing_csv, const std::string& global_txt,
                                   double headroom = 1.5);
};

// A live order: one 32-byte node in the order pool.
struct BookOrder {
    OrderRef ref;
    Qty shares;
    uint32_t prev;   // neighbours in the level's FIFO queue, or kNil
    uint32_t next;   // also links free nodes
    uint32_t level;  // slot in the level pool
    uint32_t seq;    // arrival order; queue position in the simulator compares these
    Locate locate;
    Side side;
};

struct BookLevel {
    Price price;
    uint32_t count;   // orders
    uint64_t shares;  // their total
    uint32_t head;    // front of the queue (oldest)
    uint32_t tail;
    uint32_t next;    // links free levels
};

// What the exchange simulator needs to know about an order before a message changes it.
struct OrderView {
    Locate locate;
    Side side;
    Price price;
    Qty shares;
    uint32_t seq;
};

// The fast book: every order and level preallocated, no allocation after
// construction (as long as each side's level vector stays within its reserve,
// which the sizing profile ensures and growths() reports).
//   Index:  ref -> order slot (HashIndex or FlatIndex)
//   Levels: per side, price -> level slot (SortedLevels or TickLevels)
template <class Index, template <Side> class Levels>
class FastBook {
public:
    explicit FastBook(const BookSizing& sizing)
        : index_(sizing.max_orders, sizing.max_ref), orders_(sizing.max_orders), levels_(sizing.max_levels),
          books_(sizing.stocks) {
        for (size_t i = 0; i < orders_.size(); ++i) orders_[i].next = i + 1 < orders_.size() ? static_cast<uint32_t>(i + 1) : kNil;
        for (size_t i = 0; i < levels_.size(); ++i) levels_[i].next = i + 1 < levels_.size() ? static_cast<uint32_t>(i + 1) : kNil;
        free_order_ = orders_.empty() ? kNil : 0;
        free_level_ = levels_.empty() ? kNil : 0;
        for (size_t s = 0; s < sizing.side_levels.size() && s < books_.size(); ++s) {
            books_[s].bids.reserve(sizing.side_levels[s][0]);
            books_[s].asks.reserve(sizing.side_levels[s][1]);
        }
    }

    bool add(Locate locate, OrderRef ref, Side side, Qty shares, Price price) {
        if (shares == 0 || locate >= books_.size() || free_order_ == kNil) return error();
        const uint32_t oi = free_order_;
        if (!index_.insert(ref, oi)) return error();
        const uint32_t li = side == Side::Buy ? books_[locate].bids.find_or_insert(price, [&] { return new_level(price); })
                                              : books_[locate].asks.find_or_insert(price, [&] { return new_level(price); });
        if (li == kNil) {
            index_.erase(ref);
            return error();
        }
        free_order_ = orders_[oi].next;
        BookOrder& o = orders_[oi];
        o = BookOrder{ref, shares, kNil, kNil, li, seq_++, locate, side};
        BookLevel& l = levels_[li];
        if (l.tail == kNil) {
            l.head = oi;
        } else {
            orders_[l.tail].next = oi;
            o.prev = l.tail;
        }
        l.tail = oi;
        ++l.count;
        l.shares += shares;
        return true;
    }

    bool execute(OrderRef ref, Qty shares) { return reduce(ref, shares); }
    bool cancel(OrderRef ref, Qty shares) { return reduce(ref, shares); }

    bool remove(OrderRef ref) {
        const uint32_t oi = index_.erase(ref);
        if (oi == kNil) return error();
        unlink(oi);
        return true;
    }

    bool replace(OrderRef old_ref, OrderRef new_ref, Qty shares, Price price) {
        const uint32_t oi = index_.erase(old_ref);
        if (oi == kNil) return error();
        const Locate locate = orders_[oi].locate;
        const Side side = orders_[oi].side;
        unlink(oi);
        return add(locate, new_ref, side, shares, price);
    }

    TopOfBook top(Locate locate) const {
        TopOfBook t;
        const StockBook& b = books_[locate];
        const uint32_t bi = b.bids.best();
        if (bi != kNil) {
            t.bid = levels_[bi].price;
            t.bid_shares = levels_[bi].shares;
        }
        const uint32_t ai = b.asks.best();
        if (ai != kNil) {
            t.ask = levels_[ai].price;
            t.ask_shares = levels_[ai].shares;
        }
        return t;
    }

    size_t levels(Locate locate, Side side, size_t n, LevelInfo* out) const {
        size_t i = 0;
        auto copy = [&](uint32_t li) { out[i++] = LevelInfo{levels_[li].price, levels_[li].shares, levels_[li].count}; };
        if (side == Side::Buy) books_[locate].bids.for_each(n, copy);
        else books_[locate].asks.for_each(n, copy);
        return i;
    }

    // Shares resting at one price (0 if there's no level).
    uint64_t shares_at(Locate locate, Side side, Price price) const {
        const uint32_t li = side == Side::Buy ? books_[locate].bids.find(price) : books_[locate].asks.find(price);
        return li == kNil ? 0 : levels_[li].shares;
    }

    bool find(OrderRef ref, OrderView& v) const {
        const uint32_t oi = index_.find(ref);
        if (oi == kNil) return false;
        const BookOrder& o = orders_[oi];
        v = OrderView{o.locate, o.side, levels_[o.level].price, o.shares, o.seq};
        return true;
    }

    // The arrival number the next order will get.
    uint32_t next_seq() const { return seq_; }

    template <class F>
    void for_each_order(Locate locate, Side side, Price price, F f) const {
        const uint32_t li = side == Side::Buy ? books_[locate].bids.find(price) : books_[locate].asks.find(price);
        if (li == kNil) return;
        for (uint32_t oi = levels_[li].head; oi != kNil; oi = orders_[oi].next) f(orders_[oi].ref, orders_[oi].shares);
    }

    bool contains(OrderRef ref) const { return index_.find(ref) != kNil; }
    Qty order_shares(OrderRef ref) const {
        const uint32_t oi = index_.find(ref);
        return oi == kNil ? 0 : orders_[oi].shares;
    }
    size_t order_count() const { return index_.size(); }
    size_t level_count(Locate locate, Side side) const {
        return side == Side::Buy ? books_[locate].bids.size() : books_[locate].asks.size();
    }
    uint64_t errors() const { return errors_; }

    // Times a side's level vector outgrew its reserve (each one allocates).
    uint64_t growths() const {
        uint64_t g = 0;
        for (const StockBook& b : books_) g += b.bids.growths() + b.asks.growths();
        return g;
    }

    size_t memory_bytes() const {
        return index_.memory_bytes() + orders_.size() * sizeof(BookOrder) + levels_.size() * sizeof(BookLevel);
    }

    static std::string name() { return std::string(Index::name) + " + " + Levels<Side::Buy>::name; }

private:
    struct StockBook {
        Levels<Side::Buy> bids;
        Levels<Side::Sell> asks;
    };

    bool error() {
        ++errors_;
        return false;
    }

    uint32_t new_level(Price price) {
        const uint32_t li = free_level_;
        if (li == kNil) return kNil;
        free_level_ = levels_[li].next;
        levels_[li] = BookLevel{price, 0, 0, kNil, kNil, kNil};
        return li;
    }

    bool reduce(OrderRef ref, Qty shares) {
        const uint32_t oi = index_.find(ref);
        if (oi == kNil) return error();
        BookOrder& o = orders_[oi];
        if (shares >= o.shares) {
            if (shares > o.shares) ++errors_;
            index_.erase(ref);
            unlink(oi);
        } else {
            o.shares -= shares;
            levels_[o.level].shares -= shares;
        }
        return true;
    }

    // Take an order out of its level's queue and return it to the pool; an
    // emptied level leaves its side and returns to its pool too.
    void unlink(uint32_t oi) {
        BookOrder& o = orders_[oi];
        BookLevel& l = levels_[o.level];
        if (o.prev == kNil) l.head = o.next;
        else orders_[o.prev].next = o.next;
        if (o.next == kNil) l.tail = o.prev;
        else orders_[o.next].prev = o.prev;
        l.shares -= o.shares;
        if (--l.count == 0) {
            if (o.side == Side::Buy) books_[o.locate].bids.erase(l.price);
            else books_[o.locate].asks.erase(l.price);
            l.next = free_level_;
            free_level_ = o.level;
        }
        o.next = free_order_;
        free_order_ = oi;
    }

    Index index_;
    std::vector<BookOrder> orders_;
    std::vector<BookLevel> levels_;
    std::vector<StockBook> books_;
    uint32_t free_order_ = kNil;
    uint32_t free_level_ = kNil;
    uint32_t seq_ = 0;
    uint64_t errors_ = 0;
};

}  // namespace ts
