// Stage 4 measurement: replay the whole day through one book, timing every
// book message, and append a row to a CSV file.
//
//   book_bench <itch file> --book <name> --sizing <csv> --global <txt> --csv results/raw/book_bench.csv
//   book_bench --report results/raw/book_bench.csv --out results/book.md
//
// Books: reference, legacy, hash-linear, flat-linear, hash-sorted, flat-sorted,
// local-hash-sorted, hash-tick, hash-tick256.
// Run each in its own process, so the peak memory belongs to that book alone.

#include <sys/resource.h>

#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/reference_book.hpp"
#include "book/sorted_levels.hpp"
#include "book/tick_levels.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/histogram.hpp"
#include "common/machine.hpp"
#include "itch/source.hpp"
#include "legacy/legacy_books.hpp"

using namespace ts;

namespace {

// Times only the book call: the message is already decoded when t0 is taken.
template <class Book>
struct TimedFeed : itch::NullHandler {
    Book& book;
    Histogram& hist;
    TimedFeed(Book& b, Histogram& h) : book(b), hist(h) {}

    void on_add(const itch::AddOrder& m) {
        const uint64_t t0 = read_counter();
        book.add(m.h.locate, m.ref, m.side, m.shares, m.price);
        hist.record(read_counter() - t0);
    }
    void on_executed(const itch::OrderExecuted& m) {
        const uint64_t t0 = read_counter();
        book.execute(m.ref, m.shares);
        hist.record(read_counter() - t0);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        const uint64_t t0 = read_counter();
        book.execute(m.ref, m.shares);
        hist.record(read_counter() - t0);
    }
    void on_cancel(const itch::OrderCancel& m) {
        const uint64_t t0 = read_counter();
        book.cancel(m.ref, m.shares);
        hist.record(read_counter() - t0);
    }
    void on_delete(const itch::OrderDelete& m) {
        const uint64_t t0 = read_counter();
        book.remove(m.ref);
        hist.record(read_counter() - t0);
    }
    void on_replace(const itch::OrderReplace& m) {
        const uint64_t t0 = read_counter();
        book.replace(m.old_ref, m.new_ref, m.shares, m.price);
        hist.record(read_counter() - t0);
    }
};

// The earlier book needs the stock on every call.
struct TimedLegacyFeed : itch::NullHandler {
    LegacyBooks& book;
    Histogram& hist;
    TimedLegacyFeed(LegacyBooks& b, Histogram& h) : book(b), hist(h) {}

    void on_add(const itch::AddOrder& m) {
        const uint64_t t0 = read_counter();
        book.add(m.h.locate, m.ref, m.side, m.shares, m.price);
        hist.record(read_counter() - t0);
    }
    void on_executed(const itch::OrderExecuted& m) { timed_reduce(m.h.locate, m.ref, m.shares); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { timed_reduce(m.h.locate, m.ref, m.shares); }
    void on_cancel(const itch::OrderCancel& m) { timed_reduce(m.h.locate, m.ref, m.shares); }
    void on_delete(const itch::OrderDelete& m) {
        const uint64_t t0 = read_counter();
        book.remove(m.h.locate, m.ref);
        hist.record(read_counter() - t0);
    }
    void on_replace(const itch::OrderReplace& m) {
        const uint64_t t0 = read_counter();
        book.replace(m.h.locate, m.old_ref, m.new_ref, m.shares, m.price);
        hist.record(read_counter() - t0);
    }
    void timed_reduce(Locate locate, OrderRef ref, Qty shares) {
        const uint64_t t0 = read_counter();
        book.reduce(locate, ref, shares);
        hist.record(read_counter() - t0);
    }
};

struct Row {
    std::string book;
    uint64_t messages = 0, book_messages = 0;
    double total_s = 0, read_s = 0, mean_ns = 0, p50 = 0, p99 = 0, p999 = 0, max_ns = 0, rss_mb = 0;
};

double peak_rss_mb() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(u.ru_maxrss) / 1024.0;
#endif
}

template <class Feed>
Row run(const std::string& path, Feed& feed, Histogram& hist, const std::string& name) {
    Stopwatch watch;
    itch::MessageSource source(path);
    itch::RawMessage m;
    Row r;
    r.book = name;
    while (source.next(m)) {
        itch::dispatch(m.data, m.len, feed);
        ++r.messages;
    }
    r.total_s = watch.seconds();
    r.read_s = source.read_seconds();
    r.book_messages = hist.count();
    r.mean_ns = ticks_to_ns(hist.mean());
    r.p50 = ticks_to_ns(static_cast<double>(hist.percentile(0.50)));
    r.p99 = ticks_to_ns(static_cast<double>(hist.percentile(0.99)));
    r.p999 = ticks_to_ns(static_cast<double>(hist.percentile(0.999)));
    r.max_ns = ticks_to_ns(static_cast<double>(hist.max()));
    r.rss_mb = peak_rss_mb();
    return r;
}

template <class Book>
Row run_book(const std::string& path, Book& book, const std::string& name) {
    Histogram hist;
    TimedFeed<Book> feed(book, hist);
    return run(path, feed, hist, name);
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

// Back-to-back counter reads: the floor under every sample.
double timer_overhead_ns() {
    Histogram h;
    for (int i = 0; i < 1'000'000; ++i) {
        const uint64_t t0 = read_counter();
        h.record(read_counter() - t0);
    }
    return ticks_to_ns(h.mean());
}

int report(const std::string& csv_path, const std::string& out_path) {
    std::ifstream in(csv_path);
    std::string line;
    std::getline(in, line);
    std::map<std::string, std::vector<Row>> runs;  // every run of each book
    while (std::getline(in, line)) {
        std::stringstream s(line);
        Row r;
        std::string f;
        std::getline(s, r.book, ',');
        std::getline(s, f, ','); r.messages = std::stoull(f);
        std::getline(s, f, ','); r.book_messages = std::stoull(f);
        double* fields[] = {&r.total_s, &r.read_s, &r.mean_ns, &r.p50, &r.p99, &r.p999, &r.max_ns, &r.rss_mb};
        for (double* d : fields) {
            std::getline(s, f, ',');
            *d = std::stod(f);
        }
        runs[r.book].push_back(r);
    }
    // each book's row is the field-by-field median of its runs
    std::map<std::string, Row> rows;
    std::map<std::string, std::pair<double, double>> mean_range;
    for (auto& [book, list] : runs) {
        Row med = list.front();
        double Row::*fields[] = {&Row::total_s, &Row::read_s, &Row::mean_ns, &Row::p50, &Row::p99,
                                 &Row::p999, &Row::max_ns, &Row::rss_mb};
        for (double Row::*f : fields) {
            std::vector<double> v;
            for (const Row& r : list) v.push_back(r.*f);
            std::sort(v.begin(), v.end());
            med.*f = v[v.size() / 2];
            if (f == &Row::mean_ns) mean_range[book] = {v.front(), v.back()};
        }
        rows[book] = med;
    }
    const std::vector<std::pair<std::string, std::string>> order = {
        {"legacy", "Earlier book (`shared_ptr` per order, `std::map<double, ...>`)"},
        {"reference", "Reference book (`unordered_map` + `map` + `list`)"},
        {"hash-linear", "Fast: hash table + sorted vector, linear search (first version)"},
        {"flat-linear", "Fast: flat array + sorted vector, linear search"},
        {"hash-sorted", "Fast: hash table + sorted vector"},
        {"flat-sorted", "Fast: flat array + sorted vector"},
        {"local-hash-sorted", "Fast: hash table (ref mod size) + sorted vector"},
        {"hash-tick", "Fast: hash table + tick array (1024 ticks)"},
        {"hash-tick256", "Fast: hash table + tick array (256 ticks)"},
    };

    std::ofstream out(out_path);
    out << "# Stage 4: three order books on the same day\n\n"
        << "Written by `book_bench --report` from `" << csv_path << "`. Each book replays the whole day in "
           "its own process. Each sample is one book message (A F E C X D U), timed from after decoding to "
           "after the book call; parsing and file reads are outside the samples. A book run several times shows "
           "the median of its runs, and the range of the mean across them in brackets.\n\n"
        << machine_description()
        << "- Timer overhead (back-to-back reads, included in every sample): " << fixed(timer_overhead_ns(), 1)
        << " ns on average\n\n"
        << "| Book | Runs | Mean ns | Median ns | 99% ns | 99.9% ns | Max µs | Replay s | of which file reads s | Peak memory MB |\n"
        << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& [key, label] : order) {
        auto it = rows.find(key);
        if (it == rows.end()) continue;
        const Row& r = it->second;
        std::string mean = fixed(r.mean_ns, 1);
        if (runs[key].size() > 1) {
            mean += " (" + fixed(mean_range[key].first, 1) + "-" + fixed(mean_range[key].second, 1) + ")";
        }
        out << "| " << label << " | " << runs[key].size() << " | " << mean << " | " << fixed(r.p50, 0) << " | " << fixed(r.p99, 0)
            << " | " << fixed(r.p999, 0) << " | " << fixed(r.max_ns / 1000.0, 1) << " | " << fixed(r.total_s, 1)
            << " | " << fixed(r.read_s, 1) << " | " << fixed(r.rss_mb, 0) << " |\n";
    }
    if (!rows.empty()) {
        const Row& any = rows.begin()->second;
        out << "\n" << with_commas(any.book_messages) << " book messages out of " << with_commas(any.messages)
            << " in each run. Percentiles come from a histogram with 64 buckets per power of two.\n";
    }
    if (std::string(counter_name()) == "cntvct_el0") {
        out << "\nOn Apple Silicon the counter ticks every 41.7 ns, so the percentiles here are whole "
               "numbers of ticks (0, 42, 83, ... ns) and say little below 100 ns. The mean is still "
               "meaningful, because the rounding errors average out over 263 million samples. Take the "
               "final percentiles on a Linux x86 machine, where `rdtsc` resolves well under 1 ns.\n";
    }
    auto ratio = [&](const std::string& a, const std::string& b) -> std::string {
        if (!rows.count(a) || !rows.count(b) || rows[b].mean_ns == 0) return "n/a";
        return fixed(rows[a].mean_ns / rows[b].mean_ns, 2) + "x";
    };
    out << "\n## Mean time per message, as ratios\n\n"
        << "| Comparison | Ratio |\n|---|---:|\n"
        << "| Earlier book / reference book | " << ratio("legacy", "reference") << " |\n"
        << "| Reference book / fast book (hash + sorted vector) | " << ratio("reference", "hash-sorted") << " |\n"
        << "| Earlier book / fast book (hash + sorted vector) | " << ratio("legacy", "hash-sorted") << " |\n"
        << "| Linear search / linear then binary search (hash table) | " << ratio("hash-linear", "hash-sorted") << " |\n"
        << "| Hash table / flat array (both with sorted vectors) | " << ratio("hash-sorted", "flat-sorted") << " |\n"
        << "| Hash table (ref mod size) / flat array | " << ratio("local-hash-sorted", "flat-sorted") << " |\n"
        << "| Sorted vector / tick array of 1024 (both with hash table) | " << ratio("hash-sorted", "hash-tick") << " |\n"
        << "| Tick array of 256 / tick array of 1024 | " << ratio("hash-tick256", "hash-tick") << " |\n";
    auto mean = [&](const std::string& key) { return rows.count(key) ? fixed(rows[key].mean_ns, 1) + " ns" : std::string("n/a"); };
    out << "\n## Change log\n\n"
        << "The book is the slowest stage by far (`results/latency.md`), so the changes went there, one at a "
           "time, each measured over the whole day on this machine. Mean ns per book message:\n\n"
        << "| Step | Change | Mean |\n|---|---|---:|\n"
        << "| 0 | First version: hash table, sorted vector searched linearly from the best price | " << mean("hash-linear") << " |\n"
        << "| 1 | Sorted vector: linear for 16 levels, then binary search (stub quotes sit thousands of levels deep) | "
        << mean("hash-sorted") << " |\n"
        << "| 2 | Order lookup: flat array indexed by reference number instead of the hash table | " << mean("flat-sorted") << " |\n"
        << "| tried | Hash table with slot = ref mod size, to keep neighbouring refs together | " << mean("local-hash-sorted") << " |\n"
        << "| tried | Tick array of 1024 ticks around the best instead of the sorted vector | " << mean("hash-tick") << " |\n"
        << "| tried | Tick array of 256 ticks | " << mean("hash-tick256") << " |\n"
        << "| check | Flat array with the linear search, to separate the two changes | " << mean("flat-linear") << " |\n";
    std::cerr << "wrote " << out_path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string report_csv = arg_value(argc, argv, "--report", "");
    if (!report_csv.empty()) return report(report_csv, arg_value(argc, argv, "--out", "results/book.md"));
    if (argc < 2) {
        std::cerr << "usage: book_bench <itch file> --book <name> --sizing <csv> --global <txt> --csv <out>\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string book = arg_value(argc, argv, "--book", "hash-sorted");
    const std::string csv_path = arg_value(argc, argv, "--csv", "results/raw/book_bench.csv");
    const BookSizing sizing = BookSizing::from_profile(arg_value(argc, argv, "--sizing", ""),
                                                       arg_value(argc, argv, "--global", ""));
    Row r;
    if (book == "reference") {
        ReferenceBook b(sizing.stocks);
        r = run_book(path, b, book);
    } else if (book == "legacy") {
        LegacyBooks b(sizing.stocks);
        Histogram hist;
        TimedLegacyFeed feed(b, hist);
        r = run(path, feed, hist, book);
    } else if (book == "hash-sorted") {
        auto b = std::make_unique<FastBook<HashIndex, SortedLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "flat-sorted") {
        auto b = std::make_unique<FastBook<FlatIndex, SortedLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "hash-linear") {
        auto b = std::make_unique<FastBook<HashIndex, LinearLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "flat-linear") {
        auto b = std::make_unique<FastBook<FlatIndex, LinearLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "local-hash-sorted") {
        auto b = std::make_unique<FastBook<LocalHashIndex, SortedLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "hash-tick") {
        auto b = std::make_unique<FastBook<HashIndex, TickLevels>>(sizing);
        r = run_book(path, *b, book);
    } else if (book == "hash-tick256") {
        auto b = std::make_unique<FastBook<HashIndex, TickLevels256>>(sizing);
        r = run_book(path, *b, book);
    } else {
        std::cerr << "unknown book " << book << "\n";
        return 2;
    }

    const bool new_file = !std::ifstream(csv_path).good();
    std::ofstream csv(csv_path, std::ios::app);
    if (new_file) csv << "book,messages,book_messages,total_s,read_s,mean_ns,p50_ns,p99_ns,p999_ns,max_ns,peak_rss_mb\n";
    csv << r.book << ',' << r.messages << ',' << r.book_messages << ',' << r.total_s << ',' << r.read_s << ','
        << r.mean_ns << ',' << r.p50 << ',' << r.p99 << ',' << r.p999 << ',' << r.max_ns << ',' << r.rss_mb << '\n';
    std::cerr << book << ": mean " << fixed(r.mean_ns, 1) << " ns, p99 " << fixed(r.p99, 0) << " ns, replay "
              << fixed(r.total_s, 1) << " s, peak " << fixed(r.rss_mb, 0) << " MB\n";
    return 0;
}
