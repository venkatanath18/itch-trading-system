// Stretch: send the start of the day as MoldUDP64 packets over UDP on
// localhost, dropping some packets on purpose, and serve re-requests.
//
//   mold_replay <itch file> [--port 31001] [--request-port 31002]
//               [--messages 10000000] [--loss 0.001] [--seed 1] [--rate 100000]
//
// --rate paces the feed in packets per second, as a real feed is paced by
// the market; sent all at once, most packets would overflow the receiver's
// socket buffer and be lost in the kernel.
//
// The receiver (mold_receive) sees the gaps, asks this program's re-request
// server for the missing messages, and checks it got the file exactly.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.hpp"
#include "common/format.hpp"
#include "itch/source.hpp"
#include "net/moldudp64.hpp"

using namespace ts;

namespace {

const mold::Session kSession = mold::make_session("P6REPLAY");

sockaddr_in localhost(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

int udp_socket(uint16_t bind_port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    const int size = 4 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    if (bind_port != 0) {
        sockaddr_in a = localhost(bind_port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) throw std::runtime_error("bind failed");
    }
    return fd;
}

// The messages kept for re-requests: bytes without their length prefixes.
struct Store {
    std::vector<std::byte> bytes;
    std::vector<uint64_t> start;  // start[i] = offset of message with sequence number i + 1
    uint64_t count() const { return start.size() - 1; }
    const std::byte* message(uint64_t seq) const { return bytes.data() + start[seq - 1]; }
    uint16_t length(uint64_t seq) const { return static_cast<uint16_t>(start[seq] - start[seq - 1]); }
};

// Send messages [first, first + n) as packets to `to`, from socket fd.
// drop(): whether to skip a packet on purpose. Returns packets sent.
template <class Drop>
uint64_t send_range(int fd, const sockaddr_in& to, const Store& store, uint64_t first, uint64_t n, Drop drop,
                    uint64_t& dropped, double packets_per_second = 0) {
    mold::PacketBuilder packet(kSession);
    uint64_t sent = 0;
    const auto gap = std::chrono::nanoseconds(packets_per_second > 0 ? static_cast<int64_t>(1e9 / packets_per_second) : 0);
    auto next = std::chrono::steady_clock::now();
    auto flush = [&] {
        if (packet.count() == 0) return;
        if (gap.count() > 0) {
            while (std::chrono::steady_clock::now() < next) {}
            next += gap;
        }
        if (drop()) {
            ++dropped;
        } else {
            ::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
            ++sent;
        }
    };
    packet.start(first);
    for (uint64_t seq = first; seq < first + n; ++seq) {
        if (!packet.add(store.message(seq), store.length(seq))) {
            flush();
            packet.start(seq);
            packet.add(store.message(seq), store.length(seq));
        }
    }
    flush();
    return sent;
}

std::string arg_value(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return argv[i + 1];
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mold_replay <itch file> [--port P] [--request-port P] [--messages N] [--loss p]\n";
        return 2;
    }
    const auto port = static_cast<uint16_t>(std::stoi(arg_value(argc, argv, "--port", "31001")));
    const auto request_port = static_cast<uint16_t>(std::stoi(arg_value(argc, argv, "--request-port", "31002")));
    const uint64_t wanted = std::stoull(arg_value(argc, argv, "--messages", "10000000"));
    const double loss = std::stod(arg_value(argc, argv, "--loss", "0.001"));
    const double rate = std::stod(arg_value(argc, argv, "--rate", "100000"));
    std::mt19937_64 rng(std::stoull(arg_value(argc, argv, "--seed", "1")));

    Store store;
    store.start.push_back(0);
    itch::MessageSource source(argv[1]);
    itch::RawMessage m;
    while (store.count() < wanted && source.next(m)) {
        store.bytes.insert(store.bytes.end(), m.data, m.data + m.len);
        store.start.push_back(store.bytes.size());
    }

    // re-request server
    const int request_fd = udp_socket(request_port);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> requests{0}, resent_messages{0};
    std::thread server([&] {
        std::byte buf[64];
        uint64_t unused = 0;
        while (!done.load()) {
            pollfd p{request_fd, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(request_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
            mold::Header h;
            if (n < 0 || !mold::read_header(buf, static_cast<size_t>(n), h)) continue;
            if (h.sequence == 0 && h.count == 0) {  // the receiver has everything
                done.store(true);
                break;
            }
            const uint64_t first = std::min<uint64_t>(std::max<uint64_t>(h.sequence, 1), store.count() + 1);
            const uint64_t count = std::min<uint64_t>(h.count, store.count() + 1 - first);
            ++requests;
            resent_messages += count;
            send_range(request_fd, from, store, first, count, [] { return false; }, unused);
        }
    });

    // the feed, as fast as it goes
    const int fd = udp_socket(0);
    const sockaddr_in to = localhost(port);
    uint64_t dropped = 0;
    std::uniform_real_distribution<double> u(0, 1);
    Stopwatch watch;
    const uint64_t sent = send_range(fd, to, store, 1, store.count(), [&] { return u(rng) < loss; }, dropped, rate);
    const double seconds = watch.seconds();

    // end of session, repeated until the receiver says it's done (or 30 s)
    mold::Header end{kSession, store.count() + 1, mold::kEndOfSession};
    std::byte end_packet[mold::kHeaderBytes];
    mold::write_header(end_packet, end);
    Stopwatch waiting;
    while (!done.load() && waiting.seconds() < 30) {
        ::sendto(fd, end_packet, sizeof(end_packet), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    done.store(true);
    server.join();
    ::close(fd);
    ::close(request_fd);

    std::printf("sender: %s messages in %s packets at up to %s packets a second (%s dropped on purpose, loss "
                "%.4f) in %.2f s; %s re-requests answered with %s messages\n",
                with_commas(store.count()).c_str(), with_commas(sent + dropped).c_str(),
                with_commas(static_cast<uint64_t>(rate)).c_str(), with_commas(dropped).c_str(), loss, seconds,
                with_commas(requests.load()).c_str(), with_commas(resent_messages.load()).c_str());
    return 0;
}
