// Stage 4 check: replay the day through the reference book and every fast
// book at once, and compare them after every message.
//
//   book_diff <itch file> --sizing <csv> --global <txt> [--levels 5] [--out results/book_diff.md]
//
// After each book message the touched stock's top N levels on both sides are
// compared (a message changes only its own stock's book). Every 20 million
// messages and at the end, every level of every stock is compared as well.

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/reference_book.hpp"
#include "book/sorted_levels.hpp"
#include "book/tick_levels.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "itch/source.hpp"

using namespace ts;

namespace {

struct Lane {
    std::string name;
    uint64_t differences = 0;
    std::string first_difference;
};

template <class A, class B>
bool same_levels(const A& a, const B& b, Locate locate, Side side, size_t n, std::vector<LevelInfo>& buf_a,
                 std::vector<LevelInfo>& buf_b) {
    const size_t na = a.levels(locate, side, n, buf_a.data());
    const size_t nb = b.levels(locate, side, n, buf_b.data());
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i) {
        if (!(buf_a[i] == buf_b[i])) return false;
    }
    return true;
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: book_diff <itch file> --sizing <csv> --global <txt> [--levels N] [--out f]\n";
        return 2;
    }
    const std::string path = argv[1];
    const BookSizing sizing = BookSizing::from_profile(arg_value(argc, argv, "--sizing", ""),
                                                       arg_value(argc, argv, "--global", ""));
    const size_t top_n = std::stoul(arg_value(argc, argv, "--levels", "5"));
    const std::string out_path = arg_value(argc, argv, "--out", "results/book_diff.md");

    ReferenceBook ref(sizing.stocks);
    using HashSorted = FastBook<HashIndex, SortedLevels>;
    using FlatSorted = FastBook<FlatIndex, SortedLevels>;
    using HashTick = FastBook<HashIndex, TickLevels>;
    using HashTick256 = FastBook<HashIndex, TickLevels256>;
    auto b1 = std::make_unique<HashSorted>(sizing);
    auto b2 = std::make_unique<FlatSorted>(sizing);
    auto b3 = std::make_unique<HashTick>(sizing);
    auto b4 = std::make_unique<HashTick256>(sizing);
    std::vector<Lane> lanes = {{HashSorted::name(), 0, {}}, {FlatSorted::name(), 0, {}},
                               {HashTick::name(), 0, {}}, {HashTick256::name() + " (256 ticks)", 0, {}}};

    BookFeed<ReferenceBook> ref_feed(ref);
    BookFeed<HashSorted> f1(*b1);
    BookFeed<FlatSorted> f2(*b2);
    BookFeed<HashTick> f3(*b3);
    BookFeed<HashTick256> f4(*b4);

    std::vector<LevelInfo> buf_a(1 << 16), buf_b(1 << 16);
    uint64_t messages = 0, book_messages = 0, full_comparisons = 0;

    auto compare = [&](auto& fast, Lane& lane, Locate locate, size_t n, Timestamp ts) {
        for (Side side : {Side::Buy, Side::Sell}) {
            if (!same_levels(ref, fast, locate, side, n, buf_a, buf_b)) {
                if (lane.differences++ == 0) {
                    lane.first_difference = clock_time(ts) + " locate " + std::to_string(locate) +
                                            (side == Side::Buy ? " bids" : " asks");
                }
            }
        }
    };
    auto compare_all = [&](Timestamp ts) {
        ++full_comparisons;
        for (size_t l = 0; l < sizing.stocks; ++l) {
            const auto locate = static_cast<Locate>(l);
            compare(*b1, lanes[0], locate, buf_a.size(), ts);
            compare(*b2, lanes[1], locate, buf_a.size(), ts);
            compare(*b3, lanes[2], locate, buf_a.size(), ts);
            compare(*b4, lanes[3], locate, buf_a.size(), ts);
        }
        if (b1->order_count() != ref.order_count()) ++lanes[0].differences;
        if (b2->order_count() != ref.order_count()) ++lanes[1].differences;
        if (b3->order_count() != ref.order_count()) ++lanes[2].differences;
        if (b4->order_count() != ref.order_count()) ++lanes[3].differences;
    };

    Stopwatch watch;
    itch::MessageSource source(path);
    itch::RawMessage m;
    Timestamp last_ts = 0;
    while (source.next(m)) {
        ++messages;
        itch::dispatch(m.data, m.len, ref_feed);
        if (!changes_book(m.type())) continue;
        ++book_messages;
        itch::dispatch(m.data, m.len, f1);
        itch::dispatch(m.data, m.len, f2);
        itch::dispatch(m.data, m.len, f3);
        itch::dispatch(m.data, m.len, f4);
        const Locate locate = m.locate();
        last_ts = m.timestamp();
        compare(*b1, lanes[0], locate, top_n, last_ts);
        compare(*b2, lanes[1], locate, top_n, last_ts);
        compare(*b3, lanes[2], locate, top_n, last_ts);
        compare(*b4, lanes[3], locate, top_n, last_ts);
        if (book_messages % 20'000'000 == 0) compare_all(last_ts);
    }
    compare_all(last_ts);
    const double seconds = watch.seconds();

    uint64_t total = 0;
    for (const Lane& lane : lanes) total += lane.differences;

    std::ofstream out(out_path);
    out << "# Stage 4: fast books against the reference book\n\n"
        << "Written by `book_diff`. One replay of the whole day drives the reference book and four fast "
           "books. After **every** book message (A F E C X D U) the touched stock's top " << top_n
        << " levels on both sides (price, shares, order count) are compared; a message only changes its "
           "own stock's book. Every 20 million book messages and at the end of the day, every level of "
           "every stock and the order counts are compared too.\n\n"
        << machine_description() << "\n"
        << "| | Value |\n|---|---:|\n"
        << "| Messages | " << with_commas(messages) << " |\n"
        << "| Book messages, each followed by a comparison | " << with_commas(book_messages) << " |\n"
        << "| Full comparisons (all levels, all stocks) | " << full_comparisons << " |\n"
        << "| Run time | " << fixed(seconds, 1) << " s |\n\n"
        << "| Fast book | Differences | First difference | Book errors | Level-vector growths |\n"
        << "|---|---:|---|---:|---:|\n";
    auto row = [&](const Lane& lane, const auto& book) {
        out << "| " << lane.name << " | " << lane.differences << " | "
            << (lane.first_difference.empty() ? "none" : lane.first_difference) << " | " << book.errors()
            << " | " << book.growths() << " |\n";
    };
    row(lanes[0], *b1);
    row(lanes[1], *b2);
    row(lanes[2], *b3);
    row(lanes[3], *b4);
    out << "\nReference book errors: " << ref.errors() << ". Orders left at the end: reference "
        << ref.order_count() << ", fast " << b1->order_count() << ".\n";

    std::cerr << "differences " << total << ", " << fixed(seconds, 1) << " s, wrote " << out_path << "\n";
    return total == 0 ? 0 : 1;
}
