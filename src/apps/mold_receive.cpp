// Stretch: receive the MoldUDP64 feed from mold_replay, find sequence gaps,
// ask the re-request server for what's missing, and deliver every message in
// order to the order book. Checks the result against the file.
//
//   mold_receive <itch file> --sizing <csv> --global <txt> [--port 31001]
//                [--request-port 31002] [--messages 10000000] [--sender "line"]
//                [--out results/network.md]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "book/fast_book.hpp"
#include "book/sorted_levels.hpp"
#include "common/clock.hpp"
#include "common/format.hpp"
#include "common/histogram.hpp"
#include "common/machine.hpp"
#include "infra/log.hpp"
#include "itch/source.hpp"
#include "net/moldudp64.hpp"

using namespace ts;

namespace {

using Book = FastBook<HashIndex, SortedLevels>;

class Receiver {
public:
    Receiver(const BookSizing& sizing, uint16_t port, uint16_t request_port)
        : book_(std::make_unique<Book>(sizing)), feed_(*book_) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        const int size = 8 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        sockaddr_in a = address(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) throw std::runtime_error("bind failed");
        request_to_ = address(request_port);
    }
    ~Receiver() { ::close(fd_); }

    void run() {
        std::vector<std::byte> buf(65536);
        Stopwatch idle;
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 5) > 0) {
                const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
                if (n > 0) {
                    idle = Stopwatch();
                    const uint64_t delivered_before = delivered_;
                    const uint64_t t0 = read_counter();
                    handle(buf.data(), static_cast<size_t>(n));
                    const uint64_t t1 = read_counter();
                    if (delivered_ > delivered_before) per_packet_.record(t1 - t0);
                    ++packets_;
                    if (!started_) {
                        started_ = true;
                        clock_ = Stopwatch();
                    }
                }
            }
            if (end_ != 0 && expected_ == end_) break;
            if (idle.seconds() > 30) throw std::runtime_error("no packets for 30 s");
            // a gap still open after 50 ms: ask again
            if (gap_open() && since_request_.seconds() > 0.05) request_gap();
        }
        seconds_ = clock_.seconds();
        mold::Header done{session_, 0, 0};
        std::byte msg[mold::kHeaderBytes];
        mold::write_header(msg, done);
        for (int i = 0; i < 5; ++i) send_request(msg);
    }

    // stats
    uint64_t delivered_ = 0, packets_ = 0, gaps_ = 0, requests_ = 0, duplicates_ = 0, held_ = 0;
    double seconds_ = 0;
    Histogram per_packet_;
    Fnv1a hash_;
    const Book& book() const { return *book_; }

private:
    static sockaddr_in address(uint16_t port) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return a;
    }

    void handle(const std::byte* data, size_t n) {
        mold::Header h;
        if (!mold::read_header(data, n, h)) return;
        session_ = h.session;
        if (h.count == mold::kEndOfSession) {
            end_ = h.sequence;
            check_gap();
            return;
        }
        if (h.count == 0) return;  // heartbeat
        const uint64_t first = h.sequence, last = h.sequence + h.count;
        if (last <= expected_) {
            ++duplicates_;
            return;
        }
        if (first > expected_) {
            // after a gap: keep the packet for later
            if (!held_packets_.count(first)) {
                held_packets_.emplace(first, std::vector<std::byte>(data, data + n));
                ++held_;
            }
        } else {
            deliver(data, n, first);
            // held packets that now follow on
            while (!held_packets_.empty() && held_packets_.begin()->first <= expected_) {
                auto it = held_packets_.begin();
                deliver(it->second.data(), it->second.size(), it->first);
                held_packets_.erase(it);
            }
        }
        check_gap();
    }

    // Something is missing if packets are held or the session ended past us.
    // A gap that starts somewhere new is counted and requested at once.
    void check_gap() {
        gap_open_ = !held_packets_.empty() || (end_ != 0 && expected_ < end_);
        if (gap_open_ && requested_from_ != expected_) {
            ++gaps_;
            request_gap();
        }
    }

    void deliver(const std::byte* data, size_t n, uint64_t first) {
        mold::PacketReader reader(data, n);
        const std::byte* m;
        uint16_t len;
        for (uint64_t seq = first; reader.next(m, len); ++seq) {
            if (seq < expected_) continue;  // already have it
            const std::byte prefix[2] = {static_cast<std::byte>(len >> 8), static_cast<std::byte>(len)};
            hash_.update(reinterpret_cast<const char*>(prefix), 2);
            hash_.update(reinterpret_cast<const char*>(m), len);
            itch::dispatch(m, len, feed_);
            ++delivered_;
            expected_ = seq + 1;
        }
    }

    bool gap_open() const { return gap_open_; }

    void request_gap() {
        uint64_t until = end_ != 0 ? end_ : expected_ + 1;
        if (!held_packets_.empty()) until = held_packets_.begin()->first;
        // at most 1,000 messages a request, so the answer can't flood the socket buffer
        const uint64_t count = std::min<uint64_t>(until - expected_, 1000);
        if (count == 0) return;
        mold::Header req{session_, expected_, static_cast<uint16_t>(count)};
        std::byte msg[mold::kHeaderBytes];
        mold::write_header(msg, req);
        send_request(msg);
        ++requests_;
        requested_from_ = expected_;
        since_request_ = Stopwatch();
    }

    void send_request(const std::byte* msg) {
        ::sendto(fd_, msg, mold::kHeaderBytes, 0, reinterpret_cast<const sockaddr*>(&request_to_), sizeof(request_to_));
    }

    std::unique_ptr<Book> book_;
    BookFeed<Book> feed_;
    int fd_ = -1;
    sockaddr_in request_to_{};
    mold::Session session_ = mold::make_session("");
    uint64_t expected_ = 1;  // next sequence number to deliver
    uint64_t end_ = 0;       // from the end-of-session packet
    uint64_t requested_from_ = 0;
    bool gap_open_ = false;
    bool started_ = false;
    std::map<uint64_t, std::vector<std::byte>> held_packets_;  // arrived after a gap
    Stopwatch since_request_;
    Stopwatch clock_;
};

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mold_receive <itch file> --sizing <csv> --global <txt> [options]\n";
        return 2;
    }
    const BookSizing sizing = BookSizing::from_profile(arg_value(argc, argv, "--sizing", ""),
                                                       arg_value(argc, argv, "--global", ""));
    const uint64_t wanted = std::stoull(arg_value(argc, argv, "--messages", "10000000"));
    Receiver r(sizing, static_cast<uint16_t>(std::stoi(arg_value(argc, argv, "--port", "31001"))),
               static_cast<uint16_t>(std::stoi(arg_value(argc, argv, "--request-port", "31002"))));
    std::cerr << "receiver ready\n";
    r.run();

    // the same messages straight from the file
    Fnv1a file_hash;
    uint64_t file_messages = 0;
    itch::MessageSource source(argv[1]);
    itch::RawMessage m;
    while (file_messages < wanted && source.next(m)) {
        file_hash.update(reinterpret_cast<const char*>(m.data - 2), m.len + size_t{2});
        ++file_messages;
    }
    const bool same = file_hash.value() == r.hash_.value() && file_messages == r.delivered_;

    std::ofstream out(arg_value(argc, argv, "--out", "results/network.md"));
    out << "# Stretch: the feed over the network\n\n"
        << "Written by `mold_receive`. `mold_replay` sends the first " << with_commas(wanted)
        << " messages of the day as MoldUDP64 packets (at most 1,400 bytes each) over UDP on localhost, paced, "
           "and drops a share of the packets on purpose; the kernel may drop more when the socket buffer fills. `mold_receive` finds each sequence gap, holds the "
           "packets that arrived after it, asks the re-request server for the missing messages, and delivers every "
           "message in order to the order book.\n\n"
        << machine_description() << "\n"
        << "- " << arg_value(argc, argv, "--sender", "sender line not given") << "\n\n"
        << "| | Value |\n|---|---:|\n"
        << "| Messages delivered in order | " << with_commas(r.delivered_) << " |\n"
        << "| **Identical to the file** (count and FNV-1a hash of every message) | **" << (same ? "yes" : "NO")
        << "** |\n"
        << "| Packets received (including resent ones) | " << with_commas(r.packets_) << " |\n"
        << "| Gaps found | " << with_commas(r.gaps_) << " |\n"
        << "| Re-requests sent | " << with_commas(r.requests_) << " |\n"
        << "| Packets held because they arrived after a gap | " << with_commas(r.held_) << " |\n"
        << "| Duplicate packets ignored | " << with_commas(r.duplicates_) << " |\n"
        << "| Orders in the book at the end | " << with_commas(r.book().order_count()) << " |\n"
        << "| Time from first packet to last message | " << fixed(r.seconds_, 2) << " s |\n"
        << "| Messages per second, end to end | "
        << with_commas(static_cast<uint64_t>(static_cast<double>(r.delivered_) / r.seconds_)) << " |\n\n"
        << "Receive path, per packet that delivered messages, from `recv` returning to the last message of the "
           "packet applied to the book (about 40 messages a packet; " << with_commas(r.per_packet_.count())
        << " packets): median "
        << fixed(ticks_to_ns(static_cast<double>(r.per_packet_.percentile(0.5))), 0) << " ns, 99% "
        << fixed(ticks_to_ns(static_cast<double>(r.per_packet_.percentile(0.99))), 0) << " ns, mean "
        << fixed(ticks_to_ns(r.per_packet_.mean()), 0)
        << " ns. That excludes the kernel's UDP stack and the copy into the socket buffer, which a "
           "kernel-bypass network card would remove.\n";
    std::cerr << "delivered " << r.delivered_ << ", identical: " << (same ? "yes" : "NO") << ", gaps " << r.gaps_ << "\n";
    return same ? 0 : 1;
}
