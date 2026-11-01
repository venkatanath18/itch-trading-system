// The whole system on a synthetic day: determinism, and the threaded log
// writing exactly what the single-threaded one writes.
#include <gtest/gtest.h>

#include "book_types_under_test.hpp"
#include "itch/source.hpp"
#include "synthetic_day.hpp"

using namespace ts;

namespace {

template <class Log>
uint64_t run_day(const std::vector<std::byte>& day, Timestamp latency, uint64_t* fills = nullptr,
                 uint64_t* records = nullptr) {
    Log log("");
    auto system = std::make_unique<TradingSystem<FastHashSorted, Log>>(synthetic_config(latency), test_sizing(), log);
    itch::BufferSource source(day.data(), day.size());
    system->run(source);
    log.finish();
    if (fills) *fills = system->counts().fills;
    if (records) *records = log.records();
    return log.hash();
}

}  // namespace

TEST(System, TradesOnTheSyntheticDay) {
    const auto day = synthetic_day(1);
    uint64_t fills = 0, records = 0;
    run_day<InlineLog>(day, 0, &fills, &records);
    EXPECT_GT(fills, 10u);
    EXPECT_GT(records, 100u);
}

TEST(System, TwoRunsGiveTheSameLog) {
    const auto day = synthetic_day(2);
    EXPECT_EQ(run_day<InlineLog>(day, 0), run_day<InlineLog>(day, 0));
    EXPECT_EQ(run_day<InlineLog>(day, 50'000), run_day<InlineLog>(day, 50'000));
}

TEST(System, LatencyChangesTheOutcome) {
    const auto day = synthetic_day(3);
    EXPECT_NE(run_day<InlineLog>(day, 0), run_day<InlineLog>(day, 2'000'000));
}

TEST(System, ThreadedLogMatchesSingleThreaded) {
    const auto day = synthetic_day(4);
    uint64_t r1 = 0, r2 = 0;
    const uint64_t single = run_day<InlineLog>(day, 0, nullptr, &r1);
    const uint64_t threaded = run_day<ThreadedLog>(day, 0, nullptr, &r2);
    EXPECT_EQ(single, threaded);
    EXPECT_EQ(r1, r2);
}
