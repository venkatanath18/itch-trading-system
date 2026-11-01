#pragma once

#include <cstdint>

#include "sim/events.hpp"
#include "signals/signals.hpp"
#include "strategy/avellaneda_stoikov.hpp"

namespace ts {

struct MarketMakerConfig {
    Locate locate = 0;
    as::Params as;
    double ofi_dollars_per_share = 0;  // forecast of the next mid change per share of 1-second OFI
    bool use_ofi = false;
    Qty order_size = 100;
    int64_t max_position = 500;              // don't quote a side that would go beyond this
    Timestamp min_requote = 100'000'000;     // per side, between new orders and replaces
    Timestamp session_end = hms(16, 0, 0);
};

enum class OrderState : uint8_t { None, PendingNew, Live, PendingReplace, PendingCancel };

struct WorkingOrder {
    OrderState state = OrderState::None;
    uint64_t id = 0;
    Price price = 0;
};

// Avellaneda-Stoikov quoting for one stock, one order per side.
// evaluate() proposes intents without changing anything (the system gives
// new orders their ids); sent() is called for the intents the risk layer let
// through, and only then does the state change. Responses from the exchange
// move the orders along.
class MarketMaker {
public:
    explicit MarketMaker(const MarketMakerConfig& config) : cfg_(config) {}

    // The quotes the model wants now (0 = none on that side).
    as::Quotes target(Timestamp now, const StockSignals& s, int64_t position) const {
        if (!s.top.two_sided() || s.top.bid >= s.top.ask || now >= cfg_.session_end) return {};
        const double mid = static_cast<double>(s.mid2()) / (2.0 * kPriceScale);
        const double time_left = static_cast<double>(cfg_.session_end - now) / 1e9;
        double r = as::reservation_price(mid, static_cast<double>(position), time_left, cfg_.as);
        if (cfg_.use_ofi) r += cfg_.ofi_dollars_per_share * static_cast<double>(s.ofi_1s.sum_at(now));
        as::Quotes q = as::quotes(r, as::total_spread(time_left, cfg_.as), s.top.bid, s.top.ask, tick_size(s.top.bid));
        if (position + static_cast<int64_t>(cfg_.order_size) > cfg_.max_position) q.bid = 0;
        if (position - static_cast<int64_t>(cfg_.order_size) < -cfg_.max_position) q.ask = 0;
        return q;
    }

    void evaluate(Timestamp now, const StockSignals& s, int64_t position, bool can_quote, IntentList& out) const {
        const as::Quotes q = can_quote ? target(now, s, position) : as::Quotes{};
        propose(bid_, Side::Buy, q.bid, now, bid_next_, out);
        propose(ask_, Side::Sell, q.ask, now, ask_next_, out);
    }

    // The risk layer sent this intent to the exchange.
    void sent(const OrderIntent& o, Timestamp now) {
        WorkingOrder& w = o.side == Side::Buy ? bid_ : ask_;
        Timestamp& next = o.side == Side::Buy ? bid_next_ : ask_next_;
        switch (o.type) {
            case IntentType::New:
                w = WorkingOrder{OrderState::PendingNew, o.id, o.price};
                next = now + cfg_.min_requote;
                break;
            case IntentType::Replace:
                w = WorkingOrder{OrderState::PendingReplace, o.id, o.price};
                next = now + cfg_.min_requote;
                break;
            case IntentType::Cancel:
                if (w.id == o.id) w.state = OrderState::PendingCancel;
                break;
        }
    }

    void on_response(const Response& r) {
        WorkingOrder& w = r.side == Side::Buy ? bid_ : ask_;
        if (w.id != r.id) return;  // a fill of an order already replaced, for example
        switch (r.type) {
            case ResponseType::Ack:
            case ResponseType::ReplaceAck:
                if (w.state == OrderState::PendingNew || w.state == OrderState::PendingReplace) w.state = OrderState::Live;
                break;
            case ResponseType::Reject:
            case ResponseType::ReplaceReject:
            case ResponseType::CancelAck:
            case ResponseType::CancelReject:
                w = WorkingOrder{};
                break;
            case ResponseType::Fill:
                if (r.remaining == 0 && w.state == OrderState::Live) w = WorkingOrder{};
                break;
        }
    }

    const WorkingOrder& bid() const { return bid_; }
    const WorkingOrder& ask() const { return ask_; }
    const MarketMakerConfig& config() const { return cfg_; }
    Locate locate() const { return cfg_.locate; }

private:
    void propose(const WorkingOrder& w, Side side, Price want, Timestamp now, Timestamp next, IntentList& out) const {
        OrderIntent o;
        o.locate = cfg_.locate;
        o.side = side;
        o.qty = cfg_.order_size;
        o.price = want;
        switch (w.state) {
            case OrderState::None:
                if (want == 0 || now < next) return;
                o.type = IntentType::New;
                out.push(o);
                return;
            case OrderState::Live:
                if (want == 0) {
                    o.type = IntentType::Cancel;
                    o.id = w.id;
                    o.price = w.price;
                    out.push(o);
                } else if (want != w.price && now >= next) {
                    o.type = IntentType::Replace;
                    o.old_id = w.id;
                    out.push(o);
                }
                return;
            default:
                return;  // waiting for the exchange
        }
    }

    MarketMakerConfig cfg_;
    WorkingOrder bid_, ask_;
    Timestamp bid_next_ = 0, ask_next_ = 0;
};

}  // namespace ts
