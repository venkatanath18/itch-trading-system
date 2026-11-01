// Stage 1: walk one ITCH day, count the messages by type, and measure what
// the later stages need: the stocks, the peak number of live orders, the
// order reference numbers and the price range of each stock.
//
//   day_summary <itch file> [--md5-file f] [--out results/day_summary.md]
//               [--sizing data/derived/<day>.sizing.csv] [--global data/derived/<day>.global.txt]

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "common/mapped_file.hpp"
#include "itch/parser.hpp"

using namespace ts;
using namespace ts::itch;

namespace {

struct Counts {
    std::array<uint64_t, 256> by_type{};
    uint64_t total = 0;
    uint64_t bytes = 0;
    char first_type = 0;
    char first_event = 0;
    Status status = Status::Ok;
    double seconds = 0;
};

// Framing only: read each length, look at the type byte, move on.
Counts count_messages(const MappedFile& file) {
    Stopwatch watch;
    Counts c;
    Cursor cursor(file.data(), file.size());
    RawMessage m;
    while (cursor.next(m)) {
        if (c.total == 0) {
            c.first_type = m.type();
            c.first_event = load_char(m.data + 11);
        }
        ++c.by_type[static_cast<unsigned char>(m.type())];
        ++c.total;
    }
    c.status = cursor.status();
    c.bytes = cursor.offset();
    c.seconds = watch.seconds();
    return c;
}

struct LiveOrder {
    Locate locate;
    Side side;
    Qty shares;
    Price price;
};

struct StockStats {
    std::string symbol;
    char category = 0;
    uint64_t adds = 0;
    Price min_price = kMaxPrice;
    Price max_price = 0;
    uint32_t orders = 0;
    uint32_t peak_orders = 0;
    std::array<uint32_t, 2> levels{};
    std::array<uint32_t, 2> peak_levels{};
    Price min_traded = kMaxPrice;  // executions during market hours
    Price max_traded = 0;
};

// Tracks every live order in a plain hash map: slow, but simple enough to
// trust, and enough to size the fast book later.
class Survey : public NullHandler {
public:
    Survey() : stocks_(65536) { live_.reserve(1 << 22); }

    void on_system_event(const SystemEvent& e) {
        events_.push_back({e.event_code, e.h.timestamp});
        if (e.event_code == 'Q') market_open_ = true;
        if (e.event_code == 'M') market_open_ = false;
    }

    void on_stock_directory(const StockDirectory& r) {
        ++directory_count_;
        max_locate_ = std::max(max_locate_, r.h.locate);
        stocks_[r.h.locate].symbol = std::string(r.stock.view());
        stocks_[r.h.locate].category = r.market_category;
    }

    void on_add(const AddOrder& a) {
        if (a.ref < min_ref_) min_ref_ = a.ref;
        add(a.h.locate, a.ref, a.side, a.shares, a.price, a.h.timestamp);
    }

    void on_executed(const OrderExecuted& e) {
        auto it = live_.find(e.ref);
        if (it != live_.end()) traded(it->second.locate, it->second.price);
        reduce(e.ref, e.shares, 'E');
    }

    void on_executed_with_price(const OrderExecutedWithPrice& e) {
        auto it = live_.find(e.ref);
        if (it != live_.end()) traded(it->second.locate, e.price);
        reduce(e.ref, e.shares, 'C');
    }

    void on_trade(const Trade& t) { traded(t.h.locate, t.price); }
    void on_cancel(const OrderCancel& x) { reduce(x.ref, x.shares, 'X'); }

    void on_delete(const OrderDelete& d) {
        auto it = live_.find(d.ref);
        if (it == live_.end()) {
            ++unknown_ref_['D'];
            return;
        }
        remove(it);
    }

    void on_replace(const OrderReplace& u) {
        auto it = live_.find(u.old_ref);
        if (it == live_.end()) {
            ++unknown_ref_['U'];
            return;
        }
        const LiveOrder old = it->second;
        if (old.locate != u.h.locate) ++replace_locate_mismatch_;
        remove(it);
        add(old.locate, u.new_ref, old.side, u.shares, u.price, u.h.timestamp);
    }

    // results
    std::vector<StockStats> stocks_;
    std::vector<std::pair<char, Timestamp>> events_;
    uint64_t directory_count_ = 0;
    Locate max_locate_ = 0;
    uint64_t peak_live_ = 0;
    Timestamp peak_time_ = 0;
    uint64_t live_at_end_ = 0;
    OrderRef min_ref_ = UINT64_MAX;
    OrderRef max_ref_ = 0;
    uint64_t new_refs_ = 0;
    uint64_t refs_out_of_order_ = 0;
    uint64_t duplicate_refs_ = 0;
    std::array<uint64_t, 256> unknown_ref_{};
    uint64_t replace_locate_mismatch_ = 0;
    uint64_t subpenny_at_or_above_dollar_ = 0;
    uint64_t zero_shares_ = 0;
    uint64_t over_executions_ = 0;

    uint64_t live_now() const { return live_.size(); }

private:
    void traded(Locate locate, Price price) {
        if (!market_open_) return;
        StockStats& s = stocks_[locate];
        s.min_traded = std::min(s.min_traded, price);
        s.max_traded = std::max(s.max_traded, price);
    }

    bool market_open_ = false;
    static uint64_t level_key(Locate locate, Side side, Price price) {
        return (uint64_t{locate} << 33) | (uint64_t{static_cast<uint8_t>(side)} << 32) | price;
    }

    void add(Locate locate, OrderRef ref, Side side, Qty shares, Price price, Timestamp ts) {
        ++new_refs_;
        if (ref <= max_ref_) ++refs_out_of_order_;
        max_ref_ = std::max(max_ref_, ref);
        if (shares == 0) ++zero_shares_;
        if (price >= kPriceScale && price % 100 != 0) ++subpenny_at_or_above_dollar_;

        auto [it, inserted] = live_.try_emplace(ref, LiveOrder{locate, side, shares, price});
        if (!inserted) {
            ++duplicate_refs_;
            return;
        }
        if (live_.size() > peak_live_) {
            peak_live_ = live_.size();
            peak_time_ = ts;
        }

        StockStats& s = stocks_[locate];
        ++s.adds;
        s.min_price = std::min(s.min_price, price);
        s.max_price = std::max(s.max_price, price);
        s.peak_orders = std::max(s.peak_orders, ++s.orders);
        const auto si = static_cast<size_t>(side);
        if (++level_orders_[level_key(locate, side, price)] == 1) {
            s.peak_levels[si] = std::max(s.peak_levels[si], ++s.levels[si]);
        }
    }

    void remove(std::unordered_map<OrderRef, LiveOrder>::iterator it) {
        const LiveOrder& o = it->second;
        StockStats& s = stocks_[o.locate];
        --s.orders;
        auto level = level_orders_.find(level_key(o.locate, o.side, o.price));
        if (--level->second == 0) {
            level_orders_.erase(level);
            --s.levels[static_cast<size_t>(o.side)];
        }
        live_.erase(it);
    }

    void reduce(OrderRef ref, Qty shares, char type) {
        auto it = live_.find(ref);
        if (it == live_.end()) {
            ++unknown_ref_[static_cast<unsigned char>(type)];
            return;
        }
        if (shares > it->second.shares) ++over_executions_;
        if (shares >= it->second.shares) {
            remove(it);
        } else {
            it->second.shares -= shares;
        }
    }

    std::unordered_map<OrderRef, LiveOrder> live_;
    std::unordered_map<uint64_t, uint32_t> level_orders_;
};

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

std::string base_name(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string read_md5(const std::string& path) {
    std::ifstream in(path);
    std::string md5;
    in >> md5;
    return md5.empty() ? "not checked" : md5;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: day_summary <itch file> [--md5-file f] [--out f] [--sizing f]\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string out_path = arg_value(argc, argv, "--out", "results/day_summary.md");
    const std::string sizing_path = arg_value(argc, argv, "--sizing", "");
    const std::string global_path = arg_value(argc, argv, "--global", "");
    const std::string md5 = read_md5(arg_value(argc, argv, "--md5-file", ""));

    MappedFile file(path);
    std::cerr << "counting " << with_commas(file.size()) << " bytes ...\n";
    const Counts first = count_messages(file);
    const Counts second = count_messages(file);
    const bool repeatable = first.by_type == second.by_type && first.total == second.total;

    std::cerr << "tracking live orders ...\n";
    Survey survey;
    Stopwatch survey_watch;
    const ParseResult parsed = parse(file.data(), file.size(), survey);
    const double survey_seconds = survey_watch.seconds();
    survey.live_at_end_ = survey.live_now();

    uint64_t typed_sum = 0;
    for (uint64_t c : first.by_type) typed_sum += c;

    std::ofstream out(out_path);
    out << "# Stage 1: the day\n\n"
        << "Written by `day_summary`.\n\n"
        << "- File: `" << base_name(path) << "`, " << with_commas(file.size()) << " bytes decompressed\n"
        << "- MD5 of the `.gz` download: `" << md5 << "` (Nasdaq's `.md5sum` file returned 404; "
        << "gzip's CRC-32 check passed while decompressing)\n"
        << machine_description() << "\n";

    out << "## Checks\n\n"
        << "| Check | Result |\n|---|---|\n"
        << "| First message is a System Event | " << (first.first_type == 'S' ? "yes" : "NO")
        << " (type `" << first.first_type << "`, event code `" << first.first_event << "`) |\n"
        << "| Framing reaches the end of the file | " << to_string(first.status) << ", "
        << with_commas(first.bytes) << " of " << with_commas(file.size()) << " bytes |\n"
        << "| Counts by type add up to the total | " << (typed_sum == first.total ? "yes" : "NO")
        << " (" << with_commas(typed_sum) << " = " << with_commas(first.total) << ") |\n"
        << "| A second run gives the same counts | " << (repeatable ? "yes" : "NO") << " |\n"
        << "| Every message decodes | " << to_string(parsed.status) << ", "
        << with_commas(parsed.messages) << " messages |\n\n";

    out << "## Messages by type\n\n| Type | Name | Count | Share |\n|---|---|---:|---:|\n";
    std::vector<std::pair<uint64_t, char>> by_count;
    for (int t = 0; t < 256; ++t) {
        if (first.by_type[static_cast<size_t>(t)]) by_count.push_back({first.by_type[static_cast<size_t>(t)], static_cast<char>(t)});
    }
    std::sort(by_count.rbegin(), by_count.rend());
    for (auto [count, type] : by_count) {
        out << "| `" << type << "` | " << message_name(type) << " | " << with_commas(count) << " | "
            << fixed(100.0 * static_cast<double>(count) / static_cast<double>(first.total), 2) << "% |\n";
    }
    out << "| | **Total** | **" << with_commas(first.total) << "** | |\n\n";

    out << "## Run time\n\n"
        << "| Pass | Seconds | Messages per second |\n|---|---:|---:|\n"
        << "| Walk the framing and count by type (run 1) | " << fixed(first.seconds, 2) << " | "
        << with_commas(static_cast<uint64_t>(static_cast<double>(first.total) / first.seconds)) << " |\n"
        << "| Walk the framing and count by type (run 2) | " << fixed(second.seconds, 2) << " | "
        << with_commas(static_cast<uint64_t>(static_cast<double>(second.total) / second.seconds)) << " |\n"
        << "| Decode everything and track live orders in `std::unordered_map` | "
        << fixed(survey_seconds, 2) << " | "
        << with_commas(static_cast<uint64_t>(static_cast<double>(parsed.messages) / survey_seconds)) << " |\n\n"
        << "The file is larger than this machine's memory, so the walks read it from the SSD.\n\n";

    out << "## Session\n\n| Event | Code | Time |\n|---|---|---|\n";
    for (auto [code, ts] : survey.events_) {
        const char* name = code == 'O' ? "Start of messages" : code == 'S' ? "Start of system hours"
                         : code == 'Q' ? "Start of market hours" : code == 'M' ? "End of market hours"
                         : code == 'E' ? "End of system hours" : code == 'C' ? "End of messages" : "other";
        out << "| " << name << " | `" << code << "` | " << clock_time(ts) << " |\n";
    }

    uint64_t active_stocks = 0;
    for (const auto& s : survey.stocks_) active_stocks += s.adds > 0;
    const double span = static_cast<double>(survey.max_ref_ - survey.min_ref_ + 1);

    out << "\n## What the later stages need\n\n"
        << "| Quantity | Value |\n|---|---:|\n"
        << "| Stock Directory messages (stocks) | " << with_commas(survey.directory_count_) << " |\n"
        << "| Largest stock locate code | " << survey.max_locate_ << " |\n"
        << "| Stocks with at least one order | " << with_commas(active_stocks) << " |\n"
        << "| **Peak number of live orders** | **" << with_commas(survey.peak_live_) << "** at "
        << clock_time(survey.peak_time_) << " |\n"
        << "| Live orders at the end of the day | " << with_commas(survey.live_at_end_) << " |\n"
        << "| Order reference numbers used (adds + replacements) | " << with_commas(survey.new_refs_) << " |\n"
        << "| Smallest / largest reference number | " << with_commas(survey.min_ref_) << " / "
        << with_commas(survey.max_ref_) << " |\n"
        << "| Density: numbers used / (largest - smallest + 1) | "
        << fixed(static_cast<double>(survey.new_refs_) / span, 3) << " |\n"
        << "| New reference number not above all earlier ones | " << with_commas(survey.refs_out_of_order_) << " |\n"
        << "| Reference number added twice while live | " << with_commas(survey.duplicate_refs_) << " |\n"
        << "| E / C / X / D / U naming an unknown order | " << survey.unknown_ref_['E'] << " / "
        << survey.unknown_ref_['C'] << " / " << survey.unknown_ref_['X'] << " / " << survey.unknown_ref_['D']
        << " / " << survey.unknown_ref_['U'] << " |\n"
        << "| Executions or cancels larger than the order | " << survey.over_executions_ << " |\n"
        << "| Replace whose header locate differs from the order's | " << survey.replace_locate_mismatch_ << " |\n"
        << "| Orders priced at $1 or more but not in whole cents | " << survey.subpenny_at_or_above_dollar_ << " |\n"
        << "| Orders with zero shares | " << survey.zero_shares_ << " |\n\n";

    // price ranges and book depth, per stock
    std::vector<const StockStats*> active;
    for (const auto& s : survey.stocks_) {
        if (s.adds > 0) active.push_back(&s);
    }
    auto quantile = [](std::vector<uint64_t> v, double q) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0 : v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
    };
    std::vector<uint64_t> levels, orders, range_ticks, traded_ticks;
    uint64_t stub_stocks = 0;
    for (const StockStats* s : active) {
        levels.push_back(std::max(s->peak_levels[0], s->peak_levels[1]));
        orders.push_back(s->peak_orders);
        range_ticks.push_back((s->max_price - s->min_price) / tick_size(s->max_price));
        if (s->min_price <= 100 && s->max_price >= 1'000'000'000) ++stub_stocks;
        if (s->max_traded > 0) traded_ticks.push_back((s->max_traded - s->min_traded) / tick_size(s->max_traded));
    }
    uint64_t total_peak_levels = 0;
    for (const StockStats* s : active) total_peak_levels += s->peak_levels[0] + s->peak_levels[1];

    out << "### Per stock (stocks with orders)\n\n"
        << "| Per stock | Median | 99th percentile | Max |\n|---|---:|---:|---:|\n"
        << "| Peak live orders | " << quantile(orders, 0.5) << " | " << quantile(orders, 0.99) << " | "
        << quantile(orders, 1.0) << " |\n"
        << "| Peak price levels on one side | " << quantile(levels, 0.5) << " | " << quantile(levels, 0.99)
        << " | " << quantile(levels, 1.0) << " |\n"
        << "| Range of all order prices, in ticks | " << with_commas(quantile(range_ticks, 0.5)) << " | "
        << with_commas(quantile(range_ticks, 0.99)) << " | " << with_commas(quantile(range_ticks, 1.0)) << " |\n"
        << "| Range of execution prices in market hours, in ticks | " << with_commas(quantile(traded_ticks, 0.5))
        << " | " << with_commas(quantile(traded_ticks, 0.99)) << " | " << with_commas(quantile(traded_ticks, 1.0))
        << " |\n\n"
        << with_commas(stub_stocks) << " of " << with_commas(active.size())
        << " stocks have orders both at $0.01 or less and at $100,000 or more: stub quotes, which "
           "market makers leave far from the market. So a flat array over a stock's whole price range "
           "is out of the question, while the prices that actually trade span a few hundred ticks.\n\n"
        << "Sum over stocks of the peak level count of each side: " << with_commas(total_peak_levels) << ".\n\n";

    std::sort(active.begin(), active.end(), [](auto* a, auto* b) { return a->adds > b->adds; });
    out << "### Most active stocks, and the ones used later\n\n"
        << "| Symbol | Adds | Traded low | Traded high | Peak live orders | Peak bid levels | Peak ask levels |\n"
        << "|---|---:|---:|---:|---:|---:|---:|\n";
    auto row = [&](const StockStats& s) {
        out << "| " << s.symbol << " | " << with_commas(s.adds) << " | " << dollars(s.min_traded) << " | "
            << dollars(s.max_traded) << " | " << with_commas(s.peak_orders) << " | " << s.peak_levels[0]
            << " | " << s.peak_levels[1] << " |\n";
    };
    const size_t top = std::min<size_t>(10, active.size());
    for (size_t i = 0; i < top; ++i) row(*active[i]);
    for (const char* name : {"AAPL", "MSFT", "INTC", "CSCO", "AMZN"}) {
        for (size_t i = top; i < active.size(); ++i) {
            if (active[i]->symbol == name) row(*active[i]);
        }
    }

    if (!sizing_path.empty()) {
        std::ofstream sizing(sizing_path);
        sizing << "locate,symbol,adds,min_price,max_price,peak_orders,peak_bid_levels,peak_ask_levels,"
                  "min_traded,max_traded\n";
        for (size_t i = 0; i < survey.stocks_.size(); ++i) {
            const StockStats& s = survey.stocks_[i];
            if (s.symbol.empty() && s.adds == 0) continue;
            sizing << i << ',' << s.symbol << ',' << s.adds << ',' << (s.adds ? s.min_price : 0) << ','
                   << s.max_price << ',' << s.peak_orders << ',' << s.peak_levels[0] << ','
                   << s.peak_levels[1] << ',' << (s.max_traded ? s.min_traded : 0) << ',' << s.max_traded
                   << '\n';
        }
    }

    if (!global_path.empty()) {
        std::ofstream global(global_path);
        global << "messages " << first.total << "\n"
               << "peak_live_orders " << survey.peak_live_ << "\n"
               << "max_ref " << survey.max_ref_ << "\n"
               << "max_locate " << survey.max_locate_ << "\n";
    }

    std::cerr << "messages " << with_commas(first.total) << ", peak live orders "
              << with_commas(survey.peak_live_) << ", wrote " << out_path << "\n";
    return first.first_type == 'S' && repeatable && parsed.status == Status::Ok ? 0 : 1;
}
