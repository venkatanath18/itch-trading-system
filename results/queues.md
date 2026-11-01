# Stage 9: the cost of one queue hop

Written by `queue_bench`. Two threads, 64-byte messages, queues of 1024 slots.
- **One hop**: thread A sends a message to B and B sends it straight back through a second queue; one hop is half the round trip. 199,000 round trips per run.
- **Throughput**: A pushes 10 million messages as fast as B pops them; time per message. Here the cache lines move in batches, so it's much cheaper than a lone hop.

Each cell: median of 5 runs, with the range across the runs in brackets. Times in ns.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns
- Threads: macOS can't pin threads to cores; both threads asked for the performance cores (QoS user-interactive)

| Queue | One hop, mean | One hop, median | One hop, 99% | Throughput, per message |
|---|---:|---:|---:|---:|
| SPSC ring buffer (this project) | 67.7 (63.8 to 68.9) | 62.5 (62.5 to 62.5) | 83.3 (83.3 to 83.3) | 20.0 (19.5 to 20.3) |
| SPSC queue (earlier, MPMC.cpp) | 105.2 (103.9 to 105.5) | 104.2 (104.2 to 104.2) | 125.0 (125.0 to 125.0) | 18.4 (18.1 to 18.8) |
| Lock-free MPMC queue (earlier, MPMC.cpp) | 127.7 (120.1 to 133.9) | 125.0 (125.0 to 125.0) | 166.7 (166.7 to 166.7) | 34.8 (28.9 to 35.0) |
| Ring buffer behind a std::mutex | 2884.0 (2789.5 to 3272.8) | 1208.3 (1208.3 to 1250.0) | 21645.8 (20145.8 to 23645.8) | 56.9 (49.8 to 57.8) |

The counter ticks every 41.7 ns, so the median and 99% columns move in steps of about 21 ns (half a tick, since a hop is half a round trip); the means average over many samples.
