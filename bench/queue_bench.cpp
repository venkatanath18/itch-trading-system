// Stage 9 measurement: the cost of one hop through a queue between two threads.
//
//   queue_bench [--out results/queues.md]
//
// Queues: the new SPSC ring buffer, the earlier lock-free MPMC queue, the
// earlier SPSC queue from the same file, and a ring buffer behind a std::mutex.
//   ping-pong:  thread A sends a 64-byte message to B through one queue, B sends
//               it back through another; one hop = round trip / 2
//   throughput: A sends 10 million messages as fast as B takes them
// Each test runs 5 times; the table shows the median and the spread.

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/histogram.hpp"
#include "common/machine.hpp"
#include "infra/spsc_queue.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreorder"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "legacy/mpmc_queue.hpp"
#pragma GCC diagnostic pop

using namespace ts;

namespace {

struct Message {
    uint64_t seq;
    uint64_t payload[7];  // 64 bytes in all, like a log record
};

constexpr size_t kCapacity = 1024;

// Same try_push / try_pop interface for every queue.
struct NewSpsc {
    static constexpr const char* name = "SPSC ring buffer (this project)";
    SpscQueue<Message, kCapacity> q;
    bool push(const Message& m) { return q.try_push(m); }
    bool pop(Message& m) { return q.try_pop(m); }
};

struct OldMpmc {
    static constexpr const char* name = "Lock-free MPMC queue (earlier, MPMC.cpp)";
    legacy::MPMC_queue<Message, kCapacity> q;
    bool push(const Message& m) { return q.enqueue(m); }
    bool pop(Message& m) { return q.dequeue(m); }
};

struct OldSpsc {
    static constexpr const char* name = "SPSC queue (earlier, MPMC.cpp)";
    legacy::SPSC_queue<Message, kCapacity> q;
    bool push(const Message& m) { return q.enqueue(m); }
    bool pop(Message& m) { return q.dequeue(m); }
};

struct MutexQueue {
    static constexpr const char* name = "Ring buffer behind a std::mutex";
    std::mutex mutex;
    std::array<Message, kCapacity> items{};
    size_t head = 0, tail = 0;
    bool push(const Message& m) {
        std::lock_guard<std::mutex> lock(mutex);
        if (tail - head == kCapacity) return false;
        items[tail++ % kCapacity] = m;
        return true;
    }
    bool pop(Message& m) {
        std::lock_guard<std::mutex> lock(mutex);
        if (head == tail) return false;
        m = items[head++ % kCapacity];
        return true;
    }
};

// On macOS threads can't be pinned; ask for the performance cores instead.
void fast_thread(int core) {
#if defined(__APPLE__)
    (void)core;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    pin_this_thread(core);
#endif
}

struct Result {
    double hop_mean_ns, hop_p50_ns, hop_p99_ns, throughput_ns;
};

template <class Q>
Result run_once() {
    auto there = std::make_unique<Q>();
    auto back = std::make_unique<Q>();
    constexpr int kPings = 200'000;
    Histogram rtt;

    std::thread echo([&] {
        fast_thread(2);
        Message m{};
        for (int i = 0; i < kPings; ++i) {
            while (!there->pop(m)) {}
            while (!back->push(m)) {}
        }
    });
    fast_thread(1);
    Message m{};
    for (int i = 0; i < 1000; ++i) {  // warm up
        while (!there->push(m)) {}
        while (!back->pop(m)) {}
    }
    for (int i = 0; i < kPings - 1000; ++i) {
        m.seq = static_cast<uint64_t>(i);
        const uint64_t t0 = read_counter();
        while (!there->push(m)) {}
        while (!back->pop(m)) {}
        rtt.record(read_counter() - t0);
    }
    echo.join();

    auto q = std::make_unique<Q>();
    constexpr uint64_t kItems = 10'000'000;
    std::atomic<bool> start{false};
    std::thread consumer([&] {
        fast_thread(2);
        Message in{};
        while (!start.load(std::memory_order_acquire)) {}
        for (uint64_t i = 0; i < kItems; ++i) {
            while (!q->pop(in)) {}
        }
    });
    Message out{};
    Stopwatch watch;
    start.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < kItems; ++i) {
        out.seq = i;
        while (!q->push(out)) {}
    }
    consumer.join();
    const double per_item = watch.seconds() * 1e9 / static_cast<double>(kItems);

    return Result{ticks_to_ns(rtt.mean()) / 2, ticks_to_ns(static_cast<double>(rtt.percentile(0.5))) / 2,
                  ticks_to_ns(static_cast<double>(rtt.percentile(0.99))) / 2, per_item};
}

template <class Q>
void measure(std::ostream& out) {
    std::vector<Result> runs;
    for (int i = 0; i < 5; ++i) runs.push_back(run_once<Q>());
    auto median_and_spread = [&](double Result::*field) {
        std::vector<double> v;
        for (const Result& r : runs) v.push_back(r.*field);
        std::sort(v.begin(), v.end());
        return fixed(v[2], 1) + " (" + fixed(v.front(), 1) + " to " + fixed(v.back(), 1) + ")";
    };
    out << "| " << Q::name << " | " << median_and_spread(&Result::hop_mean_ns) << " | "
        << median_and_spread(&Result::hop_p50_ns) << " | " << median_and_spread(&Result::hop_p99_ns) << " | "
        << median_and_spread(&Result::throughput_ns) << " |\n";
    std::cerr << Q::name << " done\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path = "results/queues.md";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--out") out_path = argv[i + 1];
    }
    std::ofstream out(out_path);
    out << "# Stage 9: the cost of one queue hop\n\n"
        << "Written by `queue_bench`. Two threads, 64-byte messages, queues of " << kCapacity << " slots.\n"
        << "- **One hop**: thread A sends a message to B and B sends it straight back through a second queue; "
           "one hop is half the round trip. 199,000 round trips per run.\n"
        << "- **Throughput**: A pushes 10 million messages as fast as B pops them; time per message. "
           "Here the cache lines move in batches, so it's much cheaper than a lone hop.\n\n"
        << "Each cell: median of 5 runs, with the range across the runs in brackets. Times in ns.\n\n"
        << machine_description();
#if defined(__APPLE__)
    out << "- Threads: macOS can't pin threads to cores; both threads asked for the performance cores "
           "(QoS user-interactive)\n";
#else
    out << "- Threads: pinned to cores 1 and 2\n";
#endif
    out << "\n| Queue | One hop, mean | One hop, median | One hop, 99% | Throughput, per message |\n"
        << "|---|---:|---:|---:|---:|\n";
    measure<NewSpsc>(out);
    measure<OldSpsc>(out);
    measure<OldMpmc>(out);
    measure<MutexQueue>(out);
    if (std::string(counter_name()) == "cntvct_el0") {
        out << "\nThe counter ticks every 41.7 ns, so the median and 99% columns move in steps of about "
               "21 ns (half a tick, since a hop is half a round trip); the means average over many samples.\n";
    }
    std::cerr << "wrote " << out_path << "\n";
    return 0;
}
