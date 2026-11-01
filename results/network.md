# Stretch: the feed over the network

Written by `mold_receive`. `mold_replay` sends the first 10,000,000 messages of the day as MoldUDP64 packets (at most 1,400 bytes each) over UDP on localhost, paced, and drops a share of the packets on purpose; the kernel may drop more when the socket buffer fills. `mold_receive` finds each sequence gap, holds the packets that arrived after it, asks the re-request server for the missing messages, and delivers every message in order to the order book.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

- sender: 10,000,000 messages in 249,420 packets at up to 100,000 packets a second (253 dropped on purpose, loss 0.0010) in 2.49 s; 253 re-requests answered with 10,206 messages

| | Value |
|---|---:|
| Messages delivered in order | 10,000,000 |
| **Identical to the file** (count and FNV-1a hash of every message) | **yes** |
| Packets received (including resent ones) | 249,422 |
| Gaps found | 253 |
| Re-requests sent | 253 |
| Packets held because they arrived after a gap | 6,178 |
| Duplicate packets ignored | 1 |
| Orders in the book at the end | 1,629,240 |
| Time from first packet to last message | 2.49 s |
| Messages per second, end to end | 4,009,525 |

Receive path, per packet that delivered messages, from `recv` returning to the last message of the packet applied to the book (about 40 messages a packet; 243,242 packets): median 3625 ns, 99% 13625 ns, mean 4823 ns. That excludes the kernel's UDP stack and the copy into the socket buffer, which a kernel-bypass network card would remove.
