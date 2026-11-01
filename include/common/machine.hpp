#pragma once

#include <string>

namespace ts {

// "CPU, cores, memory, OS, compiler, build type and flags" as markdown lines,
// for the top of every results file.
std::string machine_description();

// Pin the calling thread to one core. Linux only; returns false elsewhere.
bool pin_this_thread(int core);

}  // namespace ts
