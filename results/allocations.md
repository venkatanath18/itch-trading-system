# Stage 9: allocations on the hot path

Written by `tools/reproduce.sh`. `trade_alloc_check` is the `trade` program linked with a replacement
global `operator new` that counts every call (`src/apps/alloc_counter.cpp`). It builds the whole
system first (start-up may allocate), then counts the calls during the replay of the whole day with the
base configuration:

    allocations during the run: 0

The test `no_allocation_on_the_hot_path` checks the same on a synthetic day, with the log written by
one thread and by two, in every Debug and Release build.
