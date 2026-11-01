// Replaces the global operator new so a build can count allocations: the
// trade_alloc_check program compares the count before and after the day's
// run, which must be zero on the hot path.
#include "alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
std::atomic<uint64_t> g_allocations{0};

void* allocate(std::size_t n, std::size_t align) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (align <= alignof(std::max_align_t)) {
        p = std::malloc(n ? n : 1);
    } else if (posix_memalign(&p, align, n ? n : 1) != 0) {
        p = nullptr;
    }
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
}  // namespace

uint64_t allocation_count() { return g_allocations.load(std::memory_order_relaxed); }

void* operator new(std::size_t n) { return allocate(n, 0); }
void* operator new[](std::size_t n) { return allocate(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
