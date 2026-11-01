#pragma once

#include <cstddef>
#include <string>

namespace ts {

// Reads a file in large chunks into one buffer that is reused, so the code
// that processes the bytes never waits on the disk or takes a page fault.
// The caller says how many bytes it used; the rest (a message cut in two by
// the chunk boundary) moves to the front and the next read continues after it.
class ChunkedReader {
public:
    ChunkedReader(const std::string& path, size_t chunk_bytes);
    ~ChunkedReader();
    ChunkedReader(const ChunkedReader&) = delete;
    ChunkedReader& operator=(const ChunkedReader&) = delete;

    // Keep the unused tail and read up to one more chunk. False when the file
    // has nothing left to read.
    bool fill();

    const std::byte* data() const { return buffer_; }
    size_t size() const { return size_; }
    void consume(size_t n) { consumed_ = n; }

    double read_seconds() const { return read_seconds_; }
    size_t file_size() const { return file_size_; }

private:
    int fd_ = -1;
    std::byte* buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t chunk_ = 0;
    size_t size_ = 0;
    size_t consumed_ = 0;
    size_t file_size_ = 0;
    double read_seconds_ = 0;
};

}  // namespace ts
