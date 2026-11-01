#pragma once

#include <cstdint>

// Number of calls to operator new so far (only in builds that link alloc_counter.cpp).
uint64_t allocation_count();
