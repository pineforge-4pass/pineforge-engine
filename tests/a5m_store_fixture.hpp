#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_backing.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace a5m_test {

inline void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

// Disclosed disk backing, with no userspace page directory. The lab may supply
// PF_A5M_SCRATCH_DIR; otherwise scratch is in the source worktree's parent,
// the lane's scratch/ directory. Files are retained for the supervisor.
class FileBacking final : public pineforge::detail::ScratchBacking {
public:
    FileBacking() : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, 4096) {
        const char* supplied = std::getenv("PF_A5M_SCRATCH_DIR");
        std::string directory;
        if (supplied) {
            directory = supplied;
        } else {
            const std::string source = __FILE__;
            const auto tests = source.rfind("/tests/");
            require(tests != std::string::npos, "fixture needs an absolute source path");
            const auto parent = source.rfind('/', tests - 1);
            require(parent != std::string::npos, "fixture needs a source parent directory");
            directory = source.substr(0, parent) + "/scratch";
        }
        require(::mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST,
                "create A5M scratch directory");
        std::string path = directory + "/a5m-XXXXXX";
        descriptor_ = ::mkstemp(path.data());
        require(descriptor_ >= 0, "open A5M disk backing");
    }
    ~FileBacking() override { if (descriptor_ >= 0) ::close(descriptor_); }
    std::uint64_t page_count() const noexcept override { return pages_; }

private:
    using Status = pineforge::detail::ScratchStatus;
    Status do_read_page(pineforge::detail::ScratchPageId page,
                        pineforge::detail::ScratchBuffer out) const noexcept override {
        if (page >= pages_) return Status::not_found;
        std::size_t done = 0;
        while (done < out.size) {
            const auto n = ::pread(descriptor_, out.data + done, out.size - done,
                                  static_cast<off_t>(page * page_size() + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return Status::io_error;
            done += static_cast<std::size_t>(n);
        }
        return Status::ok;
    }
    Status do_write_page(pineforge::detail::ScratchPageId page,
                         pineforge::detail::ScratchBytes in) noexcept override {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (page >= maximum / page_size()) return Status::io_error;
        std::size_t done = 0;
        while (done < in.size) {
            const auto n = ::pwrite(descriptor_, in.data + done, in.size - done,
                                   static_cast<off_t>(page * page_size() + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return Status::io_error;
            done += static_cast<std::size_t>(n);
        }
        pages_ = std::max(pages_, page + 1);
        return Status::ok;
    }
    Status do_sync(pineforge::detail::ScratchDurability durability) noexcept override {
        if (durability == pineforge::detail::ScratchDurability::buffered) return Status::ok;
        int result;
        do { result = ::fsync(descriptor_); } while (result < 0 && errno == EINTR);
        return result == 0 ? Status::ok : Status::io_error;
    }
    int descriptor_ = -1;
    std::uint64_t pages_ = 0;
};

inline pineforge::detail::o1::Shape shape() {
    pineforge::detail::o1::Shape result;
    result.resident_pages = 8;
    return result;
}

// ScratchStore: eight cache pages, six work pages, ceil(32*8/4096)
// metadata pages, two Core/alignment pages, and the chunk/header buffer.
// Two matrix transfer pages and one control page bound fixed matrix overhead.
// This arithmetic does not inspect a measured resident value or population.
inline constexpr std::uint64_t kStoreBound = (8 + 6 + 1 + 2) * UINT64_C(4096) + 1024 + 16;
inline constexpr std::uint64_t kOperationBound = (2 + 1) * UINT64_C(4096);

// A Store seen through a handle counter. begin() hands out a wrapper around the
// real Tx: its constructor raises the LIVE count and the peaks, its destructor
// lowers the count, commit() and rollback() forward, and the inner handle dies
// with the wrapper. Every other call is forwarded unchanged. begins() counts
// begin() calls (a handle made and destroyed still counts); peak_handles() is
// the most handles alive at once since construction; window_peak() is the same
// since the last restart_window(). Store::scope() made on this view calls its
// join and abandon hooks: each forwards to the inner Store and is only counted
// (joined_scopes(), abandoned_scopes()), so an operation that joined is visible.
class CountingStore final : public pineforge::detail::o1::Store {
public:
    using Id = pineforge::detail::o1::Id;

    explicit CountingStore(pineforge::detail::o1::Store& inner) : inner_(inner) {}

    Id create(std::uint64_t bytes, std::uint64_t align) override {
        return inner_.create(bytes, align);
    }
    void release(Id id) override { inner_.release(id); }
    bool alive(Id id) const override { return inner_.alive(id); }
    std::uint64_t size(Id id) const override { return inner_.size(id); }
    bool resize(Id id, std::uint64_t bytes) override { return inner_.resize(id, bytes); }
    std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t n) const override {
        return inner_.read(id, off, dst, n);
    }
    std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t n) override {
        return inner_.write(id, off, src, n);
    }
    std::unique_ptr<pineforge::detail::o1::Tx> begin() override {
        auto handle = inner_.begin(); // a refused begin() counts nothing
        ++begins_;
        return std::unique_ptr<pineforge::detail::o1::Tx>(
            new Counted(*this, std::move(handle)));
    }
    bool join_active_tx() override {
        const bool joined = inner_.join_active_tx();
        if (joined) ++joins_;
        return joined;
    }
    void abandon_joined_tx() noexcept override {
        ++abandons_;
        inner_.abandon_joined_tx();
    }
    // A cleanup handle takes the inner Store's reserved slot, not a caller
    // slot, so it is forwarded without a Counted wrapper.
    std::unique_ptr<pineforge::detail::o1::Tx> begin_cleanup() override {
        return inner_.begin_cleanup();
    }
    void note_cleanup_skipped(pineforge::detail::o1::CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    pineforge::detail::o1::Stats stats() const override { return inner_.stats(); }

    std::uint64_t begins() const noexcept { return begins_; }
    std::uint64_t live_handles() const noexcept { return live_; }
    std::uint64_t peak_handles() const noexcept { return peak_; }
    std::uint64_t window_peak() const noexcept { return window_; }
    void restart_window() noexcept { window_ = live_; }
    std::uint64_t joined_scopes() const noexcept { return joins_; }
    std::uint64_t abandoned_scopes() const noexcept { return abandons_; }

private:
    class Counted final : public pineforge::detail::o1::Tx {
    public:
        Counted(CountingStore& owner, std::unique_ptr<pineforge::detail::o1::Tx> inner)
            : owner_(owner), inner_(std::move(inner)) {
            ++owner_.live_;
            owner_.peak_ = std::max(owner_.peak_, owner_.live_);
            owner_.window_ = std::max(owner_.window_, owner_.live_);
        }
        ~Counted() override { --owner_.live_; }
        void commit() override { inner_->commit(); }
        void rollback() override { inner_->rollback(); }

    private:
        CountingStore& owner_;
        std::unique_ptr<pineforge::detail::o1::Tx> inner_;
    };

    pineforge::detail::o1::Store& inner_;
    std::uint64_t begins_ = 0, live_ = 0, peak_ = 0, window_ = 0;
    std::uint64_t joins_ = 0, abandons_ = 0;
};

struct StoreFixture {
    StoreFixture() = default;
    // A Store of another shape (a small live-object limit, say) over the same backing.
    explicit StoreFixture(const pineforge::detail::o1::Shape& value)
        : store(pineforge::detail::o1::Store::open(value, backing)) {}
    FileBacking backing;
    std::unique_ptr<pineforge::detail::o1::Store> store =
        pineforge::detail::o1::Store::open(shape(), backing);
    // The same Store through the handle counter; a test that bounds the live
    // handle count builds its Runtime on this view.
    CountingStore counted{*store};
};

} // namespace a5m_test
