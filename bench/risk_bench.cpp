// Stage 7 measurement: what the pre-trade checks cost on the hot path.
//
//   risk_bench [--out results/risk.md]
//
// A gate with four stocks, positions, and six open orders. Replacing one of
// them runs every check (size, collar, open orders, position over all open
// orders on the side, message rate, loss budget) and hands the order to the
// exchange queue. The counter ticks only every 41.7 ns on Apple Silicon, so
// each sample times a batch of 1,000 calls.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "risk/risk_gate.hpp"

using namespace ts;

namespace {

constexpr int kBatch = 1000;
constexpr int kBatches = 2000;

struct Setup {
    Signals signals{8};
    Portfolio portfolio{8};
    OrderGateway gateway{0};
    RiskLimits limits;
    std::unique_ptr<RiskGate> gate;
    std::vector<uint64_t> open_ids;  // one per (stock, side)
    uint64_t next_id = 1;

    Setup() {
        limits.max_messages_per_second = 2'000'000;  // with one call per microsecond, never binding
        limits.max_open_orders = 20;
        limits.max_position = 100'000;
        for (Locate l = 1; l <= 4; ++l) signals.on_top(l, TopOfBook{1000000, 500, 1000100, 500}, 1);
        Response fill;
        fill.type = ResponseType::Fill;
        fill.locate = 1;
        fill.side = Side::Buy;
        fill.price = 1000000;
        fill.qty = 300;
        portfolio.on_fill(fill);
        gate = std::make_unique<RiskGate>(limits, gateway, portfolio, signals, std::vector<Locate>{1, 2, 3, 4});
        gate->mark(1);
        for (Locate l = 1; l <= 3; ++l) {
            for (Side s : {Side::Buy, Side::Sell}) {
                OrderIntent o = order(l, s, s == Side::Buy ? 1000000 : 1000100);
                gate->submit(o, 2);
                open_ids.push_back(o.id);
            }
        }
        drain();
    }

    OrderIntent order(Locate l, Side s, Price p) {
        OrderIntent o;
        o.type = IntentType::New;
        o.locate = l;
        o.side = s;
        o.price = p;
        o.qty = 100;
        o.id = next_id++;
        return o;
    }

    void drain() {
        while (!gateway.empty()) gateway.pop();
    }
};

struct Stats {
    double median, p99, mean;
};

template <class F>
Stats time_batches(F call) {
    std::vector<double> per_call;
    double sum = 0;
    for (int b = 0; b < kBatches; ++b) {
        const uint64_t t0 = read_counter();
        for (int i = 0; i < kBatch; ++i) call(b * kBatch + i);
        const double ns = ticks_to_ns(static_cast<double>(read_counter() - t0)) / kBatch;
        per_call.push_back(ns);
        sum += ns;
    }
    std::sort(per_call.begin(), per_call.end());
    return Stats{per_call[per_call.size() / 2], per_call[per_call.size() * 99 / 100], sum / kBatches};
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "results/risk.md";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--out") out_path = argv[i + 1];
    }

    uint64_t accepted = 0, rejected = 0;
    std::vector<Stats> fulls, earlies, marks;
    for (int round = 0; round < 5; ++round) {
        Setup s;
        // replace each open order in turn, alternating between two prices
        fulls.push_back(time_batches([&](int i) {
            const size_t k = static_cast<size_t>(i) % s.open_ids.size();
            const Locate l = static_cast<Locate>(1 + k / 2);
            const Side side = k % 2 == 0 ? Side::Buy : Side::Sell;
            const Price step = static_cast<Price>(100 * (i % 2));
            OrderIntent o = s.order(l, side, side == Side::Buy ? 1000000 - step : 1000100 + step);
            o.type = IntentType::Replace;
            o.old_id = s.open_ids[k];
            if (s.gate->submit(o, 3 + static_cast<Timestamp>(i) * 1000) == Verdict::Accepted) {
                s.open_ids[k] = o.id;
                ++accepted;
            }
            if ((i & 1023) == 1023) s.drain();
        }));
        s.drain();
        earlies.push_back(time_batches([&](int) {
            OrderIntent o = s.order(1, Side::Buy, 1000000);
            o.qty = 1'000'000;  // too large: refused by the first check
            rejected += s.gate->submit(o, 3) == Verdict::OrderSize;
        }));
        marks.push_back(time_batches([&](int) { s.gate->mark(4); }));
    }
    // median over the five rounds of each statistic, with the range of the median
    auto summarize = [](std::vector<Stats> v, std::string& range) {
        auto pick = [&](double Stats::*f) {
            std::vector<double> x;
            for (const Stats& st : v) x.push_back(st.*f);
            std::sort(x.begin(), x.end());
            if (f == &Stats::median) range = fixed(x.front(), 1) + "-" + fixed(x.back(), 1);
            return x[x.size() / 2];
        };
        return Stats{pick(&Stats::median), pick(&Stats::p99), pick(&Stats::mean)};
    };
    std::string full_range, early_range, mark_range;
    const Stats full = summarize(fulls, full_range);
    const Stats early = summarize(earlies, early_range);
    const Stats mark = summarize(marks, mark_range);

    std::ofstream out(out_path);
    out << "# Stage 7: the cost of the risk checks\n\n"
        << "Written by `risk_bench`. A risk gate with four stocks, a position of 300 shares and six open orders. "
        << with_commas(static_cast<uint64_t>(kBatch) * kBatches) << " calls per row, timed in batches of "
        << kBatch << ", repeated in 5 rounds on a fresh gate. Each cell is the median over the rounds of the "
           "batches' median, 99th percentile and mean time per call; the range of the median across the rounds "
           "is in brackets.\n\n"
        << machine_description() << "\n"
        << "| Call | Median ns | 99% of batches ns | Mean ns |\n|---|---:|---:|---:|\n"
        << "| `submit`, a replace that passes every check and goes to the exchange queue | " << fixed(full.median, 1) << " (" << full_range << ")"
        << " | " << fixed(full.p99, 1) << " | " << fixed(full.mean, 1) << " |\n"
        << "| `submit`, an order refused by the first check (size) | " << fixed(early.median, 1) << " (" << early_range << ") | "
        << fixed(early.p99, 1) << " | " << fixed(early.mean, 1) << " |\n"
        << "| `mark`: equity over four stocks, peak, budget, kill-switch test | " << fixed(mark.median, 1) << " (" << mark_range << ") | "
        << fixed(mark.p99, 1) << " | " << fixed(mark.mean, 1) << " |\n\n"
        << "Accepted " << with_commas(accepted) << " of the replaces and refused " << with_commas(rejected)
        << " of the oversized orders (all of them, as expected).\n\n"
        << "The same checks are also timed inside the full system (stage 10, `results/latency.md`, the risk "
           "stage), where the gate's state is colder in the cache.\n";
    std::cerr << "full " << fixed(full.median, 1) << " ns, early " << fixed(early.median, 1) << " ns, mark "
              << fixed(mark.median, 1) << " ns; wrote " << out_path << "\n";
    return 0;
}
