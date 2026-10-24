#pragma once

#include <pineforge/detail/o1_store.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace a5_fixture {
// Disk pages, not a heap-backed mock. Files are retained below the worktree's
// scratch-s4e for the supervisor; the fixture never deletes a file or uses /tmp.
class DiskBacking final : public pineforge::detail::ScratchBacking {
public:
    DiskBacking()
        : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, 4096) {
        const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path() / "scratch-s4e";
        std::filesystem::create_directories(directory);
        auto path = (directory / "a5strmap-XXXXXX").string();
        fd_ = ::mkstemp(path.data());
        if (fd_ < 0) throw std::runtime_error("a5strmap fixture backing open failed");
        if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("a5strmap fixture backing cloexec failed");
        }
    }
    ~DiskBacking() override { if (fd_ >= 0) ::close(fd_); }
    std::uint64_t page_count() const noexcept override { return extent_; }
protected:
    pineforge::detail::ScratchStatus do_read_page(
        pineforge::detail::ScratchPageId id,
        pineforge::detail::ScratchBuffer output) const noexcept override {
        using pineforge::detail::ScratchStatus;
        if (id >= extent_) return ScratchStatus::not_found;
        return transfer(id, output.data, output.size, false);
    }
    pineforge::detail::ScratchStatus do_write_page(
        pineforge::detail::ScratchPageId id,
        pineforge::detail::ScratchBytes input) noexcept override {
        const auto status = transfer(id, const_cast<std::byte*>(input.data), input.size, true);
        if (status == pineforge::detail::ScratchStatus::ok && id >= extent_) extent_ = id + 1;
        return status;
    }
    pineforge::detail::ScratchStatus do_sync(pineforge::detail::ScratchDurability durability) noexcept override {
        using pineforge::detail::ScratchStatus;
        if (durability == pineforge::detail::ScratchDurability::buffered) return ScratchStatus::ok;
        int status;
        do { status = ::fsync(fd_); } while (status != 0 && errno == EINTR);
        return status == 0 ? ScratchStatus::ok : ScratchStatus::io_error;
    }
private:
    pineforge::detail::ScratchStatus transfer(std::uint64_t id, std::byte* data,
                                             std::size_t size, bool write) const noexcept {
        using pineforge::detail::ScratchStatus;
        if (id > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / page_size())
            return ScratchStatus::io_error;
        std::size_t done = 0;
        while (done < size) {
            const auto offset = static_cast<off_t>(id * page_size() + done);
            const auto n = write ? ::pwrite(fd_, data + done, size - done, offset)
                                 : ::pread(fd_, data + done, size - done, offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return ScratchStatus::io_error;
            done += static_cast<std::size_t>(n);
        }
        return ScratchStatus::ok;
    }
    int fd_ = -1;
    std::uint64_t extent_ = 0;
};

struct Session {
    DiskBacking backing;
    pineforge::detail::o1::Shape shape{4096, 8};
    std::unique_ptr<pineforge::detail::o1::Store> store =
        pineforge::detail::o1::Store::open(shape, backing);
};
} // namespace a5_fixture
