// Stage 6: estimate the Avellaneda-Stoikov inputs for each traded stock from
// the day's own data, 9:45 to 15:45.
//
//   calibrate <itch file> --sizing <csv> --global <txt> [--symbols AAPL,MSFT,INTC,CSCO]
//             [--params data/derived/<day>.params.csv] [--out results/calibration.md]
//
//   sigma  standard deviation of the 1-second change of the mid, in dollars
//          (per square root of a second, since the step is one second)
//   k, A   from how often trades reach a distance delta from the mid within a
//          second. A limit order delta away would fill in that second; for a
//          Poisson rate lambda(delta), P(fill in 1 s) = 1 - exp(-lambda), so
//          lambda(delta) = -ln(1 - P). Fit ln lambda = ln A - k delta.
//   beta   next second's mid change (dollars) on this second's OFI (shares)
//   gamma  chosen so one order of 100 shares moves the reservation price by
//          one tick at mid-day: gamma = tick / (100 sigma^2 * 11,700 s)

#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/sorted_levels.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "itch/source.hpp"
#include "signals/signals.hpp"

using namespace ts;

namespace {

constexpr Timestamp kFrom = hms(9, 45, 0);
constexpr Timestamp kTo = hms(15, 45, 0);
constexpr size_t kSeconds = (kTo - kFrom) / kNanosPerSecond;
constexpr int kDeltas = 8;  // delta = 0.5, 1.5, ..., 7.5 ticks

struct Stock {
    std::string symbol;
    Locate locate = 0;
    std::vector<int64_t> mid2_at;         // mid2 at the start of each second (-1 unknown)
    std::vector<int64_t> ofi;             // OFI within each second
    std::vector<int64_t> low_buy_fill;    // lowest price a resting buy traded at, per second
    std::vector<int64_t> high_sell_fill;  // highest price a resting sell traded at
    int64_t mid2 = -1;
    size_t filled_to = 0;                 // seconds whose start mid is known
    double spread_sum = 0;
    uint64_t spread_n = 0;
    Price tick = 100;

    Stock() : mid2_at(kSeconds + 1, -1), ofi(kSeconds, 0), low_buy_fill(kSeconds, -1), high_sell_fill(kSeconds, -1) {}

    // grid points up to time t take the mid that held before t
    void advance(Timestamp t) {
        while (filled_to <= kSeconds && kFrom + filled_to * kNanosPerSecond <= t) mid2_at[filled_to++] = mid2;
    }
};

using Book = FastBook<HashIndex, SortedLevels>;

class Calibrator : public itch::NullHandler {
public:
    Calibrator(const BookSizing& sizing, std::vector<std::string> symbols)
        : book_(std::make_unique<Book>(sizing)), signals_(sizing.stocks), index_(sizing.stocks, -1) {
        for (auto& s : symbols) {
            stocks_.emplace_back();
            stocks_.back().symbol = s;
        }
    }

    void on_stock_directory(const itch::StockDirectory& r) {
        for (size_t i = 0; i < stocks_.size(); ++i) {
            if (stocks_[i].symbol == r.stock.view()) {
                stocks_[i].locate = r.h.locate;
                index_[r.h.locate] = static_cast<int>(i);
            }
        }
    }

    void on_add(const itch::AddOrder& m) {
        book_->add(m.h.locate, m.ref, m.side, m.shares, m.price);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_executed(const itch::OrderExecuted& m) { executed(m.h.locate, m.ref, m.shares, 0, m.h.timestamp); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        executed(m.h.locate, m.ref, m.shares, m.price, m.h.timestamp);
    }
    void on_cancel(const itch::OrderCancel& m) {
        book_->cancel(m.ref, m.shares);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_delete(const itch::OrderDelete& m) {
        book_->remove(m.ref);
        changed(m.h.locate, m.h.timestamp);
    }
    void on_replace(const itch::OrderReplace& m) {
        book_->replace(m.old_ref, m.new_ref, m.shares, m.price);
        changed(m.h.locate, m.h.timestamp);
    }

    void finish() {
        for (Stock& s : stocks_) s.advance(kTo);
    }

    std::vector<Stock> stocks_;

private:
    void executed(Locate locate, OrderRef ref, Qty shares, Price price, Timestamp t) {
        const int i = index_[locate];
        if (i >= 0 && t >= kFrom && t < kTo) {
            OrderView v;
            if (book_->find(ref, v)) {
                Stock& s = stocks_[static_cast<size_t>(i)];
                const int64_t p = price ? price : v.price;
                const size_t k = static_cast<size_t>((t - kFrom) / kNanosPerSecond);
                if (v.side == Side::Buy) {
                    if (s.low_buy_fill[k] < 0 || p < s.low_buy_fill[k]) s.low_buy_fill[k] = p;
                } else {
                    if (p > s.high_sell_fill[k]) s.high_sell_fill[k] = p;
                }
            }
        }
        book_->execute(ref, shares);
        changed(locate, t);
    }

    void changed(Locate locate, Timestamp t) {
        if (!signals_.on_top(locate, book_->top(locate), t)) return;
        const int i = index_[locate];
        if (i < 0) return;
        Stock& s = stocks_[static_cast<size_t>(i)];
        s.advance(t);
        const StockSignals& sig = signals_.stock(locate);
        if (!sig.top.two_sided() || sig.top.bid >= sig.top.ask) return;
        s.mid2 = static_cast<int64_t>(sig.mid2());
        s.tick = tick_size(sig.top.bid);
        if (t >= kFrom && t < kTo) {
            s.ofi[static_cast<size_t>((t - kFrom) / kNanosPerSecond)] += sig.last_e;
            s.spread_sum += static_cast<double>(sig.top.ask - sig.top.bid) / s.tick;
            ++s.spread_n;
        }
    }

    std::unique_ptr<Book> book_;
    Signals signals_;
    std::vector<int> index_;
};

struct Fit {
    double sigma = 0, k = 0, A = 0, r2 = 0, beta = 0, gamma = 0, spread_ticks = 0, price = 0;
    double prob[kDeltas] = {}, lambda[kDeltas] = {};
};

// least squares y = a + b x
void ols(const std::vector<double>& x, const std::vector<double>& y, double& a, double& b, double& r2) {
    const double n = static_cast<double>(x.size());
    double mx = 0, my = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= n;
    my /= n;
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    b = sxx > 0 ? sxy / sxx : 0;
    a = my - b * mx;
    r2 = sxx > 0 && syy > 0 ? sxy * sxy / (sxx * syy) : 0;
}

Fit fit(const Stock& s) {
    Fit f;
    const double tick = static_cast<double>(s.tick) / kPriceScale;
    std::vector<double> d_mid;
    for (size_t k = 0; k < kSeconds; ++k) {
        if (s.mid2_at[k] > 0 && s.mid2_at[k + 1] > 0) {
            d_mid.push_back(static_cast<double>(s.mid2_at[k + 1] - s.mid2_at[k]) / (2.0 * kPriceScale));
        }
    }
    double mean = 0;
    for (double d : d_mid) mean += d;
    mean /= static_cast<double>(d_mid.size());
    double var = 0;
    for (double d : d_mid) var += (d - mean) * (d - mean);
    f.sigma = std::sqrt(var / static_cast<double>(d_mid.size() - 1));
    f.price = static_cast<double>(s.mid2_at[kSeconds / 2]) / (2.0 * kPriceScale);
    f.spread_ticks = s.spread_n ? s.spread_sum / static_cast<double>(s.spread_n) : 0;

    // fill probability at each distance, both sides pooled
    std::vector<double> xs, ys;
    for (int j = 0; j < kDeltas; ++j) {
        const double delta = (j + 0.5) * tick;
        uint64_t hits = 0, total = 0;
        for (size_t k = 0; k < kSeconds; ++k) {
            if (s.mid2_at[k] <= 0) continue;
            const double mid = static_cast<double>(s.mid2_at[k]) / (2.0 * kPriceScale);
            total += 2;
            if (s.low_buy_fill[k] > 0 && mid - static_cast<double>(s.low_buy_fill[k]) / kPriceScale >= delta - 1e-9) ++hits;
            if (s.high_sell_fill[k] > 0 && static_cast<double>(s.high_sell_fill[k]) / kPriceScale - mid >= delta - 1e-9) ++hits;
        }
        f.prob[j] = total ? static_cast<double>(hits) / static_cast<double>(total) : 0;
        f.lambda[j] = f.prob[j] < 1 ? -std::log(1 - f.prob[j]) : 0;
        if (hits >= 20 && f.prob[j] < 0.999) {
            xs.push_back(delta);
            ys.push_back(std::log(f.lambda[j]));
        }
    }
    double a = 0, b = 0;
    ols(xs, ys, a, b, f.r2);
    f.k = -b;
    f.A = std::exp(a);

    // OFI over a second against the next second's mid change
    std::vector<double> x, y;
    for (size_t k = 0; k + 2 <= kSeconds; ++k) {
        if (s.mid2_at[k + 1] <= 0 || s.mid2_at[k + 2] <= 0) continue;
        x.push_back(static_cast<double>(s.ofi[k]));
        y.push_back(static_cast<double>(s.mid2_at[k + 2] - s.mid2_at[k + 1]) / (2.0 * kPriceScale));
    }
    double a2 = 0, r2 = 0;
    ols(x, y, a2, f.beta, r2);

    f.gamma = tick / (100.0 * f.sigma * f.sigma * 11700.0);
    return f;
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

std::string sci(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: calibrate <itch file> --sizing <csv> --global <txt> [--symbols A,B] [--params f] [--out f]\n";
        return 2;
    }
    const BookSizing sizing = BookSizing::from_profile(arg_value(argc, argv, "--sizing", ""),
                                                       arg_value(argc, argv, "--global", ""));
    std::vector<std::string> symbols;
    std::stringstream list(arg_value(argc, argv, "--symbols", "AAPL,MSFT,INTC,CSCO"));
    for (std::string s; std::getline(list, s, ',');) symbols.push_back(s);

    Calibrator cal(sizing, symbols);
    itch::replay_file(argv[1], cal);
    cal.finish();

    std::ofstream params(arg_value(argc, argv, "--params", "data/derived/params.csv"));
    params << "symbol,locate,tick,sigma,k,A,ofi_beta,gamma\n";
    std::ofstream out(arg_value(argc, argv, "--out", "results/calibration.md"));
    out << "# Stage 6: strategy inputs, estimated from the day\n\n"
        << "Written by `calibrate`, from 9:45 to 15:45 (" << kSeconds << " one-second steps).\n\n"
        << machine_description() << "\n"
        << "| Stock | Mid at 12:45 | Mean spread (ticks) | σ ($/√s) | σ over 6.5 h | k (per $) | A (per s) | Fit R² | OFI β ($ per 1,000 shares) | γ (per $) |\n"
        << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    std::vector<Fit> fits;
    for (const Stock& s : cal.stocks_) {
        const Fit f = fit(s);
        fits.push_back(f);
        params << s.symbol << ',' << s.locate << ',' << s.tick << ',' << sci(f.sigma) << ',' << sci(f.k) << ','
               << sci(f.A) << ',' << sci(f.beta) << ',' << sci(f.gamma) << '\n';
        const double day_move = f.sigma * std::sqrt(23400.0) / f.price * 100;
        out << "| " << s.symbol << " | " << fixed(f.price, 2) << " | " << fixed(f.spread_ticks, 2) << " | "
            << sci(f.sigma) << " | " << fixed(day_move, 2) << "% | " << fixed(f.k, 1) << " | " << sci(f.A) << " | "
            << fixed(f.r2, 3) << " | " << sci(1000 * f.beta) << " | " << sci(f.gamma) << " |\n";
    }

    out << "\n## How often trades reach a distance from the mid within one second\n\n"
        << "Pooled over the bid and ask sides. The fit of k and A uses the distances with at least 20 hits.\n\n"
        << "| Stock |";
    for (int j = 0; j < kDeltas; ++j) out << " " << fixed(j + 0.5, 1) << " ticks |";
    out << "\n|---|";
    for (int j = 0; j < kDeltas; ++j) out << "---:|";
    out << "\n";
    for (size_t i = 0; i < fits.size(); ++i) {
        out << "| " << cal.stocks_[i].symbol << " |";
        for (int j = 0; j < kDeltas; ++j) out << " " << fixed(100 * fits[i].prob[j], 2) << "% |";
        out << "\n";
    }

    out << "\n## What the model then quotes at mid-day\n\n"
        << "Half the A-S spread and the shift of the reservation price for 100 shares held, both at 12:45 "
           "(3.25 hours left), in ticks.\n\n"
        << "| Stock | 2/k in ticks | Half-spread | Shift per 100 shares |\n|---|---:|---:|---:|\n";
    for (size_t i = 0; i < fits.size(); ++i) {
        const Fit& f = fits[i];
        const double tick = static_cast<double>(cal.stocks_[i].tick) / kPriceScale;
        const double tau = 11700.0;
        const double spread = f.gamma * f.sigma * f.sigma * tau + (2 / f.gamma) * std::log1p(f.gamma / f.k);
        const double shift = 100 * f.gamma * f.sigma * f.sigma * tau;
        out << "| " << cal.stocks_[i].symbol << " | " << fixed(2 / f.k / tick, 2) << " | " << fixed(spread / 2 / tick, 2)
            << " | " << fixed(shift / tick, 2) << " |\n";
    }
    out << "\nThese use the same day the strategy then trades on, which is look-ahead: a live system would "
           "estimate them on earlier days. With one day of data, that is a stated limitation.\n";
    std::cerr << "wrote calibration for " << cal.stocks_.size() << " stocks\n";
    return 0;
}
