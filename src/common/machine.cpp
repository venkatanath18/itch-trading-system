#include "common/machine.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>

#include "build_info.hpp"
#include "common/clock.hpp"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ts {

namespace {

std::string cpu_model() {
#if defined(__APPLE__)
    char buf[256] = {};
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#elif defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    }
#endif
    return "unknown CPU";
}

uint64_t memory_bytes() {
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return mem;
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
}

}  // namespace

std::string machine_description() {
    utsname u{};
    uname(&u);
    std::ostringstream out;
    out << "- Machine: " << cpu_model() << ", " << std::thread::hardware_concurrency()
        << " cores, " << memory_bytes() / (1024 * 1024 * 1024) << " GB RAM\n"
        << "- OS: " << u.sysname << " " << u.release << " (" << u.machine << ")\n"
        << "- Compiler: " << build::compiler << ", " << build::build_type << " build, flags `"
        << build::flags << "`\n"
        << "- Timer: " << counter_name() << ", one tick = " << nanos_per_tick() << " ns\n";
    return out.str();
}

bool pin_this_thread(int core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false;
#endif
}

}  // namespace ts
