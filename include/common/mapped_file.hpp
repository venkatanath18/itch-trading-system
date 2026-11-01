#pragma once

#include <cstddef>
#include <string>

namespace ts {

// A read-only view of a whole file through mmap.
class MappedFile {
public:
    explicit MappedFile(const std::string& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::byte* data() const { return data_; }
    size_t size() const { return size_; }

    // Read every page once so a later pass doesn't wait on the disk
    // (only useful when the file fits in memory).
    void touch_pages() const;

private:
    const std::byte* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace ts
