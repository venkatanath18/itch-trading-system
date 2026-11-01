#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace ts {

// The cycle counter, used only to measure latency. Simulated time always
// comes from the feed's timestamps.
//   x86-64: rdtsc, a few tenths of a nanosecond per tick.
//   arm64:  the generic timer (cntvct_el0). On Apple Silicon it runs at
//           24 MHz, so one tick is about 41.7 ns.
inline uint64_t read_counter() {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline const char* counter_name() {
#if defined(__x86_64__)
    return "rdtsc";
#elif defined(__aarch64__)
    return "cntvct_el0";
#else
    return "steady_clock";
#endif
}

namespace detail {
inline double measure_nanos_per_tick() {
#if defined(__aarch64__)
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return 1e9 / static_cast<double>(freq);
#elif defined(__x86_64__)
    // calibrate the TSC against the steady clock over 200 ms
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = read_counter();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t c1 = read_counter();
    auto t1 = std::chrono::steady_clock::now();
    double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(c1 - c0);
#else
    return 1.0;
#endif
}
}  // namespace detail

inline double nanos_per_tick() {
    static const double value = detail::measure_nanos_per_tick();
    return value;
}

inline double ticks_to_ns(double ticks) { return ticks * nanos_per_tick(); }

// Wall-clock stopwatch for whole runs.
class Stopwatch {
public:
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

}  // namespace ts
