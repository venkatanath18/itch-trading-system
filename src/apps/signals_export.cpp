// Stage 5: export what the Python cross-check and the regression need.
//
//   signals_export <itch file> --prefix data/derived/<day> [--symbols AAPL,MSFT,INTC,CSCO,AMZN]
//                  [--check-symbol AAPL] [--check-from 09:59:50] [--check-to 11:00:00]
//
// Replays the day through the reference book and feeds every top-of-book
// change to the Signals module (the code the strategy uses). Writes:
//   <prefix>.tob_check.csv      one stock's top-of-book changes for one hour,
//                               with the C++ OFI values
//   <prefix>.intervals_1s.csv   per stock and second of market hours: OFI,
//   <prefix>.intervals_10s.csv  mid at the start and end, average depth

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "book/reference_book.hpp"
#include "common/format.hpp"
#include "itch/source.hpp"
#include "signals/signals.hpp"

using namespace ts;

namespace {

constexpr Timestamp kOpen = hms(9, 30, 0);
constexpr Timestamp kClose = hms(16, 0, 0);

// OFI, depth and mid per fixed interval of market hours, for one stock.
struct Intervals {
    Timestamp width;
    std::vector<int64_t> ofi;
    std::vector<double> depth_sum;
    std::vector<uint64_t> updates;
    std::vector<int64_t> mid_end2;  // -1 until a change lands in the interval
    int64_t open_mid2 = -1;         // mid at the open
    Price tick = 100;

    explicit Intervals(Timestamp w)
        : width(w), ofi(n()), depth_sum(n()), updates(n()), mid_end2(n(), -1) {}
    size_t n() const { return static_cast<size_t>((kClose - kOpen) / width); }

    void record(Timestamp ts, const StockSignals& s) {
        if (ts < kOpen) {
            if (s.top.two_sided()) open_mid2 = static_cast<int64_t>(s.mid2());
            return;
        }
        if (ts >= kClose) return;
        const size_t k = static_cast<size_t>((ts - kOpen) / width);
        ofi[k] += s.last_e;
        if (s.top.two_sided()) {
            depth_sum[k] += static_cast<double>(s.top.bid_shares + s.top.ask_shares) / 2.0;
            ++updates[k];
            mid_end2[k] = static_cast<int64_t>(s.mid2());
            tick = tick_size(s.top.bid);
        }
    }

    void write(std::ostream& out, const std::string& symbol) const {
        int64_t mid = open_mid2;
        double depth = 0;
        for (size_t k = 0; k < ofi.size(); ++k) {
            const int64_t start = mid;
            if (mid_end2[k] >= 0) mid = mid_end2[k];
            if (updates[k] > 0) depth = depth_sum[k] / static_cast<double>(updates[k]);
            // mid2 is twice the mid, so the change in ticks is (end - start) / (2 tick)
            const double dmid = start < 0 ? 0.0 : static_cast<double>(mid - start) / (2.0 * tick);
            out << symbol << ',' << kOpen + k * width << ',' << ofi[k] << ',' << start << ',' << mid << ','
                << dmid << ',' << depth << ',' << updates[k] << '\n';
        }
    }
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

class Exporter : public itch::NullHandler {
public:
    Exporter(std::vector<std::string> symbols, std::string check_symbol, Timestamp from, Timestamp to,
             std::ostream& check_out)
        : wanted_(std::move(symbols)), check_symbol_(std::move(check_symbol)), from_(from), to_(to),
          check_out_(check_out), signals_(65536), book_(65536), roles_(65536, -1) {}

    void on_stock_directory(const itch::StockDirectory& r) {
        const std::string symbol(r.stock.view());
        for (size_t i = 0; i < wanted_.size(); ++i) {
            if (wanted_[i] == symbol) {
                roles_[r.h.locate] = static_cast<int>(i);
                one_s_.emplace(i, Intervals(kNanosPerSecond));
                ten_s_.emplace(i, Intervals(10 * kNanosPerSecond));
            }
        }
        if (symbol == check_symbol_) check_locate_ = r.h.locate;
    }

    void on_add(const itch::AddOrder& m) {
        book_.add(m.h.locate, m.ref, m.side, m.shares, m.price);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_executed(const itch::OrderExecuted& m) {
        book_.execute(m.ref, m.shares);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        book_.execute(m.ref, m.shares);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_cancel(const itch::OrderCancel& m) {
        book_.cancel(m.ref, m.shares);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_delete(const itch::OrderDelete& m) {
        book_.remove(m.ref);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_replace(const itch::OrderReplace& m) {
        book_.replace(m.old_ref, m.new_ref, m.shares, m.price);
        changed(m.h.locate, m.h.timestamp);
    }

    void write_intervals(std::ostream& one, std::ostream& ten) const {
        const char* header = "symbol,start_ns,ofi,mid2_start,mid2_end,dmid_ticks,avg_depth,updates\n";
        one << header;
        ten << header;
        for (size_t i = 0; i < wanted_.size(); ++i) {
            if (!one_s_.count(i)) continue;
            one_s_.at(i).write(one, wanted_[i]);
            ten_s_.at(i).write(ten, wanted_[i]);
        }
    }

    uint64_t check_rows_ = 0;

private:
    void changed(Locate locate, Timestamp ts) {
        const bool is_check = locate == check_locate_ && check_locate_ != 0;
        const TopOfBook before = signals_.stock(locate).top;
        if (!signals_.on_top(locate, book_.top(locate), ts)) return;
        StockSignals& s = signals_.stock(locate);
        if (roles_[locate] >= 0) {
            one_s_.at(static_cast<size_t>(roles_[locate])).record(ts, s);
            ten_s_.at(static_cast<size_t>(roles_[locate])).record(ts, s);
        }
        if (is_check && ts >= from_ && ts < to_) {
            if (check_rows_++ == 0) {
                check_out_ << "kind,ts,bid,bid_qty,ask,ask_qty,e,ofi_1s,ofi_10s\n"
                           << "start," << ts << ',' << before.bid << ',' << before.bid_shares << ',' << before.ask
                           << ',' << before.ask_shares << ",0,0,0\n";
            }
            check_out_ << "change," << ts << ',' << s.top.bid << ',' << s.top.bid_shares << ',' << s.top.ask << ','
                       << s.top.ask_shares << ',' << s.last_e << ',' << s.ofi_1s.sum(ts) << ','
                       << s.ofi_10s.sum(ts) << '\n';
        }
    }

    std::vector<std::string> wanted_;
    std::string check_symbol_;
    Timestamp from_, to_;
    std::ostream& check_out_;
    Signals signals_;
    ReferenceBook book_;
    std::vector<int> roles_;
    std::map<size_t, Intervals> one_s_, ten_s_;
    Locate check_locate_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: signals_export <itch file> --prefix <path prefix> [--symbols A,B] [--check-symbol S] "
                     "[--check-from HH:MM:SS] [--check-to HH:MM:SS]\n";
        return 2;
    }
    const std::string prefix = arg_value(argc, argv, "--prefix", "data/derived/day");
    std::vector<std::string> symbols;
    std::stringstream list(arg_value(argc, argv, "--symbols", "AAPL,MSFT,INTC,CSCO,AMZN"));
    for (std::string s; std::getline(list, s, ',');) symbols.push_back(s);

    std::ofstream check(prefix + ".tob_check.csv");
    Exporter exporter(symbols, arg_value(argc, argv, "--check-symbol", "AAPL"),
                      parse_clock(arg_value(argc, argv, "--check-from", "09:59:50")),
                      parse_clock(arg_value(argc, argv, "--check-to", "11:00:00")), check);
    itch::replay_file(argv[1], exporter);

    std::ofstream one(prefix + ".intervals_1s.csv"), ten(prefix + ".intervals_10s.csv");
    exporter.write_intervals(one, ten);
    std::cerr << "check rows " << with_commas(exporter.check_rows_) << ", wrote " << prefix << ".*.csv\n";
    return 0;
}
