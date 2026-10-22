#include "command_scratch_production.hpp"

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <atomic>
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
#include <utility>

namespace pineforge::detail {

struct CommandScratchByteBudget {
    explicit CommandScratchByteBudget(std::uint64_t bytes) noexcept : limit(bytes) {}

    bool reserve(std::uint64_t bytes) noexcept {
        if (limit == 0) return true;
        auto used = live_bytes.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > limit - used) return false;
            if (live_bytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
                return true;
        }
    }

    void release(std::uint64_t bytes) noexcept {
        if (limit != 0) live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }

    const std::uint64_t limit;
    std::atomic<std::uint64_t> live_bytes{0};
};

namespace {

thread_local ScratchIoObserver command_scratch_observer{};
thread_local CommandScratchByteCap command_scratch_byte_cap;
thread_local std::weak_ptr<CommandScratchByteBudget> command_scratch_environment_budget;

std::uint64_t environment_byte_cap() {
    const char* selected = std::getenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES");
    if (!selected) return 0;
    std::uint64_t bytes = 0;
    for (const char* digit = selected; *digit; ++digit) {
        if (*digit < '0' || *digit > '9')
            throw coded<std::invalid_argument>(RunFailureCode::scratch_configuration_invalid,
                {{"reason", "nondecimal"}, {"stage", "command_scratch_byte_cap"}},
                "command scratch byte cap is not positive decimal uint64_t");
        const auto value = static_cast<std::uint64_t>(*digit - '0');
        if (bytes > (std::numeric_limits<std::uint64_t>::max() - value) / 10)
            throw coded<std::invalid_argument>(RunFailureCode::scratch_configuration_invalid,
                {{"reason", "overflow"}, {"stage", "command_scratch_byte_cap"}},
                "command scratch byte cap is not positive decimal uint64_t");
        bytes = bytes * 10 + value;
    }
    if (bytes == 0)
        throw coded<std::invalid_argument>(RunFailureCode::scratch_configuration_invalid,
            {{"reason", "nonpositive"}, {"stage", "command_scratch_byte_cap"}},
            "command scratch byte cap is not positive decimal uint64_t");
    return bytes;
}

ScratchStatus io_status(int error) noexcept {
    // Exhausting the host's backing provision is a programming error in this
    // contract, never a scratch-capacity run refusal.
    return error == ENOSPC || error == EDQUOT || error == EFBIG
        ? ScratchStatus::programming_error : ScratchStatus::io_error;
}

// An attached arena is the host's explicit backing selection. Borrow one
// region for this store; never create a second file or resize the inherited fd.
class CommandScratchArena final : public ScratchBacking {
public:
    CommandScratchArena(std::size_t size, ScratchIoObserver observer,
                        CommandScratchByteCap budget, ScratchArenaBackend backend)
        : ScratchBacking(backend == ScratchArenaBackend::sealed_memfd
              ? ScratchBackingKind::memfd : ScratchBackingKind::disk, size, observer),
          budget_(std::move(budget)) {
        file_.open("pineforge-command");
    }
    ~CommandScratchArena() noexcept override {
        if (budget_) budget_->release(extent_ * static_cast<std::uint64_t>(page_size()));
    }
    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        return perform([&] {
            file_.read(id * page_size(), destination.data, destination.size);
        });
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        const auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (id >= limit / page_size()) return ScratchStatus::programming_error;
        if (id >= extent_) {
            const auto end = (id + 1) * page_size();
            const auto growth = end - extent_ * page_size();
            if (budget_ && !budget_->reserve(growth)) return ScratchStatus::io_error;
            const auto status = perform([&] { file_.ensure_size(end); });
            if (status != ScratchStatus::ok) {
                if (budget_) budget_->release(growth);
                return status;
            }
            extent_ = id + 1;
        }
        return perform([&] { file_.write(id * page_size(), source.data, source.size); });
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        // Arena mappings provide positional scratch, not a durability hook.
        return durability == ScratchDurability::buffered
            ? ScratchStatus::ok : ScratchStatus::programming_error;
    }

private:
    template<class Operation> static ScratchStatus perform(Operation operation) noexcept {
        try {
            operation();
            return ScratchStatus::ok;
        } catch (const std::system_error& error) {
            return io_status(error.code().value());
        } catch (...) {
            return ScratchStatus::programming_error;
        }
    }
    ScratchFile file_{ScratchFile::IoRaiser::auxiliary};
    std::uint64_t extent_ = 0;
    CommandScratchByteCap budget_;
};

class CommandScratchFile final : public ScratchBacking {
public:
    CommandScratchFile(std::size_t size, ScratchIoObserver observer, CommandScratchByteCap budget)
        : ScratchBacking(ScratchBackingKind::disk, size, observer), budget_(std::move(budget)) {
        const char* selected = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        std::string path = selected && *selected ? selected : "/tmp";
        if (path.back() != '/') path += '/';
        path += "pineforge-command-XXXXXX";
        const int opened = ::mkstemp(path.data());
        if (opened < 0)
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(errno, std::generic_category(), "command scratch create"));

        int result;
        do {
            result = ::unlink(path.c_str());
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), "command scratch unlink"));
        }
        do {
            result = ::fcntl(opened, F_SETFD, FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            ::close(opened);
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), "command scratch cloexec"));
        }
        fd_ = opened;
    }

    ~CommandScratchFile() noexcept override {
        // Retrying close after EINTR could close a descriptor reused by another
        // thread on hosts where the first close already released it.
        ::close(fd_);
        if (budget_) budget_->release(extent_ * static_cast<std::uint64_t>(page_size()));
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
            const auto growth = static_cast<std::uint64_t>(end)
                - extent_ * static_cast<std::uint64_t>(page_size());
            if (budget_ && !budget_->reserve(growth)) return ScratchStatus::io_error;
            int result;
            do {
                result = ::ftruncate(fd_, end);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                const int error = errno;
                if (budget_) budget_->release(growth);
                return io_status(error);
            }
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
    CommandScratchByteCap budget_;
};

} // namespace

pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept {
    const auto previous = command_scratch_observer;
    command_scratch_observer = observer;
    return previous;
}

CommandScratchByteCap set_command_scratch_byte_cap(std::uint64_t bytes) {
    return set_command_scratch_byte_cap(std::make_shared<CommandScratchByteBudget>(bytes));
}

CommandScratchByteCap set_command_scratch_byte_cap(CommandScratchByteCap registration) noexcept {
    command_scratch_byte_cap.swap(registration);
    return registration;
}

std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size) {
    if (page_size == 0)
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                           "command scratch page size is zero");
    if (static_cast<std::uintmax_t>(page_size)
            > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
        throw coded<std::invalid_argument>(RunFailureCode::scratch_capacity_exceeded,
            {{"resource", "layout"}}, "command scratch page size is not representable in off_t");
    const auto default_cap = environment_byte_cap();
    auto budget = command_scratch_byte_cap;
    if (!budget && default_cap != 0) {
        budget = command_scratch_environment_budget.lock();
        if (!budget || budget->limit != default_cap) {
            budget = std::make_shared<CommandScratchByteBudget>(default_cap);
            command_scratch_environment_budget = budget;
        }
    }
    if (const auto* arena = ScratchArena::attached())
        return std::make_unique<CommandScratchArena>(
            page_size, command_scratch_observer, std::move(budget), arena->stats().backend);
    return std::make_unique<CommandScratchFile>(page_size, command_scratch_observer, std::move(budget));
}

} // namespace pineforge::detail
