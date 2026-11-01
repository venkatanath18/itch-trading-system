#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "book/fast_book.hpp"
#include "common/clock.hpp"
#include "common/histogram.hpp"
#include "infra/log.hpp"
#include "infra/ring_queue.hpp"
#include "itch/parser.hpp"
#include "itch/session.hpp"
#include "risk/portfolio.hpp"
#include "risk/risk_gate.hpp"
#include "signals/signals.hpp"
#include "sim/events.hpp"
#include "sim/exchange.hpp"
#include "strategy/market_maker.hpp"

namespace ts {

struct SystemConfig {
    std::vector<MarketMakerConfig> makers;
    RiskLimits risk;
    Timestamp latency = 0;                       // one way, both directions
    Timestamp mark_every = 60 * kNanosPerSecond; // MARK lines in the log
    // Quote only inside market hours (Q to M) and inside this window, which
    // keeps our orders out of the opening and closing crosses.
    Timestamp quote_from = hms(9, 31, 0);
    Timestamp quote_until = hms(15, 59, 0);
    // When measuring, record only from here: the pre-market part of the
    // replay warms up the caches, the branch predictors and the pools.
    Timestamp measure_from = hms(9, 30, 0);
};

// Latency of each stage in counter ticks (filled only when measuring).
//   t0 before decoding, t1 after the book, t2 after the signals,
//   t3 after the strategy, t4 after the risk checks, t5 order handed over
struct StageLatency {
    Histogram book;      // t1 - t0, book messages
    Histogram signals;   // t2 - t1, book messages
    Histogram strategy;  // t3 - t2, top-of-book changes of traded stocks
    Histogram risk;      // t4 - t3, per intent
    Histogram send;      // t5 - t4, per order sent
    Histogram to_order;  // t5 - t0, messages that led to an order
    Histogram message;   // end - t0, every market message
};

struct SystemCounts {
    uint64_t market_messages = 0;
    uint64_t events = 0;
    uint64_t orders = 0;            // new orders and replaces sent
    uint64_t cancels = 0;
    uint64_t risk_rejects = 0;
    uint64_t exchange_rejects = 0;
    uint64_t fills = 0;
    uint64_t dropped = 0;           // in-flight queues full (should stay 0)
};

struct PendingResponse {
    Response response;
    Timestamp delivery = 0;
};

// The whole hot path on one thread: sequencer, feed handler, book, signals,
// strategy, risk checks and the simulated exchange.
//
// The sequencer gives every event one global number and handles events in
// time order: market messages from the file, orders reaching the exchange,
// and responses reaching the strategy. At equal times an order arrival goes
// first, then a response, then a market message, so a run depends only on
// the file and the configuration.
template <class Book, class Log, bool Measure = false>
class TradingSystem : public itch::NullHandler {
public:
    TradingSystem(const SystemConfig& cfg, const BookSizing& sizing, Log& log)
        : cfg_(cfg), book_(std::make_unique<Book>(sizing)), signals_(sizing.stocks), portfolio_(sizing.stocks),
          gateway_(cfg.latency), risk_(cfg.risk, gateway_, portfolio_, signals_, traded(cfg)),
          sim_(*book_, sizing.stocks), maker_of_(sizing.stocks, -1), state_(sizing.stocks, 'T'),
          close_mid2_(sizing.stocks, 0),
          responses_(std::make_unique<RingQueue<PendingResponse, 4096>>()), log_(log) {
        makers_.reserve(cfg.makers.size());
        for (const MarketMakerConfig& m : cfg.makers) {
            maker_of_[m.locate] = static_cast<int>(makers_.size());
            makers_.emplace_back(m);
        }
    }

    template <class Source>
    void run(Source& source) {
        itch::RawMessage m;
        bool have = source.next(m);
        while (have || !gateway_.empty() || !responses_->empty()) {
            const Timestamp t_market = have ? m.timestamp() : kNever;
            const Timestamp t_order = gateway_.empty() ? kNever : gateway_.front().arrival;
            const Timestamp t_response = responses_->empty() ? kNever : responses_->front().delivery;
            ++seq_;
            if (t_order <= t_market && t_order <= t_response) {
                deliver_order();
            } else if (t_response <= t_market) {
                deliver_response();
            } else {
                on_market(m);
                have = source.next(m);
            }
        }
        marks();
        counts_.events = seq_;
    }

    // ---- market messages ---------------------------------------------------

    void on_system_event(const itch::SystemEvent& e) {
        session_.on_system_event(e);
        if (e.event_code == 'M') {
            for (const MarketMaker& mm : makers_) close_mid2_[mm.locate()] = signals_.stock(mm.locate()).last_mid2;
        }
        LogRecord r = record(LogKind::Session, 0);
        r.code = static_cast<uint8_t>(e.event_code);
        log_.write(r);
        for (size_t i = 0; i < makers_.size(); ++i) react(i, false);
    }

    void on_trading_action(const itch::TradingAction& a) {
        state_[a.h.locate] = a.state;
        if (maker_of_[a.h.locate] >= 0) react(static_cast<size_t>(maker_of_[a.h.locate]), false);
    }

    void on_add(const itch::AddOrder& m) {
        book_->add(m.h.locate, m.ref, m.side, m.shares, m.price);
        if (sim_.watching(m.h.locate)) sim_.on_added(m.h.locate, m.side, m.price, m.shares, now_, sim_out_);
        after_book(m.h.locate);
    }

    void on_executed(const itch::OrderExecuted& m) { executed(m.h.locate, m.ref, m.shares, true); }

    // A non-printable C is part of a cross (opening, closing, halt): it trades
    // at the single cross price, not in the continuous market, so it only
    // moves our queue position.
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        executed(m.h.locate, m.ref, m.shares, m.printable != 'N');
    }

    void on_cancel(const itch::OrderCancel& m) {
        OrderView v;
        const bool watched = sim_.watching(m.h.locate) && book_->find(m.ref, v);
        book_->cancel(m.ref, m.shares);
        if (watched) sim_.on_removed(v, m.shares);
        after_book(m.h.locate);
    }

    void on_delete(const itch::OrderDelete& m) {
        OrderView v;
        const bool watched = sim_.watching(m.h.locate) && book_->find(m.ref, v);
        book_->remove(m.ref);
        if (watched) sim_.on_removed(v, v.shares);
        after_book(m.h.locate);
    }

    void on_replace(const itch::OrderReplace& m) {
        OrderView v;
        const bool watched = sim_.watching(m.h.locate) && book_->find(m.old_ref, v);
        book_->replace(m.old_ref, m.new_ref, m.shares, m.price);
        if (watched) {
            sim_.on_removed(v, v.shares);
            sim_.on_added(m.h.locate, v.side, m.price, m.shares, now_, sim_out_);
        }
        after_book(m.h.locate);
    }

    void on_trade(const itch::Trade& t) {
        if (sim_.watching(t.h.locate)) sim_.on_hidden_trade(t.h.locate, t.price, t.shares, now_, sim_out_);
    }

    // ---- results -----------------------------------------------------------

    const Book& book() const { return *book_; }
    const Signals& signals() const { return signals_; }
    const Portfolio& portfolio() const { return portfolio_; }
    const RiskGate& risk() const { return risk_; }
    const SystemCounts& counts() const { return counts_; }
    const StageLatency& latency() const { return lat_; }
    const std::vector<MarketMaker>& makers() const { return makers_; }

    // Twice the mid at the end of market hours (the M event), where the
    // model marks what is left of the inventory.
    int64_t close_mid2(Locate locate) const { return static_cast<int64_t>(close_mid2_[locate]); }

private:
    static constexpr Timestamp kNever = ~Timestamp{0};

    static std::vector<Locate> traded(const SystemConfig& cfg) {
        std::vector<Locate> v;
        for (const MarketMakerConfig& m : cfg.makers) v.push_back(m.locate);
        return v;
    }

    void on_market(const itch::RawMessage& m) {
        now_ = m.timestamp();
        if constexpr (Measure) {
            recording_ = now_ >= cfg_.measure_from;
            sent_this_message_ = false;
            t0_ = read_counter();
        }
        if (session_.regular_hours() && now_ >= next_mark_) marks();
        if (itch::dispatch(m.data, m.len, *this) != itch::Status::Ok) ++bad_messages_;
        flush_exchange();
        ++counts_.market_messages;
        if constexpr (Measure) {
            const uint64_t end = read_counter();
            if (recording_) {
                lat_.message.record(end - t0_);
                if (sent_this_message_) lat_.to_order.record(t5_ - t0_);
            }
        }
    }

    void executed(Locate locate, OrderRef ref, Qty shares, bool continuous) {
        OrderView v;
        const bool watched = sim_.watching(locate) && book_->find(ref, v);
        book_->execute(ref, shares);
        if (watched) {
            if (continuous) sim_.on_execution(v, shares, now_, sim_out_);
            else sim_.on_removed(v, shares);
        }
        after_book(locate);
    }

    void after_book(Locate locate) {
        if constexpr (Measure) {
            t1_ = read_counter();
            if (recording_) lat_.book.record(t1_ - t0_);
        }
        const bool changed = signals_.on_top(locate, book_->top(locate), now_);
        if constexpr (Measure) {
            t2_ = read_counter();
            if (recording_) lat_.signals.record(t2_ - t1_);
        }
        if (changed && maker_of_[locate] >= 0) react(static_cast<size_t>(maker_of_[locate]), true);
    }

    bool can_quote(Locate locate) const {
        return session_.regular_hours() && now_ >= cfg_.quote_from && now_ < cfg_.quote_until && !risk_.killed() &&
               state_[locate] == 'T';
    }

    // Mark to market, ask the strategy what it wants, pass it through risk.
    void react(size_t i, bool from_market) {
        MarketMaker& mm = makers_[i];
        const Locate locate = mm.locate();
        if (risk_.mark(now_)) {
            LogRecord r = record(LogKind::Kill, locate);
            r.qty = static_cast<Qty>(risk_.kill_cancels());
            r.value = risk_.budget();
            r.value2 = risk_.equity();
            log_.write(r);
        }
        IntentList intents;
        mm.evaluate(now_, signals_.stock(locate), portfolio_.position(locate), can_quote(locate), intents);
        if constexpr (Measure) {
            if (from_market && recording_) lat_.strategy.record(read_counter() - t2_);
        }
        for (OrderIntent o : intents) {
            if (o.type != IntentType::Cancel) o.id = ++next_order_id_;
            uint64_t t3 = 0, t4 = 0;
            if constexpr (Measure) t3 = read_counter();
            const Verdict v = risk_.submit(o, now_, Measure ? &t4 : nullptr);
            LogRecord r = record(v == Verdict::Accepted ? LogKind::Order : LogKind::RiskReject, locate);
            r.side = o.side == Side::Buy ? 'B' : 'S';
            r.id = o.id;
            r.old_id = o.old_id;
            r.price = o.price;
            r.qty = o.qty;
            r.code = v == Verdict::Accepted ? static_cast<uint8_t>(o.type) : static_cast<uint8_t>(v);
            if (v == Verdict::Accepted) {
                mm.sent(o, now_);
                if (o.type == IntentType::Cancel) ++counts_.cancels;
                else ++counts_.orders;
                if constexpr (Measure) {
                    const uint64_t t5 = read_counter();
                    if (recording_) {
                        lat_.risk.record(t4 - t3);
                        lat_.send.record(t5 - t4);
                    }
                    if (from_market && !sent_this_message_) {
                        sent_this_message_ = true;
                        t5_ = t5;
                    }
                }
            } else {
                ++counts_.risk_rejects;
                if constexpr (Measure) {
                    if (recording_) lat_.risk.record(t4 - t3);
                }
            }
            log_.write(r);
        }
    }

    void deliver_order() {
        const PendingOrder p = gateway_.front();
        gateway_.pop();
        now_ = p.arrival;
        sim_.on_order(p.order, now_, sim_out_);
        flush_exchange();
    }

    void deliver_response() {
        const PendingResponse p = responses_->front();
        responses_->pop();
        now_ = p.delivery;
        const Response& r = p.response;
        if (r.type == ResponseType::Fill) portfolio_.on_fill(r);
        risk_.on_response(r);
        const int i = maker_of_[r.locate];
        if (i >= 0) {
            makers_[static_cast<size_t>(i)].on_response(r);
            react(static_cast<size_t>(i), false);
        }
    }

    // Log what the exchange just did and send it back to the strategy.
    void flush_exchange() {
        for (const Response& resp : sim_out_) {
            LogRecord r = record(log_kind(resp.type), resp.locate);
            r.side = resp.side == Side::Buy ? 'B' : 'S';
            r.id = resp.id;
            r.old_id = resp.old_id;
            r.price = resp.price;
            r.qty = resp.qty;
            r.remaining = resp.remaining;
            r.code = static_cast<uint8_t>(resp.reason);
            log_.write(r);
            if (resp.type == ResponseType::Fill) ++counts_.fills;
            if (resp.type == ResponseType::Reject || resp.type == ResponseType::ReplaceReject) ++counts_.exchange_rejects;
            if (!responses_->push(PendingResponse{resp, now_ + cfg_.latency})) ++counts_.dropped;
        }
        sim_out_.clear();
    }

    void marks() {
        for (const MarketMaker& mm : makers_) {
            const Holding& h = portfolio_.holding(mm.locate());
            LogRecord r = record(LogKind::Mark, mm.locate());
            r.value = h.position;
            r.value2 = h.cash + h.position * static_cast<int64_t>(signals_.stock(mm.locate()).last_mid2) / 2;
            log_.write(r);
        }
        next_mark_ = (now_ / cfg_.mark_every + 1) * cfg_.mark_every;
    }

    LogRecord record(LogKind kind, Locate locate) const {
        LogRecord r;
        r.seq = seq_;
        r.time = now_;
        r.kind = kind;
        r.locate = locate;
        return r;
    }

    static LogKind log_kind(ResponseType t) {
        switch (t) {
            case ResponseType::Ack: return LogKind::Ack;
            case ResponseType::Reject: return LogKind::Reject;
            case ResponseType::ReplaceAck: return LogKind::ReplaceAck;
            case ResponseType::ReplaceReject: return LogKind::ReplaceReject;
            case ResponseType::CancelAck: return LogKind::CancelAck;
            case ResponseType::CancelReject: return LogKind::CancelReject;
            case ResponseType::Fill: return LogKind::Fill;
        }
        return LogKind::Fill;
    }

    SystemConfig cfg_;
    std::unique_ptr<Book> book_;
    Signals signals_;
    Portfolio portfolio_;
    OrderGateway gateway_;
    RiskGate risk_;
    ExchangeSim<Book> sim_;
    std::vector<MarketMaker> makers_;
    std::vector<int> maker_of_;
    std::vector<char> state_;  // trading state per stock, from H messages
    std::vector<uint64_t> close_mid2_;
    std::unique_ptr<RingQueue<PendingResponse, 4096>> responses_;
    ResponseList sim_out_;
    itch::Session session_;
    Log& log_;

    uint64_t seq_ = 0;
    Timestamp now_ = 0;
    Timestamp next_mark_ = 0;
    uint64_t next_order_id_ = 0;
    uint64_t bad_messages_ = 0;
    SystemCounts counts_;

    StageLatency lat_;
    uint64_t t0_ = 0, t1_ = 0, t2_ = 0, t5_ = 0;
    bool sent_this_message_ = false;
    bool recording_ = false;
};

}  // namespace ts
