#pragma once

#include <stdexcept>
#include <string>

#include "common/chunked_reader.hpp"
#include "itch/parser.hpp"

namespace ts::itch {

// The day's messages one at a time, read from the file in large chunks.
// A message stays valid until the next call to next().
class MessageSource {
public:
    explicit MessageSource(const std::string& path, size_t chunk_bytes = size_t{256} << 20)
        : reader_(path, chunk_bytes), cursor_(nullptr, 0) {}

    bool next(RawMessage& m) {
        while (!cursor_.next(m)) {
            if (cursor_.status() == Status::BadLength) throw std::runtime_error("bad message length in the file");
            reader_.consume(cursor_.offset());
            if (!reader_.fill()) {
                if (reader_.size() != 0) throw std::runtime_error("the file ends inside a message");
                return false;
            }
            cursor_ = Cursor(reader_.data(), reader_.size());
        }
        return true;
    }

    double read_seconds() const { return reader_.read_seconds(); }
    size_t file_size() const { return reader_.file_size(); }

private:
    ChunkedReader reader_;
    Cursor cursor_;
};

// The same interface over bytes already in memory (tests, synthetic days).
class BufferSource {
public:
    BufferSource(const std::byte* data, size_t size) : cursor_(data, size) {}
    bool next(RawMessage& m) { return cursor_.next(m); }

private:
    Cursor cursor_;
};

// Decode every message of a file and pass it to the handler.
template <class Handler>
uint64_t replay_file(const std::string& path, Handler& h) {
    MessageSource source(path);
    RawMessage m;
    uint64_t n = 0;
    while (source.next(m)) {
        if (dispatch(m.data, m.len, h) != Status::Ok) throw std::runtime_error("message failed to decode");
        ++n;
    }
    return n;
}

}  // namespace ts::itch
