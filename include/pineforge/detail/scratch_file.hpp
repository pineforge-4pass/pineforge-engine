#pragma once

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace pineforge::detail {

// Keep the errno and std::system_error contract of the successor-root file,
// and carry the run's typed scratch-I/O failure through the ordinary catcher.
class ScratchIoError final : public std::system_error, public RunFailureInfo {
public:
    ScratchIoError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, {}) {}
};

// Run-owned, anonymous, close-on-exec scratch. All reads/writes are positional;
// no shared cursor, global descriptor, mapping, or growing userspace buffer.
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
            bounded_cache_ = other.bounded_cache_;
            dirty_begin_ = other.dirty_begin_;
            dirty_end_ = other.dirty_end_;
            other.dirty_begin_ = other.dirty_end_ = 0;
        }
        return *this;
    }
    ~ScratchFile() { clear(); }

    // Opt in before opening. Only the ledger/phase consumers request this;
    // existing scratch users keep their established I/O behavior.
    void bound_cache() {
        if (fd_ >= 0) throw std::logic_error("scratch cache policy changed after open");
        bounded_cache_ = true;
    }
    void set_directory(const std::string& value) {
        if (fd_ >= 0 && directory_ != value)
            throw std::logic_error("scratch directory changed after spill");
        directory_ = value;
    }
    void clear() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        dirty_begin_ = dirty_end_ = 0;
    }
    void open(const char* prefix) {
        if (fd_ >= 0) return;
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
        if (bounded_cache_) {
#if defined(__linux__)
            struct statfs backing{};
            if (::fstatfs(fd_, &backing) != 0) {
                const int error = errno;
                clear();
                throw ScratchIoError(error, "scratch backing");
            }
            // These are memory filesystems, not an external row source.
            if (backing.f_type == 0x01021994 || backing.f_type == 0x858458f6) {
                clear();
                throw ScratchIoError(ENODEV, "scratch requires disk backing");
            }
            const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
            if (error != 0) {
                clear();
                throw ScratchIoError(error, "scratch random access");
            }
#elif defined(__APPLE__)
            if (::fcntl(fd_, F_NOCACHE, 1) == -1) {
                const int error = errno;
                clear();
                throw ScratchIoError(error, "scratch uncached access");
            }
#endif
        }
    }
    static void check_extent(std::uint64_t offset, std::size_t length) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw ScratchIoError(EFBIG, "scratch offset exhausted");
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        check_extent(offset, length);
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t written = 0;
        while (written < length) {
            const auto position = offset + written;
            const auto chunk = std::min(length - written, bounded_cache_
                ? kCacheBytes - static_cast<std::size_t>(position % kCacheBytes)
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            prepare_write(position, chunk);
            const ssize_t count = ::pwrite(fd_, bytes + written, chunk,
                                           static_cast<off_t>(offset + written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, "scratch write");
            written += static_cast<std::size_t>(count);
        }
    }
    void read(std::uint64_t offset, void* data, std::size_t length) const {
        check_extent(offset, length);
        auto* bytes = static_cast<unsigned char*>(data);
        std::size_t received = 0;
        while (received < length) {
            const auto chunk = std::min(length - received, bounded_cache_ ? kCacheBytes
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::pread(fd_, bytes + received, chunk,
                                          static_cast<off_t>(offset + received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, "scratch read");
            received += static_cast<std::size_t>(count);
            discard_clean_cache();
        }
    }

private:
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    void discard_clean_cache() const {
#if defined(__linux__)
        if (bounded_cache_) {
            const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
            if (error != 0) throw ScratchIoError(error, "scratch discard cache");
        }
#endif
    }
    void prepare_write(std::uint64_t offset, std::size_t count) {
        if (!bounded_cache_) return;
        const auto first = dirty_begin_ == dirty_end_ ? offset : std::min(offset, dirty_begin_);
        const auto end = std::max(offset + count, dirty_end_);
        if (end - first > kCacheBytes) {
#if defined(__linux__)
            int result;
            do { result = ::fdatasync(fd_); } while (result != 0 && errno == EINTR);
            if (result != 0) throw ScratchIoError(errno, "scratch flush cache window");
#endif
            discard_clean_cache();
            dirty_begin_ = offset;
            dirty_end_ = offset + count;
        } else {
            dirty_begin_ = first;
            dirty_end_ = end;
        }
    }
    int fd_ = -1;
    std::string directory_;
    bool bounded_cache_ = false;
    std::uint64_t dirty_begin_ = 0, dirty_end_ = 0;
};
} // namespace pineforge::detail
