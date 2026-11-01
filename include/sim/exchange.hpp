#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "book/fast_book.hpp"
#include "sim/events.hpp"

namespace ts {

using ResponseList = FixedList<Response, 16>;

// Fills the strategy's orders against the replayed market.
//
// An order joins the back of its price level: every share resting there when
// it arrives is ahead of it. Those shares leave through executions, cancels
// and deletes, which ITCH reports order by order, so the queue position is
// exact. The book numbers orders by arrival (FastBook seq): an order with a
// lower number than ours is ahead, a higher one is behind.
// Our order fills when
//   - an execution reaches an order behind us at our price (we'd have been
//     first), for as many shares as that execution;
//   - the market trades through our price (an execution on our side at a
//     worse price, or a hidden trade beyond our price): the aggressor would
//     have hit us first, for as many shares as it traded there (a sweep shows
//     up as several executions, which add up);
//   - an order arrives on the other side at or through our price: it would
//     have traded with us, up to its size.
// Our orders are post-only: one that would cross the book when it arrives is
// rejected. The replayed market never sees our orders (no market impact).
template <class Book>
class ExchangeSim {
public:
    ExchangeSim(const Book& book, size_t stocks) : book_(book), watching_(stocks, 0) {}

    void on_order(const OrderIntent& o, Timestamp now, ResponseList& out) {
        switch (o.type) {
            case IntentType::New:
                if (crosses(o)) {
                    out.push(respond(ResponseType::Reject, o, now, RejectReason::WouldCross));
                } else {
                    rest(o);
                    out.push(respond(ResponseType::Ack, o, now));
                }
                return;
            case IntentType::Cancel: {
                SimOrder* s = find(o.id);
                if (s == nullptr) {
                    out.push(respond(ResponseType::CancelReject, o, now, RejectReason::NotLive));
                    return;
                }
                Response r = respond(ResponseType::CancelAck, o, now);
                r.remaining = s->open;
                out.push(r);
                remove(*s);
                return;
            }
            case IntentType::Replace: {
                SimOrder* old = find(o.old_id);
                if (old == nullptr) {
                    out.push(respond(ResponseType::ReplaceReject, o, now, RejectReason::NotLive));
                    return;
                }
                remove(*old);  // the replacement loses the old queue position
                if (crosses(o)) {
                    out.push(respond(ResponseType::ReplaceReject, o, now, RejectReason::WouldCross));
                } else {
                    rest(o);
                    out.push(respond(ResponseType::ReplaceAck, o, now));
                }
                return;
            }
        }
    }

    bool watching(Locate locate) const { return watching_[locate] != 0; }

    // E and C: the order at `before` traded `shares`.
    void on_execution(const OrderView& before, Qty shares, Timestamp now, ResponseList& out) {
        for (SimOrder& s : orders_) {
            if (!s.live || s.locate != before.locate || s.side != before.side) continue;
            if (before.price == s.price) {
                if (before.seq < s.seq) {
                    s.ahead -= std::min<uint64_t>(s.ahead, shares);
                } else {
                    fill(s, std::min(s.open, shares), now, out);
                }
            } else if (worse(s.side, before.price, s.price)) {
                fill(s, std::min(s.open, shares), now, out);  // traded through our price
            }
        }
    }

    // X, D and the old half of U: shares left the queue without trading.
    void on_removed(const OrderView& before, Qty shares) {
        for (SimOrder& s : orders_) {
            if (s.live && s.locate == before.locate && s.side == before.side && s.price == before.price &&
                before.seq < s.seq) {
                s.ahead -= std::min<uint64_t>(s.ahead, shares);
            }
        }
    }

    // A, F and the new half of U: an order joined the book. If it is on the
    // other side at or through our price, it would have traded with us.
    void on_added(Locate locate, Side side, Price price, Qty shares, Timestamp now, ResponseList& out) {
        for (SimOrder& s : orders_) {
            if (!s.live || s.locate != locate || s.side == side || shares == 0) continue;
            const bool reaches = s.side == Side::Buy ? price <= s.price : price >= s.price;
            if (reaches) {
                const Qty n = std::min(s.open, shares);
                shares -= n;
                fill(s, n, now, out);
            }
        }
    }

    // P: a trade with a hidden order. Its side is unknown, but a trade below
    // our bid (or above our ask) would have hit us first.
    void on_hidden_trade(Locate locate, Price price, Qty shares, Timestamp now, ResponseList& out) {
        for (SimOrder& s : orders_) {
            if (!s.live || s.locate != locate) continue;
            if (worse(s.side, price, s.price)) fill(s, std::min(s.open, shares), now, out);
        }
    }

    size_t live_orders() const {
        size_t n = 0;
        for (const SimOrder& s : orders_) n += s.live;
        return n;
    }

    // Shares ahead of an order in its queue (for tests and reports).
    uint64_t shares_ahead(uint64_t id) const {
        for (const SimOrder& s : orders_) {
            if (s.live && s.id == id) return s.ahead;
        }
        return 0;
    }

private:
    struct SimOrder {
        uint64_t id = 0;
        Locate locate = 0;
        Side side = Side::Buy;
        Price price = 0;
        Qty open = 0;
        uint64_t ahead = 0;  // shares in front of us in the queue
        uint32_t seq = 0;    // book arrival number at the moment we joined
        bool live = false;
    };

    // a is a worse price than b for a resting order on this side
    static bool worse(Side side, Price a, Price b) { return side == Side::Buy ? a < b : a > b; }

    bool crosses(const OrderIntent& o) const {
        const TopOfBook t = book_.top(o.locate);
        if (o.side == Side::Buy) return t.ask_shares > 0 && o.price >= t.ask;
        return t.bid_shares > 0 && o.price <= t.bid;
    }

    void rest(const OrderIntent& o) {
        for (SimOrder& s : orders_) {
            if (s.live) continue;
            s = SimOrder{o.id, o.locate, o.side, o.price, o.qty, book_.shares_at(o.locate, o.side, o.price),
                         book_.next_seq(), true};
            ++watching_[o.locate];
            return;
        }
        ++overflow_;
    }

    void remove(SimOrder& s) {
        s.live = false;
        --watching_[s.locate];
    }

    SimOrder* find(uint64_t id) {
        for (SimOrder& s : orders_) {
            if (s.live && s.id == id) return &s;
        }
        return nullptr;
    }

    void fill(SimOrder& s, Qty qty, Timestamp now, ResponseList& out) {
        if (qty == 0) return;
        s.open -= qty;
        Response r;
        r.type = ResponseType::Fill;
        r.locate = s.locate;
        r.side = s.side;
        r.id = s.id;
        r.price = s.price;
        r.qty = qty;
        r.remaining = s.open;
        r.exchange_time = now;
        out.push(r);
        if (s.open == 0) remove(s);
    }

    static Response respond(ResponseType type, const OrderIntent& o, Timestamp now,
                            RejectReason reason = RejectReason::None) {
        Response r;
        r.type = type;
        r.locate = o.locate;
        r.side = o.side;
        r.reason = reason;
        r.id = o.id;
        r.old_id = o.old_id;
        r.price = o.price;
        r.qty = o.qty;
        r.remaining = o.qty;
        r.exchange_time = now;
        return r;
    }

    const Book& book_;
    std::vector<uint16_t> watching_;  // live orders per stock
    std::array<SimOrder, 64> orders_{};
    uint64_t overflow_ = 0;
};

}  // namespace ts
