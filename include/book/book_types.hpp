#pragma once

#include <cstdint>

#include "common/types.hpp"
#include "itch/parser.hpp"

namespace ts {

struct LevelInfo {
    Price price = 0;
    uint64_t shares = 0;
    uint32_t orders = 0;
    bool operator==(const LevelInfo&) const = default;
};

// Best bid and ask with the shares at each; shares are 0 when a side is empty.
struct TopOfBook {
    Price bid = 0;
    uint64_t bid_shares = 0;
    Price ask = 0;
    uint64_t ask_shares = 0;

    bool two_sided() const { return bid_shares > 0 && ask_shares > 0; }
    bool operator==(const TopOfBook&) const = default;
};

// Turns decoded ITCH messages into calls on a book (either implementation).
template <class Book>
struct BookFeed : itch::NullHandler {
    Book& book;
    explicit BookFeed(Book& b) : book(b) {}

    void on_add(const itch::AddOrder& m) { book.add(m.h.locate, m.ref, m.side, m.shares, m.price); }
    void on_executed(const itch::OrderExecuted& m) { book.execute(m.ref, m.shares); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { book.execute(m.ref, m.shares); }
    void on_cancel(const itch::OrderCancel& m) { book.cancel(m.ref, m.shares); }
    void on_delete(const itch::OrderDelete& m) { book.remove(m.ref); }
    void on_replace(const itch::OrderReplace& m) { book.replace(m.old_ref, m.new_ref, m.shares, m.price); }
};

// The message types that change a book.
inline bool changes_book(char type) {
    switch (type) {
        case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': case 'U': return true;
        default: return false;
    }
}

}  // namespace ts
