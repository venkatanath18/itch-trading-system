// Stage 3: replay the day through the reference book and check its
// invariants after every message.
//
//   book_check <itch file> [--out results/reference_book.md]
//              [--snapshot-symbol AAPL] [--snapshot-time 10:00:00]

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "book/reference_book.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "itch/session.hpp"
#include "itch/source.hpp"

using namespace ts;

namespace {

// How far from the best price on its own side an order arrives or leaves,
// in ticks. Decides the width of a price array around the best.
class DistanceHistogram {
public:
    static constexpr std::array<int64_t, 13> kEdges = {0, 1, 2, 3, 4, 5, 10, 20, 50, 100, 200, 500, 1000};

    void record(const TopOfBook& top, Side side, Price price) {
        const bool empty = side == Side::Buy ? top.bid_shares == 0 : top.ask_shares == 0;
        if (empty) {
            ++empty_side_;
            return;
        }
        const int64_t best = side == Side::Buy ? top.bid : top.ask;
        const int64_t away = side == Side::Buy ? best - int64_t{price} : int64_t{price} - best;
        if (away < 0) {
            ++improves_;
            return;
        }
        const int64_t ticks = away / int64_t{tick_size(std::max<Price>(price, static_cast<Price>(best)))};
        size_t bucket = kEdges.size() - 1;  // the last bucket is open-ended
        for (size_t i = 0; i + 1 < kEdges.size(); ++i) {
            if (ticks < kEdges[i + 1]) {
                bucket = i;
                break;
            }
        }
        ++counts_[bucket];
    }

    void write(std::ostream& out, const char* what) const {
        uint64_t total = improves_ + empty_side_;
        for (uint64_t c : counts_) total += c;
        out << "| Ticks from the best on its side | " << what << " | Share | Cumulative |\n|---|---:|---:|---:|\n";
        double cumulative = 0;
        auto line = [&](const std::string& label, uint64_t c) {
            double share = 100.0 * static_cast<double>(c) / static_cast<double>(total);
            cumulative += share;
            out << "| " << label << " | " << with_commas(c) << " | " << fixed(share, 2) << "% | "
                << fixed(cumulative, 2) << "% |\n";
        };
        line("better than the best (a new best)", improves_);
        for (size_t i = 0; i < kEdges.size(); ++i) {
            std::string label;
            if (i + 1 == kEdges.size()) {
                label = std::to_string(kEdges[i]) + " or more";
            } else if (kEdges[i + 1] - kEdges[i] == 1) {
                label = std::to_string(kEdges[i]);
            } else {
                label = std::to_string(kEdges[i]) + " to " + std::to_string(kEdges[i + 1] - 1);
            }
            line(label, counts_[i]);
        }
        line("side was empty", empty_side_);
        out << "\n";
    }

private:
    std::array<uint64_t, kEdges.size()> counts_{};
    uint64_t improves_ = 0;
    uint64_t empty_side_ = 0;
};

struct Episode {
    Locate locate = 0;
    Timestamp start = 0;
    Timestamp end = 0;
    Price bid = 0;
    Price ask = 0;
    char state = 'T';
    bool open = false;
};

class Checker : public itch::NullHandler {
public:
    Checker(std::string snapshot_symbol, Timestamp snapshot_time)
        : snapshot_symbol_(std::move(snapshot_symbol)), snapshot_time_(snapshot_time),
          symbols_(65536), states_(65536, '?'), open_(65536) {}

    void on_system_event(const itch::SystemEvent& e) {
        session_.on_system_event(e);
        if (e.event_code == 'Q') {
            market_open_time_ = e.h.timestamp;
            for (size_t l = 0; l < symbols_.size(); ++l) watch_cross(static_cast<Locate>(l), e.h.timestamp);
        }
        if (e.event_code == 'M') {
            for (Episode& ep : open_) {
                if (ep.open) close(ep, e.h.timestamp);
            }
        }
    }

    void on_stock_directory(const itch::StockDirectory& r) {
        symbols_[r.h.locate] = std::string(r.stock.view());
        if (symbols_[r.h.locate] == snapshot_symbol_) snapshot_locate_ = r.h.locate;
    }

    void on_trading_action(const itch::TradingAction& a) { states_[a.h.locate] = a.state; }

    void on_add(const itch::AddOrder& m) {
        before(m.h.timestamp);
        adds_.record(book_.top(m.h.locate), m.side, m.price);
        book_.add(m.h.locate, m.ref, m.side, m.shares, m.price);
        after(m.h.locate, m.side, m.price, m.h.timestamp);
    }

    void on_executed(const itch::OrderExecuted& m) { reduce(m.ref, m.h.timestamp, [&] { book_.execute(m.ref, m.shares); }); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        reduce(m.ref, m.h.timestamp, [&] { book_.execute(m.ref, m.shares); });
    }
    void on_cancel(const itch::OrderCancel& m) { reduce(m.ref, m.h.timestamp, [&] { book_.cancel(m.ref, m.shares); }); }
    void on_delete(const itch::OrderDelete& m) { reduce(m.ref, m.h.timestamp, [&] { book_.remove(m.ref); }); }

    void on_replace(const itch::OrderReplace& m) {
        before(m.h.timestamp);
        Locate locate;
        Side side;
        Price old_price;
        if (!book_.find(m.old_ref, locate, side, old_price)) {
            violation(m.h.timestamp, "replace of an unknown order");
            return;
        }
        removals_.record(book_.top(locate), side, old_price);
        adds_.record(book_.top(locate), side, m.price);
        book_.replace(m.old_ref, m.new_ref, m.shares, m.price);
        check_level(locate, side, old_price, m.h.timestamp);
        after(locate, side, m.price, m.h.timestamp);
    }

    void finish(Timestamp end) {
        std::string why;
        ++full_checks_;
        if (!book_.check_all(&why)) violation(end, "full check: " + why);
    }

    ReferenceBook book_;
    itch::Session session_;
    DistanceHistogram adds_;
    DistanceHistogram removals_;
    std::vector<Episode> episodes_;
    std::vector<std::string> violations_;
    uint64_t violation_count_ = 0;
    uint64_t checked_messages_ = 0;
    uint64_t full_checks_ = 0;
    Timestamp market_open_time_ = 0;
    std::string snapshot_symbol_;
    Timestamp snapshot_time_;
    std::string snapshot_;
    std::vector<std::string> symbols_;

private:
    template <class Apply>
    void reduce(OrderRef ref, Timestamp ts, Apply apply) {
        before(ts);
        Locate locate;
        Side side;
        Price price;
        if (!book_.find(ref, locate, side, price)) {
            violation(ts, "message for an unknown order");
            return;
        }
        removals_.record(book_.top(locate), side, price);
        apply();
        after(locate, side, price, ts);
    }

    void before(Timestamp ts) {
        if (snapshot_.empty() && ts >= snapshot_time_ && snapshot_locate_ != 0) take_snapshot(ts);
        if (++checked_messages_ % 20'000'000 == 0) finish(ts);
    }

    void after(Locate locate, Side side, Price price, Timestamp ts) {
        check_level(locate, side, price, ts);
        if (session_.regular_hours()) watch_cross(locate, ts);
    }

    void check_level(Locate locate, Side side, Price price, Timestamp ts) {
        std::string why;
        if (!book_.check_level(locate, side, price, &why)) violation(ts, symbols_[locate] + ": " + why);
    }

    // Best bid at or above best ask: open an episode, or close it when it ends.
    void watch_cross(Locate locate, Timestamp ts) {
        const TopOfBook t = book_.top(locate);
        const bool crossed = t.two_sided() && t.bid >= t.ask;
        Episode& ep = open_[locate];
        if (crossed && !ep.open) {
            ep = Episode{locate, ts, 0, t.bid, t.ask, states_[locate], true};
        } else if (!crossed && ep.open) {
            close(ep, ts);
        }
    }

    void close(Episode& ep, Timestamp ts) {
        ep.end = ts;
        ep.open = false;
        episodes_.push_back(ep);
    }

    void violation(Timestamp ts, const std::string& what) {
        ++violation_count_;
        if (violations_.size() < 20) violations_.push_back(clock_time(ts) + " " + what);
    }

    void take_snapshot(Timestamp ts) {
        std::vector<LevelInfo> bids(5), asks(5);
        bids.resize(book_.levels(snapshot_locate_, Side::Buy, 5, bids.data()));
        asks.resize(book_.levels(snapshot_locate_, Side::Sell, 5, asks.data()));
        snapshot_ = "| Bid orders | Bid shares | Bid | Ask | Ask shares | Ask orders |\n|---:|---:|---:|---:|---:|---:|\n";
        for (size_t i = 0; i < 5; ++i) {
            auto cell = [](const std::vector<LevelInfo>& v, size_t k, int field) -> std::string {
                if (k >= v.size()) return "";
                if (field == 0) return std::to_string(v[k].orders);
                if (field == 1) return with_commas(v[k].shares);
                return dollars(v[k].price);
            };
            snapshot_ += "| " + cell(bids, i, 0) + " | " + cell(bids, i, 1) + " | " + cell(bids, i, 2) + " | " +
                         cell(asks, i, 2) + " | " + cell(asks, i, 1) + " | " + cell(asks, i, 0) + " |\n";
        }
        snapshot_ = "The " + snapshot_symbol_ + " book at " + clock_time(ts) + ", top five levels:\n\n" + snapshot_;
    }

    Locate snapshot_locate_ = 0;
    std::vector<char> states_;
    std::vector<Episode> open_;
};

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

Timestamp parse_clock(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return hms(h, m, sec);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: book_check <itch file> [--out f] [--snapshot-symbol S] [--snapshot-time HH:MM:SS]\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string out_path = arg_value(argc, argv, "--out", "results/reference_book.md");
    Checker checker(arg_value(argc, argv, "--snapshot-symbol", "AAPL"),
                    parse_clock(arg_value(argc, argv, "--snapshot-time", "10:00:00")));

    Stopwatch watch;
    const uint64_t messages = itch::replay_file(path, checker);
    checker.finish(hms(20, 5, 0));
    const double seconds = watch.seconds();

    // group the crossed or locked episodes during market hours
    uint64_t at_open = 0, halted = 0, other = 0;
    std::vector<const Episode*> others;
    for (const Episode& ep : checker.episodes_) {
        if (ep.start < checker.market_open_time_ + kNanosPerSecond) {
            ++at_open;
        } else if (ep.state != 'T') {
            ++halted;
        } else {
            ++other;
            others.push_back(&ep);
        }
    }
    std::sort(others.begin(), others.end(), [](auto* a, auto* b) { return a->end - a->start > b->end - b->start; });

    std::ofstream out(out_path);
    out << "# Stage 3: the reference book\n\n"
        << "Written by `book_check`. The whole day replayed through `ReferenceBook` "
        << "(`std::unordered_map` + `std::map` + `std::list`), checking after every message.\n\n"
        << machine_description() << "\n"
        << "## Invariants\n\n"
        << "| Check | Result |\n|---|---|\n"
        << "| Messages replayed | " << with_commas(messages) << " |\n"
        << "| Book messages checked (A F E C X D U) | " << with_commas(checker.checked_messages_) << " |\n"
        << "| After each one: the touched levels are non-empty, each level's total equals the sum of its "
           "orders, no order has zero shares, the order map agrees with the levels | "
        << (checker.violation_count_ == 0 ? "held every time" : "FAILED") << " (" << checker.violation_count_
        << " violations) |\n"
        << "| Full check of every level of every stock, every 20 million messages and at the end | "
        << checker.full_checks_ << " full checks |\n"
        << "| Orders left at the end of the day | " << with_commas(checker.book_.order_count()) << " |\n"
        << "| Book errors (unknown order, duplicate reference, over-execution) | " << checker.book_.errors() << " |\n"
        << "| Run time with all the checks | " << fixed(seconds, 1) << " s |\n\n";
    for (const std::string& v : checker.violations_) out << "- " << v << "\n";

    out << "## Best bid below best ask during market hours\n\n"
        << "An episode starts when a message leaves a stock's best bid at or above its best ask between "
           "the Q and M events, and ends when a later message uncrosses it.\n\n"
        << "| Episodes | Count | Explanation |\n|---|---:|---|\n"
        << "| Starting in the first second after the open | " << at_open
        << " | Orders entered before 9:30 can cross; the opening cross executes them (C messages, "
           "printable N) within milliseconds of the Q event |\n"
        << "| Stock halted, paused or quotation-only (H message state not T) | " << halted
        << " | No continuous matching during a halt, so the book can cross until the reopening cross |\n"
        << "| Any other time | " << other << " | listed below |\n\n";
    if (!others.empty()) {
        out << "| Stock | Start | Lasted | Bid | Ask |\n|---|---|---:|---:|---:|\n";
        for (size_t i = 0; i < std::min<size_t>(15, others.size()); ++i) {
            const Episode& ep = *others[i];
            out << "| " << checker.symbols_[ep.locate] << " | " << clock_time(ep.start) << " | "
                << fixed(static_cast<double>(ep.end - ep.start) / 1e3, 1) << " µs | " << dollars(ep.bid) << " | "
                << dollars(ep.ask) << " |\n";
        }
        out << "\n";
    }

    out << "## Where in the book the activity is\n\n"
        << "Distance of each new order (A, F and the new half of U) from the best price on its own side "
           "just before it arrived:\n\n";
    checker.adds_.write(out, "Orders added");
    out << "Distance of each order that left or shrank (E, C, X, D and the old half of U):\n\n";
    checker.removals_.write(out, "Orders removed or reduced");

    out << "## A snapshot\n\n" << checker.snapshot_ << "\n";
    std::cerr << "violations " << checker.violation_count_ << ", episodes " << checker.episodes_.size()
              << ", " << fixed(seconds, 1) << " s, wrote " << out_path << "\n";
    return checker.violation_count_ == 0 ? 0 : 1;
}
