#pragma once

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {

// Keep L19a's system_error surface, carrying the run's resource failure too.
class IdPairStoreError final : public std::system_error, public RunFailureInfo {
public:
    IdPairStoreError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, {}) {}
};

class IdPairScratchFile {
public:
    // Explicit per-instance fixture seam. Counters observe actual syscalls;
    // a selected write writes a real prefix, then reports an IO failure.
    struct FixtureIO {
        std::uint64_t reads = 0, writes = 0;
        std::uint64_t read_bytes = 0, write_bytes = 0;
        std::uint64_t fail_write = 0;
        std::size_t partial_write = 0;
    };
    void fixture_count_io(FixtureIO* io) noexcept { fixture_io_ = io; }
    FixtureIO* fixture_io() const noexcept { return fixture_io_; }

    IdPairScratchFile() = default;
    IdPairScratchFile(const IdPairScratchFile&) = delete;
    IdPairScratchFile& operator=(const IdPairScratchFile&) = delete;
    IdPairScratchFile(IdPairScratchFile&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          fixture_io_(std::exchange(other.fixture_io_, nullptr)) {}
    IdPairScratchFile& operator=(IdPairScratchFile&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
            fixture_io_ = std::exchange(other.fixture_io_, nullptr);
        }
        return *this;
    }
    ~IdPairScratchFile() { reset(); }

    bool opened() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    void open(const std::string& directory) {
        if (opened()) return;
        const char* temporary = std::getenv("TMPDIR");
        std::string name = directory.empty()
            ? (temporary && *temporary ? temporary : "/tmp") : directory;
        if (name.back() != '/') name += '/';
        name += "pineforge-id-pairs-XXXXXX";
        const int opened = ::mkstemp(name.data());
        if (opened < 0) throw IdPairStoreError(errno, "id table create");
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            throw IdPairStoreError(error, "id table unlink");
        }
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            ::close(opened);
            throw IdPairStoreError(error, "id table cloexec");
        }
        fd_ = opened;
    }
    void resize(std::uint64_t bytes) {
        check_range(0, bytes);
        int result;
        do {
            result = ::ftruncate(fd_, static_cast<off_t>(bytes));
        } while (result != 0 && errno == EINTR);
        if (result != 0) throw IdPairStoreError(errno, "id table resize");
    }
    void read(std::uint64_t offset, void* destination, std::size_t length) const {
        check_range(offset, length);
        auto* bytes = static_cast<unsigned char*>(destination);
        std::size_t received = 0;
        while (received < length) {
            if (fixture_io_) ++fixture_io_->reads;
            const ssize_t count = ::pread(fd_, bytes + received, length - received,
                static_cast<off_t>(offset + received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw IdPairStoreError(count < 0 ? errno : EIO, "id table read");
            if (fixture_io_) fixture_io_->read_bytes += static_cast<std::uint64_t>(count);
            received += static_cast<std::size_t>(count);
        }
    }
    void write(std::uint64_t offset, const void* source, std::size_t length) {
        check_range(offset, length);
        const auto* bytes = static_cast<const unsigned char*>(source);
        std::size_t written = 0;
        while (written < length) {
            if (fixture_io_ && ++fixture_io_->writes == fixture_io_->fail_write) {
                const auto prefix = std::min(fixture_io_->partial_write, length - written);
                if (prefix != 0) {
                    const auto count = ::pwrite(fd_, bytes + written, prefix,
                        static_cast<off_t>(offset + written));
                    if (count > 0) fixture_io_->write_bytes += static_cast<std::uint64_t>(count);
                }
                throw IdPairStoreError(EIO, "id table fixture short write");
            }
            const ssize_t count = ::pwrite(fd_, bytes + written, length - written,
                static_cast<off_t>(offset + written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw IdPairStoreError(count < 0 ? errno : EIO, "id table write");
            if (fixture_io_) fixture_io_->write_bytes += static_cast<std::uint64_t>(count);
            written += static_cast<std::size_t>(count);
        }
    }

private:
    static void check_range(std::uint64_t offset, std::uint64_t length) {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw IdPairStoreError(EFBIG, "id table offset exhausted");
    }
    int fd_ = -1;
    FixtureIO* fixture_io_ = nullptr;
};

} // namespace pineforge::detail
