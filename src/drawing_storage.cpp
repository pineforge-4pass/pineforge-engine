#include <pineforge/detail/drawing_records.hpp>
#include <pineforge/detail/scratch_directory.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <unistd.h>

namespace pineforge::detail {
namespace {

// The medium of one drawing family's records: an anonymous, close-on-exec
// disk file in the common scratch directory (TMPDIR, else /tmp; /var/tmp only
// after a proven memory filesystem), created the way CommandScratchFile
// creates its file. Pages are positional; a write past the end extends the
// file with a zero-filled hole. The object holds a descriptor and two
// counters, nothing that grows with pages. Buffered sync only: the store
// never asks a drawing family for durability.
class DrawingScratchFile final : public ScratchBacking {
public:
    explicit DrawingScratchFile(std::size_t page_size)
        : ScratchBacking(ScratchBackingKind::disk, page_size) {
        std::string refusal;
        int directory_error = 0;
        std::string path = resolve_scratch_directory(std::string(), refusal, directory_error);
        if (path.empty())
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(directory_error, std::generic_category(),
                    scratch_with_detail("drawing scratch create", refusal.c_str(), directory_error)));
        const auto create_message = scratch_create_message("drawing scratch create", path, refusal);
        if (path.back() != '/') path += '/';
        path += "pineforge-drawing-XXXXXX";
        const int opened = ::mkstemp(path.data());
        if (opened < 0)
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(errno, std::generic_category(), create_message));
        int result;
        do {
            result = ::unlink(path.c_str());
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), "drawing scratch unlink"));
        }
        do {
            result = ::fcntl(opened, F_SETFD, FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), "drawing scratch cloexec"));
        }
        fd_ = opened;
    }
    // Never retried: after EINTR the descriptor may already be released.
    ~DrawingScratchFile() noexcept override { ::close(fd_); }

    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        off_t offset;
        if (!page_offset(id, offset)) return ScratchStatus::programming_error;
        std::size_t done = 0;
        while (done < destination.size) {
            const auto want = std::min(destination.size - done,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t count;
            do {
                count = ::pread(fd_, destination.data + done, want, offset + static_cast<off_t>(done));
            } while (count < 0 && errno == EINTR);
            if (count <= 0) return ScratchStatus::io_error;
            done += static_cast<std::size_t>(count);
        }
        return ScratchStatus::ok;
    }

    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        off_t offset;
        if (!page_offset(id, offset)) return ScratchStatus::programming_error;
        std::size_t done = 0;
        while (done < source.size) {
            const auto want = std::min(source.size - done,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t count;
            do {
                count = ::pwrite(fd_, source.data + done, want, offset + static_cast<off_t>(done));
            } while (count < 0 && errno == EINTR);
            if (count <= 0) return ScratchStatus::io_error;
            done += static_cast<std::size_t>(count);
        }
        extent_ = std::max(extent_, id + 1);
        return ScratchStatus::ok;
    }

    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return durability == ScratchDurability::buffered
            ? ScratchStatus::ok : ScratchStatus::programming_error;
    }

private:
    bool page_offset(ScratchPageId id, off_t& offset) const noexcept {
        const auto limit = static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max());
        const auto size = static_cast<std::uintmax_t>(page_size());
        if (id >= limit / size) return false;
        offset = static_cast<off_t>(static_cast<std::uintmax_t>(id) * size);
        return true;
    }

    int fd_ = -1;
    std::uint64_t extent_ = 0;
};

} // namespace

// One family's workspace: eight resident ScratchStore pages of 4 KiB and the
// object layer's 1 KiB chunk (o1_store.hpp:57-68). Everything here is sized
// by this Shape alone, at open; nothing grows with records afterwards.
std::shared_ptr<DrawingStorage> open_drawing_storage() {
    auto storage = std::make_shared<DrawingStorage>();
    o1::Shape shape;
    shape.resident_pages = 8;
    shape.journal_slots = 4;
    shape.index_slots = 4;
    storage->backing = std::make_unique<DrawingScratchFile>(shape.page_bytes);
    storage->store = o1::Store::open(shape, *storage->backing);
    return storage;
}

} // namespace pineforge::detail
