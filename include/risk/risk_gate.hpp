#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "common/clock.hpp"
#include "infra/ring_queue.hpp"
#include "risk/portfolio.hpp"
#include "signals/signals.hpp"
#include "sim/events.hpp"

namespace ts {

class RiskGate;

// An order on its way to the exchange, with the time it gets there.
struct PendingOrder {
    OrderIntent order;
    Timestamp arrival = 0;
};

// The connection to the exchange. send() is private and only RiskGate is a
// friend, so the compiler refuses any other path out.
class OrderGateway {
public:
    explicit OrderGateway(Timestamp one_way_latency) : latency_(one_way_latency) {}

    bool empty() const { return queue_.empty(); }
    const PendingOrder& front() const { return queue_.front(); }
    void pop() { queue_.pop(); }
    uint64_t sent() const { return sent_; }
    uint64_t dropped() const { return dropped_; }

private:
    friend class RiskGate;
    void send(const OrderIntent& o, Timestamp now) {
        if (queue_.push(PendingOrder{o, now + latency_})) ++sent_;
        else ++dropped_;
    }

    RingQueue<PendingOrder, 4096> queue_;
    Timestamp latency_;
    uint64_t sent_ = 0;
    uint64_t dropped_ = 0;
};

struct RiskLimits {
    Qty max_order_shares = 1000;
    int64_t max_position = 1000;            // shares per stock, counting every open order on the side
    uint32_t collar_bps = 200;              // price within 2% of the mid
    uint32_t max_open_orders = 20;          // all stocks together
    uint32_t max_messages_per_second = 100; // new orders, replaces and cancels together
    bool kill_switch = true;                // the loss budget and the kill switch
    int64_t loss_limit = 500 * int64_t{kPriceScale};  // L, in price units ($500)
    int64_t adverse_ticks = 5;              // m in the worst case
};

enum class Verdict : uint8_t {
    Accepted, Killed, OrderSize, Position, Collar, OpenOrders, MessageRate, LossBudget, UnknownOrder, Count
};

inline const char* to_string(Verdict v) {
    switch (v) {
        case Verdict::Accepted: return "accepted";
        case Verdict::Killed: return "kill switch has fired";
        case Verdict::OrderSize: return "order too large";
        case Verdict::Position: return "position limit";
        case Verdict::Collar: return "price outside the collar";
        case Verdict::OpenOrders: return "too many open orders";
        case Verdict::MessageRate: return "too many messages per second";
        case Verdict::LossBudget: return "worst case exceeds the loss budget";
        case Verdict::UnknownOrder: return "no such open order";
        case Verdict::Count: break;
    }
    return "?";
}

// Pre-trade checks on every order, and the loss-limit kill switch:
//   equity  = cash + position marked at the mid, over the traded stocks
//   budget  z = L - (peak equity - equity)
//   an order that can add to the position is allowed only if its worst case
//   keeps z >= 0. Worst case: it fills at its price, and the mid then moves m
//   ticks against the position the side would have if all its orders filled:
//       Q * max(0, price beyond the mid) + m * tick * |position after|
//   when z reaches 0, every open order is cancelled and quoting stops for good.
class RiskGate {
public:
    RiskGate(const RiskLimits& limits, OrderGateway& gateway, const Portfolio& portfolio, const Signals& signals,
             std::vector<Locate> traded)
        : limits_(limits), gateway_(gateway), portfolio_(portfolio), signals_(signals), traded_(std::move(traded)),
          send_times_(limits.max_messages_per_second ? limits.max_messages_per_second : 1),
          budget_(limits.loss_limit) {}

    // checked_at, if given, receives the cycle counter after the checks and
    // before the send (for the stage timings).
    Verdict submit(const OrderIntent& o, Timestamp now, uint64_t* checked_at = nullptr) {
        const Verdict v = check(o, now);
        if (checked_at) *checked_at = read_counter();
        ++verdicts_[static_cast<size_t>(v)];
        if (v != Verdict::Accepted) return v;
        record_send(now);
        switch (o.type) {
            case IntentType::New:
                open_.push(Open{o.id, o.locate, o.side, o.qty, false});
                break;
            case IntentType::Replace:
                remove_open(o.old_id);
                open_.push(Open{o.id, o.locate, o.side, o.qty, false});
                break;
            case IntentType::Cancel:
                find_open(o.id)->cancelling = true;
                break;
        }
        gateway_.send(o, now);
        return v;
    }

    void on_response(const Response& r) {
        switch (r.type) {
            case ResponseType::Reject:
            case ResponseType::ReplaceReject:
            case ResponseType::CancelAck:
            case ResponseType::CancelReject:
                remove_open(r.id);
                break;
            case ResponseType::Fill:
                if (Open* o = find_open(r.id)) {
                    o->qty = r.remaining;
                    if (r.remaining == 0) remove_open(r.id);
                }
                break;
            default:
                break;
        }
    }

    // Mark the traded stocks to their mids. Fires the kill switch the first
    // time the budget is used up; returns true at that moment.
    bool mark(Timestamp now) {
        int64_t equity = 0;
        for (Locate l : traded_) {
            const Holding& h = portfolio_.holding(l);
            equity += h.cash + h.position * static_cast<int64_t>(signals_.stock(l).last_mid2) / 2;
        }
        equity_ = equity;
        if (equity_ > peak_) peak_ = equity_;
        if (peak_ - equity_ > max_drawdown_) max_drawdown_ = peak_ - equity_;
        budget_ = limits_.loss_limit - (peak_ - equity_);
        if (!limits_.kill_switch || killed_ || budget_ > 0) return false;

        killed_ = true;
        killed_at_ = now;
        for (Open& o : open_) {
            if (o.cancelling) continue;
            o.cancelling = true;
            OrderIntent c;
            c.type = IntentType::Cancel;
            c.locate = o.locate;
            c.side = o.side;
            c.id = o.id;
            record_send(now);
            gateway_.send(c, now);
            ++kill_cancels_;
        }
        return true;
    }

    bool killed() const { return killed_; }
    Timestamp killed_at() const { return killed_at_; }
    uint64_t kill_cancels() const { return kill_cancels_; }
    int64_t budget() const { return budget_; }
    int64_t equity() const { return equity_; }
    int64_t peak() const { return peak_; }
    int64_t max_drawdown() const { return max_drawdown_; }
    uint64_t verdicts(Verdict v) const { return verdicts_[static_cast<size_t>(v)]; }
    size_t open_orders() const { return open_.size(); }
    const RiskLimits& limits() const { return limits_; }

private:
    struct Open {
        uint64_t id;
        Locate locate;
        Side side;
        Qty qty;
        bool cancelling;
    };

    Verdict check(const OrderIntent& o, Timestamp now) const {
        if (o.type == IntentType::Cancel) {
            const Open* open = find_open(o.id);
            return open && !open->cancelling ? Verdict::Accepted : Verdict::UnknownOrder;  // cancels always pass
        }
        if (killed_) return Verdict::Killed;
        if (o.qty > limits_.max_order_shares) return Verdict::OrderSize;

        const StockSignals& s = signals_.stock(o.locate);
        if (!s.top.two_sided()) return Verdict::Collar;  // no mid to measure against
        const int64_t mid2 = static_cast<int64_t>(s.mid2());
        const int64_t away2 = std::llabs(2 * static_cast<int64_t>(o.price) - mid2);
        if (away2 * 10000 > int64_t{limits_.collar_bps} * mid2) return Verdict::Collar;

        if (o.type == IntentType::Replace && find_open(o.old_id) == nullptr) return Verdict::UnknownOrder;
        if (o.type == IntentType::New && open_.size() >= limits_.max_open_orders) return Verdict::OpenOrders;

        // the position if this order and every other open order on its side filled
        int64_t same_side = 0;
        for (const Open& open : open_) {
            if (open.locate == o.locate && open.side == o.side && open.id != o.old_id && !open.cancelling) {
                same_side += open.qty;
            }
        }
        const int64_t position = portfolio_.position(o.locate);
        const int64_t sign = o.side == Side::Buy ? 1 : -1;
        const int64_t after = position + sign * (same_side + int64_t{o.qty});
        if (sign * after > limits_.max_position) return Verdict::Position;

        if (!rate_ok(now)) return Verdict::MessageRate;

        if (limits_.kill_switch && std::llabs(after) > std::llabs(position)) {
            const int64_t beyond2 = sign * (2 * static_cast<int64_t>(o.price) - mid2);
            const int64_t adverse = beyond2 > 0 ? int64_t{o.qty} * beyond2 / 2 : 0;
            const int64_t worst = adverse + limits_.adverse_ticks * tick_size(o.price) * std::llabs(after);
            if (budget_ - worst < 0) return Verdict::LossBudget;
        }
        return Verdict::Accepted;
    }

    // fewer than max_messages_per_second sends in the last second of feed time
    bool rate_ok(Timestamp now) const {
        if (sends_ < send_times_.size()) return true;
        return now - send_times_[next_send_] >= kNanosPerSecond;
    }

    void record_send(Timestamp now) {
        send_times_[next_send_] = now;
        next_send_ = (next_send_ + 1) % send_times_.size();
        if (sends_ < send_times_.size()) ++sends_;
    }

    Open* find_open(uint64_t id) {
        for (Open& o : open_) {
            if (o.id == id) return &o;
        }
        return nullptr;
    }
    const Open* find_open(uint64_t id) const { return const_cast<RiskGate*>(this)->find_open(id); }

    void remove_open(uint64_t id) {
        for (size_t i = 0; i < open_.size(); ++i) {
            if (open_[i].id == id) {
                open_.erase_at(i);
                return;
            }
        }
    }

    // a small unordered list with removal by swapping in the last element
    class OpenList {
    public:
        void push(const Open& o) {
            if (size_ < items_.size()) items_[size_++] = o;
        }
        void erase_at(size_t i) { items_[i] = items_[--size_]; }
        size_t size() const { return size_; }
        Open& operator[](size_t i) { return items_[i]; }
        Open* begin() { return items_.data(); }
        Open* end() { return items_.data() + size_; }
        const Open* begin() const { return items_.data(); }
        const Open* end() const { return items_.data() + size_; }

    private:
        std::array<Open, 128> items_{};
        size_t size_ = 0;
    };

    RiskLimits limits_;
    OrderGateway& gateway_;
    const Portfolio& portfolio_;
    const Signals& signals_;
    std::vector<Locate> traded_;
    OpenList open_;
    std::vector<Timestamp> send_times_;
    size_t next_send_ = 0;
    size_t sends_ = 0;
    int64_t equity_ = 0;
    int64_t peak_ = 0;
    int64_t max_drawdown_ = 0;
    int64_t budget_ = 0;
    bool killed_ = false;
    Timestamp killed_at_ = 0;
    uint64_t kill_cancels_ = 0;
    std::array<uint64_t, static_cast<size_t>(Verdict::Count)> verdicts_{};
};

}  // namespace ts
