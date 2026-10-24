// Test setup only. The array consumer never names a backing implementation.
// Infer Store::open's borrowed backing argument, so these rows also compile
// when a5s replaces the interim standalone Store with its ScratchStore layer.
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
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace array_test {

inline void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

class SpillFile {
    int fd_ = -1;
    std::uint64_t extent_ = 0;
public:
    SpillFile() {
        // Explicit disk backing on every platform. Leave the file for the
        // supervisor under this worktree, never in the system temp directory.
        std::string path = __FILE__;
        const auto tests = path.rfind("/tests/");
        path = tests == std::string::npos ? "scratch-s4e" : path.substr(0, tests) + "/scratch-s4e";
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
            throw std::runtime_error("create array test scratch directory");
        path += "/array-test-XXXXXX";
        std::vector<char> name(path.begin(), path.end());
        name.push_back('\0');
        fd_ = ::mkstemp(name.data());
        require(fd_ >= 0, "create array test backing");
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

template<class Signature> struct OpenBacking;
template<class Result, class Shape, class Backing>
struct OpenBacking<Result (*)(const Shape&, Backing&)> { using type = Backing; };

template<class Base, bool = std::is_base_of_v<pineforge::detail::ScratchBacking, Base>>
class BorrowedBacking;

template<class Base>
class BorrowedBacking<Base, false> final : public Base {
    SpillFile file_;
public:
    explicit BorrowedBacking(std::size_t) {}
    bool read_at(std::uint64_t offset, void* target, std::size_t count) override {
        return file_.transfer(offset, target, count, false);
    }
    bool write_at(std::uint64_t offset, const void* source, std::size_t count) override {
        return file_.transfer(offset, const_cast<void*>(source), count, true);
    }
    std::uint64_t grow(std::uint64_t count) override { return file_.grow(count); }
};

template<class Base>
class BorrowedBacking<Base, true> final : public Base {
    SpillFile file_;
    std::size_t page_bytes_;
public:
    explicit BorrowedBacking(std::size_t page)
        : Base(pineforge::detail::ScratchBackingKind::disk, page), page_bytes_(page) {}
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
    using Backing = typename OpenBacking<decltype(&Store::open)>::type;
    pineforge::detail::o1::Shape shape;
    BorrowedBacking<Backing> backing;
    std::unique_ptr<Store> store;

    explicit StoreFixture(std::uint32_t pages = 8) : backing(4096) {
        shape.page_bytes = 4096;
        shape.resident_pages = pages;
        store = Store::open(shape, backing);
    }
};

} // namespace array_test
