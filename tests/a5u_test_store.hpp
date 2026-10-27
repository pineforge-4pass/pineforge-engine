// Graph test store: o1::Store over a disk backing, plus the derived
// ceilings the rows assert. No constant here depends on a test population.
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
#include <sys/types.h>
#include <unistd.h>

namespace a5u_test {

inline void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

inline constexpr std::uint32_t kPageBytes = 4096;
inline constexpr std::uint32_t kChunkBytes = 1024;

// Resident ceiling of one Store, derived from its Shape only (the same
// derivation as test_o1_store.cpp ceiling_for). ScratchStore's workspace is
// scratch_store_impl.hpp Core::required_bytes:
//   round_up(sizeof(Core), 16) + round_up(R * sizeof(Slot), 16)
//   + round_up(bucket_count(R) * 4, 64) + (R + kWorkPages) * page_bytes,
// kWorkPages = 6. Slot is 24 bytes and bucket_count(R) = 2R for a power-of-two
// R, so slots + buckets are 32 * R bytes, rounded up to whole pages here.
// sizeof(Core) is charged two whole pages. The object layer adds its 16-byte
// chunk header + one chunk_bytes buffer.
inline constexpr std::uint64_t store_bound(std::uint32_t resident_pages) {
    return (resident_pages + UINT64_C(6) +
            (UINT64_C(32) * resident_pages + kPageBytes - 1) / kPageBytes + 2) *
               kPageBytes +
           16 + kChunkBytes;
}

// Heap a graph operation may hold beyond the Store workspace. o1_store.hpp:
// one Tx-handle allocation per begin(), none for any other successful call.
// The bounded graph/growth windows use at most one caller scope and one
// graph-operation scope: two live handles, each charged one whole page.
inline constexpr std::uint64_t kOperationHeapBound = 2 * kPageBytes;

inline pineforge::detail::o1::Shape shape(std::uint32_t resident_pages) {
    pineforge::detail::o1::Shape value;
    value.page_bytes = kPageBytes;
    value.resident_pages = resident_pages;
    value.chunk_bytes = kChunkBytes;
    return value;
}

// Observe actual calls into the merged Store. This proxy neither emulates
// storage nor supplies resident statistics; Tx handles and Stats come from
// the real Store unchanged. Counts diagnose accidental population scans in
// per-bar operations independently of the runtime's own implementation.
class CountingStore final : public pineforge::detail::o1::Store {
    using Id = pineforge::detail::o1::Id;
    using Tx = pineforge::detail::o1::Tx;
    pineforge::detail::o1::Store& inner_;

public:
    explicit CountingStore(pineforge::detail::o1::Store& inner) : inner_(inner) {}
    mutable std::uint64_t data_calls = 0;
    std::uint64_t begin_calls = 0;
    std::uint64_t mutation_calls = 0;
    // Optional fixed-state observer used to retain a real collector mark for
    // replay in the stale-mark row. No storage or transaction is emulated.
    void* write_context = nullptr;
    void (*written)(void*, Id, std::uint64_t, const void*, std::size_t) = nullptr;
    Id create(std::uint64_t bytes, std::uint64_t align) override {
        ++data_calls;
        ++mutation_calls;
        return inner_.create(bytes, align);
    }
    void release(Id id) override {
        ++data_calls;
        ++mutation_calls;
        inner_.release(id);
    }
    bool alive(Id id) const override {
        ++data_calls;
        return inner_.alive(id);
    }
    std::uint64_t size(Id id) const override {
        ++data_calls;
        return inner_.size(id);
    }
    bool resize(Id id, std::uint64_t bytes) override {
        ++data_calls;
        ++mutation_calls;
        return inner_.resize(id, bytes);
    }
    std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t bytes) const override {
        ++data_calls;
        return inner_.read(id, off, dst, bytes);
    }
    std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t bytes) override {
        ++data_calls;
        ++mutation_calls;
        const auto count = inner_.write(id, off, src, bytes);
        if (written != nullptr) written(write_context, id, off, src, count);
        return count;
    }
    std::unique_ptr<Tx> begin() override {
        ++begin_calls;
        return inner_.begin();
    }
    pineforge::detail::o1::Stats stats() const override { return inner_.stats(); }
};

// Disk backing created in the test's working directory and unlinked at once
// (the tests/command_scratch_arena_fixture.hpp idiom). It keeps no userspace
// page cache or page directory, so spilled graph bytes are never process heap.
class FileBacking final : public pineforge::detail::ScratchBacking {
public:
    FileBacking() : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, kPageBytes) {
        char path[] = "./a5u-store-XXXXXX";
        descriptor_ = ::mkstemp(path);
        require(descriptor_ >= 0, "create the graph disk backing in the test directory");
        if (::unlink(path) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            require(false, "unlink the graph disk backing");
        }
    }
    ~FileBacking() override {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    std::uint64_t page_count() const noexcept override { return pages_; }

private:
    using Status = pineforge::detail::ScratchStatus;
    Status do_read_page(pineforge::detail::ScratchPageId page,
                        pineforge::detail::ScratchBuffer destination) const noexcept override {
        if (page >= pages_) return Status::not_found;
        std::size_t done = 0;
        while (done < destination.size) {
            const auto count = ::pread(descriptor_, destination.data + done,
                                       destination.size - done,
                                       static_cast<off_t>(page * page_size() + done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return Status::io_error;
            done += static_cast<std::size_t>(count);
        }
        return Status::ok;
    }
    Status do_write_page(pineforge::detail::ScratchPageId page,
                         pineforge::detail::ScratchBytes source) noexcept override {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (page >= maximum / page_size()) return Status::io_error;
        std::size_t done = 0;
        while (done < source.size) {
            const auto count = ::pwrite(descriptor_, source.data + done, source.size - done,
                                        static_cast<off_t>(page * page_size() + done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return Status::io_error;
            done += static_cast<std::size_t>(count);
        }
        pages_ = std::max(pages_, page + 1);
        return Status::ok;
    }
    Status do_sync(pineforge::detail::ScratchDurability durability) noexcept override {
        if (durability == pineforge::detail::ScratchDurability::buffered) return Status::ok;
        int result;
        do {
            result = ::fsync(descriptor_);
        } while (result < 0 && errno == EINTR);
        return result == 0 ? Status::ok : Status::io_error;
    }
    int descriptor_ = -1;
    std::uint64_t pages_ = 0;
};

struct StoreFixture {
    explicit StoreFixture(std::uint32_t resident_pages = 8)
        : store(pineforge::detail::o1::Store::open(shape(resident_pages), backing)) {}
    FileBacking backing;
    std::unique_ptr<pineforge::detail::o1::Store> store;
};

} // namespace a5u_test
