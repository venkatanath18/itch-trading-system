// Stages 6 to 10: run the whole system over the day.
//
//   trade <itch file> --sizing <csv> --global <txt> --params <csv> [options]
//     --label NAME          name of this run in the results
//     --latency-us N        one-way latency to the exchange and back (default 0)
//     --no-kill-switch      no loss budget and no kill switch
//     --ofi                 shift the reservation price by the OFI forecast
//     --loss-limit DOLLARS  L for the kill switch (default 500)
//     --gamma-scale X       multiply each stock's gamma (default 1)
//     --threads             log through the SPSC queue to a second thread
//     --pin CORE            pin the hot path to CORE and the logger to CORE+1 (Linux)
//     --measure             timestamps at every stage boundary
//     --log FILE            write the output log
//     --runs-csv FILE       append a summary row (default results/raw/trade_runs.csv)
//     --stocks-csv FILE     append one row per stock (default results/raw/trade_stocks.csv)
//     --latency-csv FILE    append the stage latencies when measuring
//
//   trade --report <runs csv> --stocks <stocks csv> --out results/strategy.md
//   trade --latency-report <latency csv> --runs <runs csv> --out results/latency.md

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/sorted_levels.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "infra/log.hpp"
#include "itch/source.hpp"
#include "sim/system.hpp"

#ifdef COUNT_ALLOCATIONS
#include "alloc_counter.hpp"
#endif

using namespace ts;

namespace {

using Book = FastBook<FlatIndex, SortedLevels>;

struct Options {
    std::string itch, sizing, global, params, label = "run", log, runs_csv = "results/raw/trade_runs.csv",
                stocks_csv = "results/raw/trade_stocks.csv", latency_csv;
    double latency_us = 0, loss_limit = 500, gamma_scale = 1;
    bool kill_switch = true, ofi = false, threads = false, measure = false;
    int pin = -1;
};

struct StockParams {
    std::string symbol;
    Locate locate = 0;
    double sigma = 0, k = 0, ofi_beta = 0, gamma = 0;
};

std::vector<StockParams> read_params(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path + " (run calibrate first)");
    std::vector<StockParams> v;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::stringstream row(line);
        std::string f[8];
        for (auto& x : f) std::getline(row, x, ',');
        // symbol,locate,tick,sigma,k,A,ofi_beta,gamma
        v.push_back({f[0], static_cast<Locate>(std::stoul(f[1])), std::stod(f[3]), std::stod(f[4]), std::stod(f[6]),
                     std::stod(f[7])});
    }
    return v;
}

double dollars_of(int64_t units) { return static_cast<double>(units) / kPriceScale; }

template <class Log, bool Measure>
int run(const Options& o, const std::vector<StockParams>& params) {
    const BookSizing sizing = BookSizing::from_profile(o.sizing, o.global);
    SystemConfig cfg;
    for (const StockParams& p : params) {
        MarketMakerConfig m;
        m.locate = p.locate;
        m.as = as::Params{p.gamma * o.gamma_scale, p.sigma, p.k};
        m.ofi_dollars_per_share = p.ofi_beta;
        m.use_ofi = o.ofi;
        cfg.makers.push_back(m);
    }
    cfg.risk.kill_switch = o.kill_switch;
    cfg.risk.loss_limit = static_cast<int64_t>(o.loss_limit * kPriceScale);
    cfg.latency = static_cast<Timestamp>(o.latency_us * 1000);

    if (o.pin >= 0) pin_this_thread(o.pin);
    Log log = [&] {
        if constexpr (std::is_same_v<Log, ThreadedLog>) return Log(o.log, o.pin >= 0 ? o.pin + 1 : -1);
        else return Log(o.log);
    }();
    auto system = std::make_unique<TradingSystem<Book, Log, Measure>>(cfg, sizing, log);
    itch::MessageSource source(o.itch);

#ifdef COUNT_ALLOCATIONS
    const uint64_t allocs_before = allocation_count();
#endif
    Stopwatch watch;
    system->run(source);
    const double seconds = watch.seconds();
#ifdef COUNT_ALLOCATIONS
    const uint64_t hot_path_allocations = allocation_count() - allocs_before;
#endif
    log.finish();

    // results
    const auto& counts = system->counts();
    const RiskGate& risk = system->risk();
    double pnl = 0;
    uint64_t fills = 0, volume = 0;
    int64_t max_inventory = 0;
    std::ofstream stocks(o.stocks_csv, std::ios::app);
    for (const StockParams& p : params) {
        const Holding& h = system->portfolio().holding(p.locate);
        int64_t mid2 = system->close_mid2(p.locate);
        if (mid2 == 0) mid2 = static_cast<int64_t>(system->signals().stock(p.locate).last_mid2);
        const double stock_pnl = dollars_of(h.cash + h.position * mid2 / 2);
        pnl += stock_pnl;
        fills += h.fills;
        volume += h.bought + h.sold;
        max_inventory = std::max(max_inventory, h.max_abs_position);
        stocks << o.label << ',' << p.symbol << ',' << fixed(stock_pnl, 2) << ',' << h.fills << ',' << h.bought << ','
               << h.sold << ',' << h.position << ',' << h.max_abs_position << '\n';
    }
    std::ofstream runs(o.runs_csv, std::ios::app);
    runs << o.label << ',' << o.latency_us << ',' << o.kill_switch << ',' << o.ofi << ',' << o.gamma_scale << ','
         << o.loss_limit << ',' << Log::name << ',' << fixed(pnl, 2) << ',' << fills << ',' << volume << ','
         << max_inventory << ',' << fixed(dollars_of(risk.max_drawdown()), 2) << ',' << risk.killed() << ','
         << (risk.killed() ? clock_time(risk.killed_at()) : "-") << ',' << counts.orders << ',' << counts.cancels << ','
         << counts.risk_rejects << ',' << counts.exchange_rejects << ',' << counts.events << ',' << std::hex
         << log.hash() << std::dec << ',' << log.records() << ',' << fixed(seconds, 2) << ',' << log.full_waits()
         << ',' << counts.dropped << '\n';

    if constexpr (Measure) {
        if (!o.latency_csv.empty()) {
            std::ofstream lat(o.latency_csv, std::ios::app);
            const StageLatency& l = system->latency();
            auto row = [&](const char* stage, const Histogram& h) {
                lat << o.label << ',' << stage << ',' << h.count() << ',' << ticks_to_ns(h.mean()) << ','
                    << ticks_to_ns(static_cast<double>(h.percentile(0.5))) << ','
                    << ticks_to_ns(static_cast<double>(h.percentile(0.99))) << ','
                    << ticks_to_ns(static_cast<double>(h.percentile(0.999))) << ','
                    << ticks_to_ns(static_cast<double>(h.max())) << ',' << seconds << '\n';
            };
            row("book", l.book);
            row("signals", l.signals);
            row("strategy", l.strategy);
            row("risk", l.risk);
            row("send", l.send);
            row("to_order", l.to_order);
            row("message", l.message);
        }
    }

    std::cerr << o.label << ": P&L $" << fixed(pnl, 2) << ", fills " << fills << ", max inventory " << max_inventory
              << ", kill " << (risk.killed() ? clock_time(risk.killed_at()) : "no") << ", log " << std::hex << log.hash()
              << std::dec << " (" << log.records() << " lines), " << fixed(seconds, 1) << " s\n";
#ifdef COUNT_ALLOCATIONS
    std::cerr << "allocations during the run: " << hot_path_allocations << "\n";
    return hot_path_allocations == 0 ? 0 : 1;
#else
    return 0;
#endif
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream s(line);
    for (std::string f; std::getline(s, f, ',');) out.push_back(f);
    return out;
}

int report(const std::string& runs_path, const std::string& stocks_path, const std::string& out_path) {
    std::ifstream runs(runs_path), stocks(stocks_path);
    std::map<std::string, std::vector<std::string>> by_label;
    std::vector<std::string> order;
    for (std::string line; std::getline(runs, line);) {
        auto f = split(line);
        if (f.size() < 24) continue;
        if (!by_label.count(f[0])) order.push_back(f[0]);
        by_label[f[0]] = f;  // the latest run of each label
    }
    std::map<std::string, std::vector<std::vector<std::string>>> stock_rows;
    for (std::string line; std::getline(stocks, line);) {
        auto f = split(line);
        if (f.size() >= 8) stock_rows[f[0]].push_back(f);
    }
    std::map<std::string, std::vector<std::vector<std::string>>> latest_stocks;
    for (auto& [label, rows] : stock_rows) {
        // the last block of rows written for this label
        std::map<std::string, std::vector<std::string>> by_symbol;
        std::vector<std::string> symbols;
        for (auto& r : rows) {
            if (!by_symbol.count(r[1])) symbols.push_back(r[1]);
            by_symbol[r[1]] = r;
        }
        for (auto& s : symbols) latest_stocks[label].push_back(by_symbol[s]);
    }

    std::ofstream out(out_path);
    out << "# Stage 11: the strategy over the day\n\n"
        << "Written by `trade --report` from `" << runs_path << "` and `" << stocks_path
        << "`. Avellaneda-Stoikov market making with 100-share orders on the stocks in `results/calibration.md`, "
           "trading only between the Q and M system events. Fills come from the exchange simulator (queue "
           "position from the replayed order flow, no market impact). P&L marks any inventory left at 16:00 "
           "to the mid, as in the model; there are no fees or rebates.\n\n"
        << machine_description() << "\n"
        << "| Run | Latency | Kill switch | OFI skew | γ scale | P&L $ | Fills | Shares | Max inventory | Max drawdown $ "
           "| Kill fired | Orders | Cancels | Risk rejects | Exchange rejects |\n"
        << "|---|---:|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|\n";
    uint64_t fired = 0, with_switch = 0;
    for (const std::string& label : order) {
        const auto& f = by_label[label];
        // label,latency,kill,ofi,gamma,L,log,pnl,fills,volume,maxinv,maxdd,killed,killtime,orders,cancels,riskrej,exrej,events,hash,records,seconds,waits,dropped
        const bool killed = f[12] == "1";
        if (f[2] == "1") {
            ++with_switch;
            fired += killed;
        }
        out << "| " << label << " | " << f[1] << " µs | " << (f[2] == "1" ? "on, L = $" + f[5] : "off") << " | "
            << (f[3] == "1" ? "on" : "off") << " | " << f[4] << " | " << f[7] << " | " << f[8] << " | " << f[9] << " | "
            << f[10] << " | " << f[11] << " | " << (killed ? "yes, " + f[13] : "no") << " | " << f[14] << " | " << f[15]
            << " | " << f[16] << " | " << f[17] << " |\n";
    }
    out << "\nThe kill switch fired in " << fired << " of the " << with_switch << " runs that had it on.\n\n";

    // the same configuration run again, and with the log on a second thread
    auto hash_of = [&](const std::string& label) -> std::string {
        return by_label.count(label) ? by_label[label][19] : "missing";
    };
    auto lines_of = [&](const std::string& label) -> std::string {
        return by_label.count(label) ? by_label[label][20] : "missing";
    };
    const std::string base = hash_of("base"), again = hash_of("base-again"), threaded = hash_of("base-threaded");
    out << "## Determinism\n\n"
        << "The output log (every order, risk decision, exchange response, kill-switch event and minute mark, "
           "each with its sequencer number) is hashed with FNV-1a as it is written.\n\n"
        << "| Run | Log lines | Log hash |\n|---|---:|---|\n"
        << "| base | " << lines_of("base") << " | `" << base << "` |\n"
        << "| base, run again | " << lines_of("base-again") << " | `" << again << "` |\n"
        << "| base, log written by a second thread through the SPSC queue | " << lines_of("base-threaded") << " | `"
        << threaded << "` |\n\n"
        << "Same hash on the second run: **" << (base == again && base != "missing" ? "yes" : "NO") << "**. "
        << "Threaded log identical to the single-threaded one: **"
        << (base == threaded && base != "missing" ? "yes" : "NO") << "**.\n\n";

    for (const std::string& label : order) {
        if (!latest_stocks.count(label)) continue;
        out << "### " << label << ", by stock\n\n| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |\n"
            << "|---|---:|---:|---:|---:|---:|---:|\n";
        for (auto& r : latest_stocks[label]) {
            out << "| " << r[1] << " | " << r[2] << " | " << r[3] << " | " << r[4] << " | " << r[5] << " | " << r[6]
                << " | " << r[7] << " |\n";
        }
        out << "\n";
    }
    std::cerr << "wrote " << out_path << "\n";
    return 0;
}

// Back-to-back counter reads: the floor under every timed stage.
double timer_overhead_ns() {
    Histogram h;
    for (int i = 0; i < 1'000'000; ++i) {
        const uint64_t t0 = read_counter();
        h.record(read_counter() - t0);
    }
    return ticks_to_ns(h.mean());
}

int latency_report(const std::string& latency_path, const std::string& runs_path, const std::string& out_path) {
    std::ifstream in(latency_path);
    // label,stage,count,mean,p50,p99,p999,max,seconds
    std::map<std::string, std::vector<std::vector<double>>> by_stage;  // stage -> runs -> values
    std::map<std::string, uint64_t> counts;
    std::vector<std::string> labels;
    for (std::string line; std::getline(in, line);) {
        auto f = split(line);
        if (f.size() < 9) continue;
        if (std::find(labels.begin(), labels.end(), f[0]) == labels.end()) labels.push_back(f[0]);
        by_stage[f[1]].push_back({std::stod(f[3]), std::stod(f[4]), std::stod(f[5]), std::stod(f[6]), std::stod(f[7])});
        counts[f[1]] = std::stoull(f[2]);
    }
    std::map<std::string, double> seconds;
    std::ifstream runs(runs_path);
    for (std::string line; std::getline(runs, line);) {
        auto f = split(line);
        if (f.size() >= 22) seconds[f[0]] = std::stod(f[21]);
    }

    auto cell = [&](const std::vector<std::vector<double>>& rows, size_t field) {
        std::vector<double> v;
        for (auto& r : rows) v.push_back(r[field]);
        std::sort(v.begin(), v.end());
        std::string text = fixed(v[v.size() / 2], 0);
        if (v.size() > 1) text += " (" + fixed(v.front(), 0) + "-" + fixed(v.back(), 0) + ")";
        return text;
    };
    const std::vector<std::pair<std::string, std::string>> stages = {
        {"book", "t0-t1: decode + book update (book messages)"},
        {"signals", "t1-t2: top of book + OFI (book messages)"},
        {"strategy", "t2-t3: strategy decision (top-of-book changes of traded stocks)"},
        {"risk", "t3-t4: risk checks (per order intent)"},
        {"send", "t4-t5: hand to the exchange (per order sent)"},
        {"to_order", "**t0-t5: end to end, messages that led to an order**"},
        {"message", "every market message, start to end"},
    };

    std::ofstream out(out_path);
    out << "# Stage 10: latency, stage by stage\n\n"
        << "Written by `trade --latency-report` from `" << latency_path << "`. The whole system replays the day "
        << labels.size() << " times with a timestamp at every stage boundary, recorded into preallocated "
           "histograms. Each cell is the median over the runs, with the range across runs in brackets; "
           "nanoseconds. The time a message waits in memory before t0 isn't included (see below).\n\n"
        << machine_description() << "- Timer overhead: two back-to-back counter reads differ by "
        << fixed(timer_overhead_ns(), 1) << " ns on average; every stage below includes about that much\n\n"
        << "| Stage | Samples per run | Median | 99% | 99.9% | Max | Mean |\n|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& [key, label] : stages) {
        if (!by_stage.count(key)) continue;
        const auto& rows = by_stage[key];
        out << "| " << label << " | " << with_commas(counts[key]) << " | " << cell(rows, 1) << " | " << cell(rows, 2)
            << " | " << cell(rows, 3) << " | " << cell(rows, 4) << " | " << cell(rows, 0) << " |\n";
    }
    if (seconds.count("base")) {
        out << "\nWhole-day replay of the full system: " << fixed(seconds["base"], 1)
            << " s without the timestamps (run `base`)";
        if (!labels.empty() && seconds.count(labels[0])) {
            out << ", " << fixed(seconds[labels[0]], 1) << " s with them (run `" << labels[0] << "`)";
        }
        out << ". The difference is the cost of reading the counter six times per message.\n";
    }
    if (std::string(counter_name()) == "cntvct_el0") {
        out << "\nOn this machine the counter ticks every 41.7 ns, so every percentile is a whole number of ticks "
               "(0, 42, 83, 125, ... ns) and stages shorter than a tick mostly read 0; the means average out the "
               "rounding. These are not the final latency numbers: those need a Linux x86 machine with pinned, "
               "isolated cores and `rdtsc` (one command, see the README).\n";
    }
    out << "\n## What these numbers leave out\n\n"
        << "t0 is taken when a message is already in memory. A firm's wire-to-wire number also includes the "
           "network card, the kernel or kernel bypass stack, reading the packet, and on the way out, encoding the "
           "order and sending it, typically several microseconds more. This system also replays from a file in "
           "large chunks, so it never waits for the next packet.\n";
    std::cerr << "wrote " << out_path << "\n";
    return 0;
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == name) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string latency_csv = arg_value(argc, argv, "--latency-report", "");
    if (!latency_csv.empty()) {
        return latency_report(latency_csv, arg_value(argc, argv, "--runs", "results/raw/trade_runs.csv"),
                              arg_value(argc, argv, "--out", "results/latency.md"));
    }
    const std::string report_runs = arg_value(argc, argv, "--report", "");
    if (!report_runs.empty()) {
        return report(report_runs, arg_value(argc, argv, "--stocks", "results/raw/trade_stocks.csv"),
                      arg_value(argc, argv, "--out", "results/strategy.md"));
    }
    if (argc < 2) {
        std::cerr << "usage: trade <itch file> --sizing <csv> --global <txt> --params <csv> [options]\n";
        return 2;
    }
    Options o;
    o.itch = argv[1];
    o.sizing = arg_value(argc, argv, "--sizing", "");
    o.global = arg_value(argc, argv, "--global", "");
    o.params = arg_value(argc, argv, "--params", "");
    o.label = arg_value(argc, argv, "--label", "run");
    o.log = arg_value(argc, argv, "--log", "");
    o.runs_csv = arg_value(argc, argv, "--runs-csv", o.runs_csv);
    o.stocks_csv = arg_value(argc, argv, "--stocks-csv", o.stocks_csv);
    o.latency_csv = arg_value(argc, argv, "--latency-csv", "");
    o.latency_us = std::stod(arg_value(argc, argv, "--latency-us", "0"));
    o.loss_limit = std::stod(arg_value(argc, argv, "--loss-limit", "500"));
    o.gamma_scale = std::stod(arg_value(argc, argv, "--gamma-scale", "1"));
    o.kill_switch = !has_flag(argc, argv, "--no-kill-switch");
    o.ofi = has_flag(argc, argv, "--ofi");
    o.threads = has_flag(argc, argv, "--threads");
    o.measure = has_flag(argc, argv, "--measure");
    o.pin = std::stoi(arg_value(argc, argv, "--pin", "-1"));

    const auto params = read_params(o.params);
    if (o.threads) {
        return o.measure ? run<ThreadedLog, true>(o, params) : run<ThreadedLog, false>(o, params);
    }
    return o.measure ? run<InlineLog, true>(o, params) : run<InlineLog, false>(o, params);
}
