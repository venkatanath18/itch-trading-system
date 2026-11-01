#include "common/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ts {

MappedFile::MappedFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("cannot stat " + path);
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ > 0) {
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            throw std::runtime_error("cannot mmap " + path + ": " + std::strerror(errno));
        }
        ::madvise(p, size_, MADV_SEQUENTIAL);
        data_ = static_cast<const std::byte*>(p);
    }
    ::close(fd);  // the mapping stays valid
}

MappedFile::~MappedFile() {
    if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
}

void MappedFile::touch_pages() const {
    volatile unsigned char sink = 0;
    const long page = ::sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < size_; i += static_cast<size_t>(page)) {
        sink = static_cast<unsigned char>(sink ^ static_cast<unsigned char>(data_[i]));
    }
}

}  // namespace ts
