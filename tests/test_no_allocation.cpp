// A separate test program with a counting operator new: the day's run must
// not allocate (start-up may).
#include <cstdio>

#include "../src/apps/alloc_counter.hpp"
#include "book_types_under_test.hpp"
#include "itch/source.hpp"
#include "synthetic_day.hpp"

using namespace ts;

template <class Log>
int check(const char* name) {
    const auto day = synthetic_day(5);
    Log log("");
    auto system = std::make_unique<TradingSystem<FastHashSorted, Log>>(synthetic_config(10'000), test_sizing(), log);
    itch::BufferSource source(day.data(), day.size());
    const uint64_t before = allocation_count();
    system->run(source);
    const uint64_t during = allocation_count() - before;
    log.finish();
    std::printf("%s: %llu allocations during the run, %llu fills\n", name, static_cast<unsigned long long>(during),
                static_cast<unsigned long long>(system->counts().fills));
    return during == 0 && system->counts().fills > 0 ? 0 : 1;
}

int main() { return check<InlineLog>("single thread") | check<ThreadedLog>("two threads"); }
