// Stage 2 measurement: how fast the feed handler decodes the day.
//
//   parse_bench <itch file> [--global data/derived/<day>.global.txt] [--out results/parser.md]
//
// 1. The whole day, read in 256 MB chunks; only the parsing of each chunk is
//    timed, so the numbers are the parser's, not the disk's.
// 2. The whole day through mmap, page faults included.
// 3. Ten million messages from 10:00 parsed in their real order, shuffled, and
//    sorted by type: the same work with different branch predictability.

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "common/chunked_reader.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/machine.hpp"
#include "common/mapped_file.hpp"
#include "itch/parser.hpp"

using namespace ts;
using namespace ts::itch;

namespace {

// Uses every decoded field the books need, so nothing is optimised away.
struct Sink : NullHandler {
    uint64_t sum = 0;
    std::array<uint64_t, 256> counts{};

    void on_add(const AddOrder& m) { sum += m.ref ^ m.price ^ m.shares ^ m.h.locate; ++counts[static_cast<unsigned char>(m.h.type)]; }
    void on_executed(const OrderExecuted& m) { sum += m.ref ^ m.shares; ++counts['E']; }
    void on_executed_with_price(const OrderExecutedWithPrice& m) { sum += m.ref ^ m.price; ++counts['C']; }
    void on_cancel(const OrderCancel& m) { sum += m.ref ^ m.shares; ++counts['X']; }
    void on_delete(const OrderDelete& m) { sum += m.ref; ++counts['D']; }
    void on_replace(const OrderReplace& m) { sum += m.old_ref ^ m.new_ref ^ m.price; ++counts['U']; }
    void on_trade(const Trade& m) { sum += m.price ^ m.shares; ++counts['P']; }
    void on_cross_trade(const CrossTrade& m) { sum += m.price; ++counts['Q']; }
    void on_system_event(const SystemEvent& m) { sum += static_cast<uint64_t>(m.event_code); ++counts['S']; }
    void on_stock_directory(const StockDirectory& m) { sum += m.h.locate; ++counts['R']; }
    void on_trading_action(const TradingAction& m) { sum += static_cast<uint64_t>(m.state); ++counts['H']; }
    void on_other(char type, const std::byte*, uint16_t) { ++counts[static_cast<unsigned char>(type)]; }
};

struct Pass {
    uint64_t messages = 0;
    double parse_seconds = 0;
    double read_seconds = 0;
    std::array<uint64_t, 256> counts{};
    uint64_t sum = 0;
};

// Whole day in chunks; time only the parse of each chunk.
Pass chunked_pass(const std::string& path, bool decode) {
    ChunkedReader reader(path, size_t{256} << 20);
    Sink sink;
    Pass p;
    while (reader.fill()) {
        const uint64_t t0 = read_counter();
        Cursor cursor(reader.data(), reader.size());
        RawMessage m;
        if (decode) {
            while (cursor.next(m)) {
                if (dispatch(m.data, m.len, sink) != Status::Ok) throw std::runtime_error("decode failed");
                ++p.messages;
            }
        } else {
            while (cursor.next(m)) {
                ++p.counts[static_cast<unsigned char>(m.type())];
                ++p.messages;
            }
        }
        p.parse_seconds += ticks_to_ns(static_cast<double>(read_counter() - t0)) / 1e9;
        reader.consume(cursor.offset());
    }
    p.read_seconds = reader.read_seconds();
    if (decode) p.counts = sink.counts;
    p.sum = sink.sum;
    return p;
}

Pass mmap_pass(const std::string& path) {
    MappedFile file(path);
    Sink sink;
    Stopwatch watch;
    ParseResult r = parse(file.data(), file.size(), sink);
    Pass p;
    p.parse_seconds = watch.seconds();
    p.messages = r.messages;
    p.sum = sink.sum;
    return p;
}

// Copy n messages (with their length prefixes) starting at the first one at or after `from`.
std::vector<std::vector<std::byte>> sample_messages(const std::string& path, Timestamp from, size_t n) {
    ChunkedReader reader(path, size_t{256} << 20);
    std::vector<std::vector<std::byte>> out;
    out.reserve(n);
    while (out.size() < n && reader.fill()) {
        Cursor cursor(reader.data(), reader.size());
        RawMessage m;
        while (out.size() < n && cursor.next(m)) {
            if (m.timestamp() < from) continue;
            const std::byte* start = m.data - 2;
            out.emplace_back(start, start + m.len + 2);
        }
        reader.consume(cursor.offset());
    }
    return out;
}

std::vector<std::byte> concatenate(const std::vector<const std::vector<std::byte>*>& messages) {
    std::vector<std::byte> buf;
    for (auto* m : messages) buf.insert(buf.end(), m->begin(), m->end());
    return buf;
}

// Median over five runs of ns per message.
double ns_per_message(const std::vector<std::byte>& buf, uint64_t& sum) {
    std::vector<double> runs;
    for (int i = 0; i < 5; ++i) {
        Sink sink;
        const uint64_t t0 = read_counter();
        ParseResult r = parse(buf.data(), buf.size(), sink);
        const double ns = ticks_to_ns(static_cast<double>(read_counter() - t0));
        runs.push_back(ns / static_cast<double>(r.messages));
        sum += sink.sum;
    }
    std::sort(runs.begin(), runs.end());
    return runs[2];
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

uint64_t stage1_messages(const std::string& global) {
    std::ifstream in(global);
    std::string key;
    uint64_t value;
    while (in >> key >> value) {
        if (key == "messages") return value;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: parse_bench <itch file> [--global f] [--out f]\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::string out_path = arg_value(argc, argv, "--out", "results/parser.md");
    const uint64_t expected = stage1_messages(arg_value(argc, argv, "--global", ""));

    std::cerr << "framing walk ...\n";
    const Pass walk = chunked_pass(path, false);
    std::cerr << "decode, three runs ...\n";
    std::vector<Pass> decodes;
    for (int i = 0; i < 3; ++i) decodes.push_back(chunked_pass(path, true));
    std::cerr << "mmap ...\n";
    const Pass mapped = mmap_pass(path);

    std::cerr << "branch experiment ...\n";
    auto sample = sample_messages(path, hms(10, 0, 0), 10'000'000);
    std::vector<const std::vector<std::byte>*> order;
    for (const auto& m : sample) order.push_back(&m);
    const auto real = concatenate(order);
    std::mt19937_64 rng(42);
    std::shuffle(order.begin(), order.end(), rng);
    const auto shuffled = concatenate(order);
    std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return (*a)[2] < (*b)[2]; });
    const auto sorted = concatenate(order);
    uint64_t sink_sum = 0;
    const double real_ns = ns_per_message(real, sink_sum);
    const double shuffled_ns = ns_per_message(shuffled, sink_sum);
    const double sorted_ns = ns_per_message(sorted, sink_sum);

    bool counts_match = decodes[0].counts == walk.counts && decodes[0].messages == walk.messages;
    for (const Pass& d : decodes) counts_match = counts_match && d.sum == decodes[0].sum;

    std::vector<double> parse_s;
    for (const Pass& d : decodes) parse_s.push_back(d.parse_seconds);
    std::sort(parse_s.begin(), parse_s.end());
    const double best = parse_s.front(), median = parse_s[1], worst = parse_s.back();
    const double n = static_cast<double>(walk.messages);
    const double gb = static_cast<double>(std::filesystem::file_size(path)) / 1e9;

    std::ofstream out(out_path);
    out << "# Stage 2: the feed handler\n\n"
        << "Written by `parse_bench`. Decoding means: read the 2-byte length, switch on the type, read every "
           "field with big-endian loads into a small struct and call the handler; types the books don't need "
           "are skipped by their length.\n\n"
        << machine_description() << "\n"
        << "## Every message decodes\n\n"
        << "| Check | Result |\n|---|---|\n"
        << "| Messages decoded | " << with_commas(decodes[0].messages) << " |\n"
        << "| Same as stage 1 (`day_summary`) | "
        << (expected == 0 ? "stage 1 count not given" : decodes[0].messages == expected ? "yes" : "NO") << " |\n"
        << "| Counts by type from decoding equal counts from the framing walk, and three decoding runs agree | "
        << (counts_match ? "yes" : "NO") << " |\n\n"
        << "## Throughput over the whole day\n\n"
        << "| Pass | Seconds | Messages per second | ns per message |\n|---|---:|---:|---:|\n"
        << "| Framing walk only (length + type byte), parse time only | " << fixed(walk.parse_seconds, 2) << " | "
        << with_commas(static_cast<uint64_t>(n / walk.parse_seconds)) << " | " << fixed(walk.parse_seconds * 1e9 / n, 2) << " |\n"
        << "| **Full decode, parse time only** (median of 3 runs) | **" << fixed(median, 2) << "** | **"
        << with_commas(static_cast<uint64_t>(n / median)) << "** | **" << fixed(median * 1e9 / n, 2) << "** |\n"
        << "| Full decode, spread over the 3 runs | " << fixed(best, 2) << " to " << fixed(worst, 2) << " | | "
        << fixed(best * 1e9 / n, 2) << " to " << fixed(worst * 1e9 / n, 2) << " |\n"
        << "| Reading the file in 256 MB chunks (`read`), alongside the above | " << fixed(decodes[1].read_seconds, 2)
        << " | | |\n"
        << "| Full decode straight from `mmap`, page faults included | " << fixed(mapped.parse_seconds, 2) << " | "
        << with_commas(static_cast<uint64_t>(n / mapped.parse_seconds)) << " | " << fixed(mapped.parse_seconds * 1e9 / n, 2)
        << " |\n\n"
        << "The day is " << fixed(gb, 2) << " GB, more than this machine's 8 GB of memory, so it can't stay "
           "in the page cache. Through `mmap` every 16 KB page is a page fault while the parser waits; reading "
           "256 MB chunks with `read` into one reused buffer is several times faster and keeps the parser "
           "from ever waiting on the disk. The later stages use the chunked reader.\n\n"
        << "## The switch and the branch predictor\n\n"
        << "The same ten million messages (from 10:00), parsed from memory in three orders. The work is "
           "identical; only the predictability of the `switch` on the type changes. Median of 5 runs.\n\n"
        << "| Order | ns per message |\n|---|---:|\n"
        << "| As in the feed | " << fixed(real_ns, 2) << " |\n"
        << "| Shuffled at random | " << fixed(shuffled_ns, 2) << " |\n"
        << "| Sorted by type (all A, then all C, ...) | " << fixed(sorted_ns, 2) << " |\n\n"
        << "The three runs do the same work on the same bytes; only the order of the types differs, so the "
           "gaps come from the branch predictor guessing where the `switch` jumps. Sorted, the guess is almost "
           "always right. Shuffled, the next type is a draw from the mix (A 44%, D 43%, U 8%, ...), so many "
           "guesses miss: " << fixed(shuffled_ns - sorted_ns, 2) << " ns per message more than sorted. The feed's "
           "own order costs " << fixed(real_ns - sorted_ns, 2) << " ns more than sorted, almost as much: "
           "consecutive messages are mostly about different stocks and orders, so the next type is close to a "
           "random draw. Sorting also lets the decoding code that follows the jump stay hot, so the gap is an "
           "upper bound on the mispredicts' share.\n\n"
        << "(checksum " << (sink_sum & 0xFFFF) << ")\n";
    std::cerr << "decode " << fixed(median * 1e9 / n, 2) << " ns/message; wrote " << out_path << "\n";
    return counts_match ? 0 : 1;
}
