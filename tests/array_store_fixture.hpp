// Test setup only. Main's o1::Store borrows one empty ScratchBacking.
// An extending disk file keeps the test's backing out of the resident heap.
#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_backing.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace array_test {

inline void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

class SpillFile {
    int fd_ = -1;
    std::uint64_t extent_ = 0;
public:
    SpillFile() {
        // Explicit disk backing on every platform: an extending file under
        // TMPDIR (or /tmp), unlinked right after it is opened so the test
        // leaves nothing behind and never writes into the source tree.
        const char* directory = std::getenv("TMPDIR");
        std::string path = (directory != nullptr && *directory != '\0') ? directory : "/tmp";
        path += "/array-test-XXXXXX";
        fd_ = ::mkstemp(path.data());
        require(fd_ >= 0, "create array test backing");
        ::unlink(path.c_str());
        if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("mark array test backing close-on-exec");
        }
    }
    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;
    ~SpillFile() { if (fd_ >= 0) ::close(fd_); }

    std::uint64_t size() const noexcept { return extent_; }
    std::uint64_t grow(std::uint64_t bytes) noexcept {
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - extent_)
            return 0;
        const auto next = extent_ + bytes;
        if (::ftruncate(fd_, static_cast<off_t>(next)) != 0) return 0;
        extent_ = next;
        return extent_;
    }
    bool transfer(std::uint64_t offset, void* data, std::size_t bytes, bool write) const noexcept {
        if (offset > extent_ || bytes > extent_ - offset) return false;
        auto* buffer = static_cast<unsigned char*>(data);
        for (std::size_t done = 0; done < bytes;) {
            const auto count = std::min<std::size_t>(
                bytes - done, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto position = static_cast<off_t>(offset + done);
            const auto moved = write ? ::pwrite(fd_, buffer + done, count, position)
                                     : ::pread(fd_, buffer + done, count, position);
            if (moved < 0 && errno == EINTR) continue;
            if (moved <= 0) return false;
            done += static_cast<std::size_t>(moved);
        }
        return true;
    }
    bool sync() noexcept { return ::fsync(fd_) == 0; }
};

class FileBacking final : public pineforge::detail::ScratchBacking {
    SpillFile file_;
    std::size_t page_bytes_;
public:
    explicit FileBacking(std::size_t page)
        : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, page), page_bytes_(page) {}
    std::uint64_t page_count() const noexcept override { return file_.size() / page_bytes_; }
protected:
    pineforge::detail::ScratchStatus do_read_page(
        pineforge::detail::ScratchPageId id, pineforge::detail::ScratchBuffer target) const noexcept override {
        using pineforge::detail::ScratchStatus;
        if (id >= page_count()) return ScratchStatus::not_found;
        return file_.transfer(id * page_bytes_, target.data, target.size, false)
            ? ScratchStatus::ok : ScratchStatus::io_error;
    }
    pineforge::detail::ScratchStatus do_write_page(
        pineforge::detail::ScratchPageId id, pineforge::detail::ScratchBytes source) noexcept override {
        using pineforge::detail::ScratchStatus;
        if (id >= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / page_bytes_)
            return ScratchStatus::io_error;
        const auto end = (id + 1) * page_bytes_;
        if (end > file_.size() && file_.grow(end - file_.size()) < end)
            return ScratchStatus::io_error;
        return file_.transfer(id * page_bytes_, const_cast<std::byte*>(source.data), source.size, true)
            ? ScratchStatus::ok : ScratchStatus::io_error;
    }
    pineforge::detail::ScratchStatus do_sync(
        pineforge::detail::ScratchDurability durability) noexcept override {
        using namespace pineforge::detail;
        return durability == ScratchDurability::buffered || file_.sync()
            ? ScratchStatus::ok : ScratchStatus::io_error;
    }
};

struct StoreFixture {
    using Store = pineforge::detail::o1::Store;
    static constexpr std::uint32_t page_bytes = 4096;
    static constexpr std::uint32_t chunk_bytes = 1024;
    static_assert(chunk_bytes <= pineforge::detail::o1::kMaxChunkBytes, "fixture chunk Shape cap");
    pineforge::detail::o1::Shape shape;
    FileBacking backing;
    std::unique_ptr<Store> store;

    explicit StoreFixture(std::uint32_t pages = 8) : backing(page_bytes) {
        shape.page_bytes = page_bytes;
        shape.resident_pages = pages;
        shape.chunk_bytes = chunk_bytes;
        store = Store::open(shape, backing);
    }
};

} // namespace array_test
