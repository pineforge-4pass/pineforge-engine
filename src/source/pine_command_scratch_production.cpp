#include "pine_command_scratch_production.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <unistd.h>

namespace pineforge::source::detail {
namespace {

using namespace pineforge::detail;

thread_local ScratchIoObserver command_scratch_observer{};

ScratchStatus io_status(int error) noexcept {
    // Exhausting the host's backing provision is a programming error in this
    // contract, never a scratch-capacity run refusal.
    return error == ENOSPC || error == EDQUOT || error == EFBIG
        ? ScratchStatus::programming_error : ScratchStatus::io_error;
}

class CommandScratchFile final : public ScratchBacking {
public:
    CommandScratchFile(std::size_t size, ScratchIoObserver observer)
        : ScratchBacking(ScratchBackingKind::disk, size, observer) {
        const char* selected = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        std::string path = selected && *selected ? selected : "/tmp";
        if (path.back() != '/') path += '/';
        path += "pineforge-command-XXXXXX";
        const int opened = ::mkstemp(path.data());
        if (opened < 0)
            throw std::system_error(errno, std::generic_category(), "command scratch create");

        int result;
        do {
            result = ::unlink(path.c_str());
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw std::system_error(error, std::generic_category(), "command scratch unlink");
        }
        do {
            result = ::fcntl(opened, F_SETFD, FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw std::system_error(error, std::generic_category(), "command scratch cloexec");
        }
        fd_ = opened;
    }

    ~CommandScratchFile() noexcept override {
        // Retrying close after EINTR could close a descriptor reused by another
        // thread on hosts where the first close already released it.
        ::close(fd_);
    }

    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        off_t offset, end;
        if (!page_offsets(id, offset, end)) return ScratchStatus::programming_error;
        std::size_t transferred = 0;
        while (transferred < destination.size) {
            const auto chunk = std::min(destination.size - transferred,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t count;
            do {
                count = ::pread(fd_, destination.data + transferred, chunk,
                                offset + static_cast<off_t>(transferred));
            } while (count < 0 && errno == EINTR);
            if (count < 0) return io_status(errno);
            if (static_cast<std::size_t>(count) != chunk) return ScratchStatus::io_error;
            transferred += static_cast<std::size_t>(count);
        }
        return ScratchStatus::ok;
    }

    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        off_t offset, end;
        if (!page_offsets(id, offset, end)) return ScratchStatus::programming_error;
        if (id >= extent_) {
            int result;
            do {
                result = ::ftruncate(fd_, end);
            } while (result < 0 && errno == EINTR);
            if (result < 0) return io_status(errno);
            // ftruncate exposes zero-filled holes. Keep the real new extent
            // even if the following positional write fails or is short.
            extent_ = id + 1;
        }
        std::size_t transferred = 0;
        while (transferred < source.size) {
            const auto chunk = std::min(source.size - transferred,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t count;
            do {
                count = ::pwrite(fd_, source.data + transferred, chunk,
                                 offset + static_cast<off_t>(transferred));
            } while (count < 0 && errno == EINTR);
            if (count < 0) return io_status(errno);
            if (static_cast<std::size_t>(count) != chunk) return ScratchStatus::io_error;
            transferred += static_cast<std::size_t>(count);
        }
        return ScratchStatus::ok;
    }

    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        if (durability == ScratchDurability::buffered) return ScratchStatus::ok;
        int result;
        do {
            result = durability == ScratchDurability::data ? ::fdatasync(fd_) : ::fsync(fd_);
        } while (result < 0 && errno == EINTR);
        return result == 0 ? ScratchStatus::ok : io_status(errno);
    }

private:
    bool page_offsets(ScratchPageId id, off_t& offset, off_t& end) const noexcept {
        const auto limit = static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max());
        const auto size = static_cast<std::uintmax_t>(page_size());
        if (id >= limit / size) return false;
        offset = static_cast<off_t>(static_cast<std::uintmax_t>(id) * size);
        end = offset + static_cast<off_t>(size);
        return true;
    }

    int fd_ = -1;
    std::uint64_t extent_ = 0;
};

} // namespace

pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept {
    const auto previous = command_scratch_observer;
    command_scratch_observer = observer;
    return previous;
}

std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size) {
    if (page_size == 0) throw std::invalid_argument("command scratch page size is zero");
    errno = 0;
    const long platform_page = ::sysconf(_SC_PAGESIZE);
    if (platform_page <= 0)
        throw std::system_error(errno ? errno : EINVAL, std::generic_category(),
                                "command scratch platform page size");
    if (page_size % static_cast<std::size_t>(platform_page) != 0
        || static_cast<std::uintmax_t>(page_size)
            > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("command scratch page size is not a representable page multiple");
    return std::make_unique<CommandScratchFile>(page_size, command_scratch_observer);
}

} // namespace pineforge::source::detail
