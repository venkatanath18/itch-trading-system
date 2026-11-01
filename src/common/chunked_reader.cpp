#include "common/chunked_reader.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "common/clock.hpp"

namespace ts {

ChunkedReader::ChunkedReader(const std::string& path, size_t chunk_bytes) : chunk_(chunk_bytes) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    struct stat st {};
    ::fstat(fd_, &st);
    file_size_ = static_cast<size_t>(st.st_size);
    if (chunk_ > file_size_) chunk_ = file_size_ == 0 ? 1 : file_size_;

    // room for one chunk plus the longest possible leftover message
    capacity_ = chunk_ + 65536 + 2;
    buffer_ = static_cast<std::byte*>(std::malloc(capacity_));
    if (buffer_ == nullptr) throw std::runtime_error("out of memory for the read buffer");
    std::memset(buffer_, 0, capacity_);  // fault every page in now, not during processing
}

ChunkedReader::~ChunkedReader() {
    std::free(buffer_);
    if (fd_ >= 0) ::close(fd_);
}

bool ChunkedReader::fill() {
    const size_t tail = size_ - consumed_;
    if (tail > 0) std::memmove(buffer_, buffer_ + consumed_, tail);
    size_ = tail;
    consumed_ = 0;

    Stopwatch watch;
    size_t added = 0;
    while (added < chunk_) {
        ssize_t n = ::read(fd_, buffer_ + size_ + added, chunk_ - added);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        if (n == 0) break;
        added += static_cast<size_t>(n);
    }
    read_seconds_ += watch.seconds();
    size_ += added;
    return added > 0;
}

}  // namespace ts
