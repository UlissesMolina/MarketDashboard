#pragma once

// Read-only memory mapping of a whole file (Linux).

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace itch {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + path);
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "fstat " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                const int err = errno;
                ::close(fd);
                throw std::system_error(err, std::generic_category(), "mmap " + path);
            }
            data_ = static_cast<const unsigned char*>(p);
            ::madvise(p, size_, MADV_SEQUENTIAL);
        }
        ::close(fd);  // the mapping keeps its own reference
    }

    MappedFile(MappedFile&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    MappedFile& operator=(MappedFile&&) = delete;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() {
        if (data_) ::munmap(const_cast<unsigned char*>(data_), size_);
    }

    [[nodiscard]] const unsigned char* data() const noexcept { return data_; }
    [[nodiscard]] const unsigned char* end() const noexcept { return data_ + size_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Touch every page so later passes measure parsing, not page faults / disk.
    // Returns a value derived from the bytes so the loop cannot be optimised out.
    unsigned prefault() const noexcept {
        unsigned acc = 0;
        for (std::size_t i = 0; i < size_; i += 4096) acc += data_[i];
        return acc;
    }

private:
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace itch
