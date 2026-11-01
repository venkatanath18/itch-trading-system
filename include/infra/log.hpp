#pragma once

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/types.hpp"
#include "infra/spsc_queue.hpp"

namespace ts {

enum class LogKind : uint8_t {
    Session, Order, RiskReject, Ack, Reject, ReplaceAck, ReplaceReject, CancelAck, CancelReject, Fill, Kill, Mark
};

inline const char* to_string(LogKind k) {
    switch (k) {
        case LogKind::Session: return "SESSION";
        case LogKind::Order: return "ORDER";
        case LogKind::RiskReject: return "RISK_REJECT";
        case LogKind::Ack: return "ACK";
        case LogKind::Reject: return "REJECT";
        case LogKind::ReplaceAck: return "REPLACE_ACK";
        case LogKind::ReplaceReject: return "REPLACE_REJECT";
        case LogKind::CancelAck: return "CANCEL_ACK";
        case LogKind::CancelReject: return "CANCEL_REJECT";
        case LogKind::Fill: return "FILL";
        case LogKind::Kill: return "KILL";
        case LogKind::Mark: return "MARK";
    }
    return "?";
}

// One line of the output log, fixed size so it can cross threads by copy.
struct LogRecord {
    uint64_t seq = 0;       // sequencer number of the event
    Timestamp time = 0;     // feed time
    LogKind kind = LogKind::Session;
    char side = '-';
    uint8_t code = 0;       // intent type, risk verdict, reject reason or session event
    Locate locate = 0;
    uint64_t id = 0;
    uint64_t old_id = 0;
    Price price = 0;
    Qty qty = 0;
    Qty remaining = 0;
    int64_t value = 0;      // Mark: position; Kill: budget
    int64_t value2 = 0;     // Mark, Kill: equity
};

inline constexpr const char* kLogHeader = "seq,time,kind,locate,side,id,old_id,price,qty,remaining,code,value,value2\n";

inline constexpr size_t kLineMax = 256;  // the longest line is about 190 characters

// The CSV line for a record; returns its length. buf holds kLineMax bytes;
// every write is checked against the end.
inline size_t format(const LogRecord& r, char* buf) {
    char* p = buf;
    char* const end = buf + kLineMax;
    auto put = [&](char c) {
        if (p < end) *p++ = c;
    };
    auto num = [&](auto v) {
        p = std::to_chars(p, end, v).ptr;
        put(',');
    };
    num(r.seq);
    num(r.time);
    for (const char* k = to_string(r.kind); *k != '\0'; ++k) put(*k);
    put(',');
    num(r.locate);
    put(r.side);
    put(',');
    num(r.id);
    num(r.old_id);
    num(r.price);
    num(r.qty);
    num(r.remaining);
    num(static_cast<unsigned>(r.code));
    num(r.value);
    p = std::to_chars(p, end, r.value2).ptr;
    put('\n');
    return static_cast<size_t>(p - buf);
}

// FNV-1a over every byte written, to compare whole logs cheaply.
class Fnv1a {
public:
    void update(const char* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h_ ^= static_cast<unsigned char>(p[i]);
            h_ *= 0x100000001b3ull;
        }
    }
    uint64_t value() const { return h_; }

private:
    uint64_t h_ = 0xcbf29ce484222325ull;
};

// Formats and writes the log. One instance per thread that writes.
class LogWriter {
public:
    explicit LogWriter(const std::string& path) {
        if (!path.empty()) {
            file_ = std::fopen(path.c_str(), "wb");
            if (file_ == nullptr) throw std::runtime_error("cannot write " + path);
        }
        buf_ = static_cast<char*>(std::malloc(kBuf));
        add(kLogHeader, std::strlen(kLogHeader));
    }
    ~LogWriter() {
        flush();
        if (file_) std::fclose(file_);
        std::free(buf_);
    }
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    void write(const LogRecord& r) {
        char line[kLineMax];
        add(line, format(r, line));
        ++records_;
    }

    void flush() {
        if (file_ && used_ > 0) std::fwrite(buf_, 1, used_, file_);
        if (file_) std::fflush(file_);
        used_ = 0;
    }

    uint64_t hash() const { return hash_.value(); }
    uint64_t records() const { return records_; }
    uint64_t bytes() const { return bytes_; }
    uint64_t flushes() const { return flushes_; }

private:
    static constexpr size_t kBuf = size_t{64} << 20;

    void add(const char* p, size_t n) {
        hash_.update(p, n);
        bytes_ += n;
        if (used_ + n > kBuf) {
            ++flushes_;
            flush();
        }
        std::memcpy(buf_ + used_, p, n);
        used_ += n;
    }

    std::FILE* file_ = nullptr;
    char* buf_ = nullptr;
    size_t used_ = 0;
    Fnv1a hash_;
    uint64_t records_ = 0;
    uint64_t bytes_ = 0;
    uint64_t flushes_ = 0;
};

// Single-threaded: the hot path formats into a 64 MB buffer and the file is
// written when the buffer fills (a system call on the hot path, counted in
// flushes()) and at the end.
class InlineLog {
public:
    explicit InlineLog(const std::string& path) : writer_(path) {}
    void write(const LogRecord& r) { writer_.write(r); }
    void finish() { writer_.flush(); }
    uint64_t hash() const { return writer_.hash(); }
    uint64_t records() const { return writer_.records(); }
    uint64_t bytes() const { return writer_.bytes(); }
    uint64_t full_waits() const { return 0; }
    static constexpr const char* name = "single thread";

private:
    LogWriter writer_;
};

// Two threads: the hot path copies each record into an SPSC queue; a second
// thread takes them out, formats and writes them. Same bytes as InlineLog.
class ThreadedLog {
public:
    explicit ThreadedLog(const std::string& path, int core = -1) : queue_(std::make_unique<Queue>()), writer_(path) {
        thread_ = std::thread([this, core] { drain(core); });
    }
    ~ThreadedLog() { finish(); }

    // Never drops: if the queue is full the hot path waits (counted).
    void write(const LogRecord& r) {
        while (!queue_->try_push(r)) ++full_waits_;
    }

    void finish() {
        if (!thread_.joinable()) return;
        done_.store(true, std::memory_order_release);
        thread_.join();
        writer_.flush();
    }

    uint64_t hash() const { return writer_.hash(); }
    uint64_t records() const { return writer_.records(); }
    uint64_t bytes() const { return writer_.bytes(); }
    uint64_t full_waits() const { return full_waits_; }
    static constexpr const char* name = "two threads";

private:
    using Queue = SpscQueue<LogRecord, 1 << 16>;

    void drain(int core);

    std::unique_ptr<Queue> queue_;
    LogWriter writer_;
    std::thread thread_;
    std::atomic<bool> done_{false};
    uint64_t full_waits_ = 0;
};

}  // namespace ts
