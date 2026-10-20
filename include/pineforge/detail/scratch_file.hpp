#pragma once

#include <pineforge/detail/scratch_arena.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {

// Single-thread-owned positional scratch. open() selects the explicitly
// attached arena, or the original mkstemp/unlink/pwrite/pread backend.
// Sparse growth reads as zero in both modes, including reused arena chunks;
// reads beyond the logical end still fail with EIO. No implicit attachment
// or RAM fallback. clear() releases the region and its mapped windows.
class ScratchFile {
public:
    ScratchFile() = default;
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&& other) noexcept { *this = std::move(other); }
    ScratchFile& operator=(ScratchFile&& other) noexcept {
        if (this != &other) {
            clear();
            fd_ = std::exchange(other.fd_, -1);
            directory_ = std::move(other.directory_);
            arena_ = std::exchange(other.arena_, nullptr);
            region_ = std::move(other.region_);
            other.region_.size = 0;
        }
        return *this;
    }
    ~ScratchFile() { clear(); }

    void set_directory(const std::string& value) {
        if (arena_) return;
        if (fd_ >= 0 && directory_ != value)
            throw std::logic_error("scratch directory changed after spill");
        directory_ = value;
    }
    void clear() noexcept {
        if (arena_) arena_->release_region(region_);
        arena_ = nullptr;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool opened() const noexcept { return fd_ >= 0 || arena_ != nullptr; }
    void open(const char* prefix) {
        if (opened()) return;
        if (auto* arena = ScratchArena::attached()) {
            arena->open_region(region_);
            arena_ = arena;
            return;
        }
        const char* temporary = std::getenv("TMPDIR");
        std::string name = directory_.empty()
            ? (temporary && *temporary ? temporary : "/tmp") : directory_;
        if (name.back() != '/') name += '/';
        name += prefix;
        name += "-XXXXXX";
        const int opened = ::mkstemp(name.data());
        if (opened < 0) throw ScratchIoError(errno, "scratch create");
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            throw ScratchIoError(error, "scratch unlink");
        }
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            ::close(opened);
            throw ScratchIoError(error, "scratch cloexec");
        }
        fd_ = opened;
    }
    // Monotone, zero-filled growth, for sparse hash indexes and similar
    // ftruncate users. Does not shrink or open the file. Arena mode claims
    // chunks without a sizing syscall; file mode uses fstat + ftruncate.
    void ensure_size(std::uint64_t bytes) {
        check_extent(bytes, 0);
        if (!opened()) throw std::logic_error("scratch growth before open");
        if (arena_) {
            arena_->ensure_size(region_, bytes);
            return;
        }
        struct stat status{};
        int result;
        do { result = ::fstat(fd_, &status); } while (result != 0 && errno == EINTR);
        if (result != 0) throw ScratchIoError(errno, "scratch size");
        if (status.st_size >= static_cast<off_t>(bytes)) return;
        do { result = ::ftruncate(fd_, static_cast<off_t>(bytes)); }
        while (result != 0 && errno == EINTR);
        if (result != 0) throw ScratchIoError(errno, "scratch resize");
    }
    static void check_extent(std::uint64_t offset, std::size_t length) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw ScratchIoError(EFBIG, "scratch offset exhausted");
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        check_extent(offset, length);
        if (arena_) {
            arena_->write(region_, offset, data, length);
            return;
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t written = 0;
        while (written < length) {
            const auto chunk = std::min(length - written,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::pwrite(fd_, bytes + written, chunk,
                                           static_cast<off_t>(offset + written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, "scratch write");
            written += static_cast<std::size_t>(count);
        }
    }
    void read(std::uint64_t offset, void* data, std::size_t length) const {
        check_extent(offset, length);
        if (arena_) {
            arena_->read(region_, offset, data, length);
            return;
        }
        auto* bytes = static_cast<unsigned char*>(data);
        std::size_t received = 0;
        while (received < length) {
            const auto chunk = std::min(length - received,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::pread(fd_, bytes + received, chunk,
                                          static_cast<off_t>(offset + received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, "scratch read");
            received += static_cast<std::size_t>(count);
        }
    }

private:
    int fd_ = -1;
    std::string directory_;
    ScratchArena* arena_ = nullptr;
    ScratchArena::Region region_;
};
} // namespace pineforge::detail
