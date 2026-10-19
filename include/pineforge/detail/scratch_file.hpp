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
        }
        return *this;
    }
    ~ScratchFile() { clear(); }

    void set_directory(const std::string& value) {
        if (fd_ >= 0 && directory_ != value)
            throw std::logic_error("scratch directory changed after spill");
        directory_ = value;
    }
    void clear() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
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
};
} // namespace pineforge::detail
