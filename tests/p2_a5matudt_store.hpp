#pragma once

#include <pineforge/detail/matrix_store.hpp>
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

// INTERIM dependency binding, not a second Store implementation. The local
// scratch copy is the owning store lane's source, with its two pre-frozen2
// ScratchStore type references renamed CommandScratchStore. A combined tree
// links the owning lane's Store::open instead. Never stage scratch-s4e/.
#if __has_include("../scratch-s4e/INTERIM-o1_store.cpp")
#include "../scratch-s4e/INTERIM-o1_store.cpp"
#endif

namespace p2_a5 {
inline void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

// A disclosed, disk-file ScratchBacking. It owns no cache/directory. All test
// scratch stays under the source worktree's scratch-s4e/; no system temp path.
// Files are retained for the supervisor, never unlinked by this lane.
class FileBacking final : public pineforge::detail::ScratchBacking {
public:
    FileBacking() : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, 4096) {
        const std::string source = __FILE__;
        const auto tests = source.rfind("/tests/");
        require(tests != std::string::npos, "compile fixture with an absolute source path");
        const std::string directory = source.substr(0, tests) + "/scratch-s4e";
        require(::mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST, "create lane scratch directory");
        std::string path = directory + "/p2-a5matudt-XXXXXX";
        descriptor_ = ::mkstemp(path.data());
        require(descriptor_ >= 0, "open lane disk backing");
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

class StoreFixture {
public:
    FileBacking backing;
    std::unique_ptr<pineforge::detail::o1::Store> store =
        pineforge::detail::o1::Store::open({}, backing);
    pineforge::detail::matrix_store::Runtime runtime{*store};
    pineforge::detail::matrix_store::ScopedRuntime scope{runtime};
    ~StoreFixture() { runtime.close(); }
};
} // namespace p2_a5
