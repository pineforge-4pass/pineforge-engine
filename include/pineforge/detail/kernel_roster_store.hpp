#pragma once

#include <algorithm>
#include <array>
#include <cassert>
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

namespace pineforge::detail {

// Storage only: the kernel allocates dense handles 1, 2, ... in exactly the
// old order. Keep their identities independently of the nonempty rosters.
// API for replacing this helper with L19l part A's table:
// set_directory/check_directory, append(handle), contains(handle), size().
// Like L19a's successor roots, a fixed tail spills to an unlinked sorted
// scratch file; reads use pread and errors propagate as std::system_error.
class KernelRosterStore {
public:
    KernelRosterStore() = default;
    KernelRosterStore(const KernelRosterStore&) = delete;
    KernelRosterStore& operator=(const KernelRosterStore&) = delete;
    ~KernelRosterStore() { if (fd_ >= 0) ::close(fd_); }

    void check_directory(const std::string& directory) const {
        if (fd_ >= 0 && directory_ != directory) {
            throw std::logic_error("native kernel rosters directory changed after spill");
        }
    }

    void set_directory(const std::string& directory) {
        check_directory(directory);
        directory_ = directory;
    }

    std::uint64_t size() const noexcept { return disk_rows_ + tail_size_; }

    void append(std::uint64_t handle) {
        const auto maximum_rows =
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / sizeof(std::uint64_t);
        if (size() >= maximum_rows) {
            throw std::overflow_error("native kernel rosters file offset exhausted");
        }
        if (handle != size() + 1) {
            throw std::invalid_argument("native kernel roster handles must be dense and ascending");
        }
        if (tail_size_ == tail_.size()) spill();
        tail_[tail_size_++] = handle;
    }

    bool contains(std::uint64_t handle) const {
        if (handle == 0 || handle > size()) return false;
        if (tail_size_ != 0 && handle >= tail_[0]) {
            const auto end = tail_.begin() + static_cast<std::ptrdiff_t>(tail_size_);
            return std::binary_search(tail_.begin(), end, handle);
        }
        std::uint64_t first = 0;
        std::uint64_t last = disk_rows_;
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            const auto found = read(middle);
            if (found == handle) return true;
            if (found < handle) first = middle + 1;
            else last = middle;
        }
        // Every handle inside [1,size()] exists; a missing one is corrupt
        // storage, never permission to treat a recurring id as a new cohort.
        throw std::system_error(EIO, std::generic_category(), "native kernel rosters missing handle");
    }

private:
    static constexpr std::size_t kTailRows = 64;
    static_assert(sizeof(std::uint64_t) == 8, "kernel roster scratch rows are eight bytes");
    std::array<std::uint64_t, kTailRows> tail_{};
    std::size_t tail_size_ = 0;
    std::uint64_t disk_rows_ = 0;
    int fd_ = -1;
    std::string directory_;

    void open_file() {
        if (fd_ >= 0) return;
        const char* temporary = std::getenv("TMPDIR");
        std::string name = directory_.empty()
            ? (temporary && *temporary ? temporary : "/tmp") : directory_;
        if (name.back() != '/') name += '/';
        name += "pineforge-kernel-rosters-XXXXXX";
        const int opened = ::mkstemp(name.data());
        if (opened < 0) {
            throw std::system_error(errno, std::generic_category(), "native kernel rosters create");
        }
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            throw std::system_error(error, std::generic_category(), "native kernel rosters unlink");
        }
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            ::close(opened);
            throw std::system_error(error, std::generic_category(), "native kernel rosters cloexec");
        }
        fd_ = opened;
    }

    void spill() {
        open_file();
        const auto offset = static_cast<off_t>(disk_rows_ * sizeof(std::uint64_t));
        const auto* bytes = reinterpret_cast<const unsigned char*>(tail_.data());
        const auto length = tail_size_ * sizeof(std::uint64_t);
        std::size_t written = 0;
        while (written < length) {
            const auto count = ::pwrite(fd_, bytes + written, length - written,
                                        offset + static_cast<off_t>(written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const int error = count < 0 ? errno : EIO;
                throw std::system_error(error, std::generic_category(), "native kernel rosters write");
            }
            written += static_cast<std::size_t>(count);
        }
        // A partial/error write leaves this whole tail authoritative. A
        // retry overwrites the same offset and publishes only a full batch.
        disk_rows_ += tail_size_;
        tail_size_ = 0;
    }

    std::uint64_t read(std::uint64_t index) const {
        assert(fd_ >= 0 && index < disk_rows_);
        std::uint64_t handle = 0;
        auto* bytes = reinterpret_cast<unsigned char*>(&handle);
        const auto offset = static_cast<off_t>(index * sizeof(std::uint64_t));
        std::size_t received = 0;
        while (received < sizeof handle) {
            const auto count = ::pread(fd_, bytes + received, sizeof handle - received,
                                       offset + static_cast<off_t>(received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const int error = count < 0 ? errno : EIO;
                throw std::system_error(error, std::generic_category(), "native kernel rosters read");
            }
            received += static_cast<std::size_t>(count);
        }
        if (handle != index + 1) {
            throw std::system_error(EIO, std::generic_category(), "native kernel rosters invalid row");
        }
        return handle;
    }
};

} // namespace pineforge::detail
