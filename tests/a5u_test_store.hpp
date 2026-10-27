#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_backing.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace a5u_test {

inline void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

inline constexpr std::uint32_t kPageBytes = 4096;
inline constexpr std::uint32_t kResidentPages = 8;
inline constexpr std::uint32_t kChunkBytes = 1024;
// Store's Slot and buckets consume 32 bytes per resident page at this
// power-of-two page count. Round metadata up; allow six work pages and two
// pages for Core/alignment, then the object layer's chunk + 16-byte header.
inline constexpr std::uint64_t kMetadataPages =
    (32 * kResidentPages + kPageBytes - 1) / kPageBytes;
inline constexpr std::uint64_t kStoreBound =
    (kResidentPages + 6 + kMetadataPages + 2) * kPageBytes + kChunkBytes + 16;
// Two further pages cover the fixed graph facade, transaction handles and
// transfer records/buffers. Neither bound depends on the population.
inline constexpr std::uint64_t kResidentBound = kStoreBound + 2 * kPageBytes;

inline pineforge::detail::o1::Shape shape() {
    pineforge::detail::o1::Shape value;
    value.page_bytes = kPageBytes;
    value.resident_pages = kResidentPages;
    value.chunk_bytes = kChunkBytes;
    return value;
}

// Disclosed disk backing with no userspace cache or page directory. The
// supervisor may set PINEFORGE_A5U_SCRATCH on the remote host. By default,
// scratch is beside the source worktree (the lane directory), never /tmp.
// Files are retained and their literal names are printed for the handoff.
class FileBacking final : public pineforge::detail::ScratchBacking {
public:
    FileBacking() : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, kPageBytes) {
        std::string directory;
        if (const char* configured = std::getenv("PINEFORGE_A5U_SCRATCH")) {
            directory = configured;
            require(!directory.empty(), "PINEFORGE_A5U_SCRATCH is empty");
        } else {
            const std::string source = __FILE__;
            const auto tests = source.rfind("/tests/");
            require(tests != std::string::npos, "fixture needs an absolute source filename");
            const auto parent = source.rfind('/', tests - 1);
            require(parent != std::string::npos, "fixture needs a source parent directory");
            directory = source.substr(0, parent) + "/scratch";
        }
        require(::mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST,
                "create UDT lane scratch directory");
        path_ = directory + "/a5u-store-XXXXXX";
        descriptor_ = ::mkstemp(path_.data());
        require(descriptor_ >= 0, "open UDT lane disk backing");
        std::printf("A5U backing=%s\n", path_.c_str());
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
        do { result = ::fsync(descriptor_); } while (result < 0 && errno == EINTR);
        return result == 0 ? Status::ok : Status::io_error;
    }
    int descriptor_ = -1;
    std::uint64_t pages_ = 0;
    std::string path_;
};

struct StoreFixture {
    FileBacking backing;
    std::unique_ptr<pineforge::detail::o1::Store> store =
        pineforge::detail::o1::Store::open(shape(), backing);
};

} // namespace a5u_test
