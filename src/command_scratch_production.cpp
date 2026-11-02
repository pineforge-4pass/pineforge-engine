#include "command_scratch_production.hpp"
#include "command_scratch_cache.hpp"

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {

void CommandScratchPhysicalIoStats::charge(void* context, const ScratchIoCharge& io) noexcept {
    auto& self = *static_cast<CommandScratchPhysicalIoStats*>(context);
    const auto add = [](std::uint64_t& counter, std::uint64_t value) noexcept {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        counter = value > maximum - counter ? maximum : counter + value;
    };
    if (io.operation == ScratchIoOperation::read_page) {
        add(self.reads, 1);
        add(self.read_bytes, io.bytes);
        if (io.status == ScratchStatus::ok) add(self.completed_reads, 1);
    } else if (io.operation == ScratchIoOperation::write_page) {
        add(self.writes, 1);
        add(self.write_bytes, io.bytes);
    } else add(self.syncs, 1);
    if (io.status != ScratchStatus::ok) add(self.failures, 1);
}

bool CommandScratchByteBudget::reserve(std::uint64_t bytes) noexcept {
    if (limit == 0) return true;
    auto used = live_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > limit - used) return false;
        if (live_bytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
            return true;
    }
}

void CommandScratchByteBudget::release(std::uint64_t bytes) noexcept {
    if (limit != 0) live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

namespace {

// floor(value * multiplier / divisor), optionally rounded up, saturated only
// when the RESULT exceeds limit. Split the whole term first; long division
// of the remainder avoids overflowing an intermediate product in C++17.
std::uint64_t scale_bytes(std::uint64_t value, std::uint64_t multiplier,
                          std::uint64_t divisor, std::uint64_t limit, bool ceil) noexcept {
    const auto whole = value / divisor;
    const auto remainder = value % divisor;
    if (whole > limit / multiplier) return limit;
    const auto base = whole * multiplier;
    if (remainder == 0) return base;
    std::uint64_t fraction = 0, rest = 0;
    for (unsigned bit = 64; bit-- != 0;) {
        fraction *= 2;
        if (rest >= divisor - rest) {
            rest -= divisor - rest;
            ++fraction;
        } else {
            rest += rest;
        }
        if ((multiplier >> bit) & 1) {
            if (rest >= divisor - remainder) {
                rest -= divisor - remainder;
                ++fraction;
            } else {
                rest += remainder;
            }
        }
    }
    if (fraction > limit - base) return limit;
    const auto result = base + fraction;
    return ceil && rest != 0 && result < limit ? result + 1 : result;
}

thread_local ScratchIoObserver command_scratch_observer{};
thread_local ScratchIoObserver command_scratch_physical_observer{};
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

} // namespace

std::uint64_t command_scratch_arena_capacity_bytes(std::uint64_t arena_bytes) noexcept {
    const auto bytes = scale_bytes(arena_bytes, kCommandScratchArenaShare.num,
        kCommandScratchArenaShare.den, std::numeric_limits<std::uint64_t>::max(), false);
    return std::max(ScratchArena::kChunkBytes, bytes - bytes % ScratchArena::kChunkBytes);
}

std::uint64_t command_scratch_arena_needed_bytes(std::uint64_t store_extent) noexcept {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto chunks = store_extent / ScratchArena::kChunkBytes
        + (store_extent % ScratchArena::kChunkBytes != 0 ? 1 : 0);
    if (chunks > maximum / ScratchArena::kChunkBytes) return maximum;
    return scale_bytes(chunks * ScratchArena::kChunkBytes, kCommandScratchArenaShare.den,
                       kCommandScratchArenaShare.num, maximum, true);
}

ScratchStatus command_scratch_io_status(int /*error*/) noexcept {
    // Backing I/O faults, including ENOSPC, EDQUOT, EFBIG and EIO, are
    // io_error. programming_error is reserved for caller contract violations.
    return ScratchStatus::io_error;
}

namespace {

// An attachment is the host's explicit medium selection. Arithmetic page
// offsets address one bounded region; no page-sized userspace directory and
// no private fd can appear after construction.
class CommandScratchArena final : public ScratchBacking {
public:
    CommandScratchArena(std::size_t size, ScratchIoObserver observer,
                        CommandScratchByteCap budget, const ScratchArenaStats& stats)
        : ScratchBacking(stats.backend == ScratchArenaBackend::sealed_memfd
              ? ScratchBackingKind::memfd : ScratchBackingKind::disk, size, observer),
          capacity_(command_scratch_arena_capacity_bytes(stats.bytes_supplied)),
          budget_(std::move(budget)) {
        file_.set_store(ScratchStore::command);
        file_.open_bounded("pineforge-command", capacity_);
    }
    ~CommandScratchArena() noexcept override {
        if (budget_) budget_->release(extent_ * static_cast<std::uint64_t>(page_size()));
    }
    std::uint64_t page_count() const noexcept override { return extent_; }
    std::optional<std::uint64_t> arena_capacity_bytes() const noexcept override { return capacity_; }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return failure_ ? &*failure_ : nullptr;
    }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        failure_.reset();
        if (id >= extent_) return ScratchStatus::not_found;
        return perform([&] {
            file_.read(id * page_size(), destination.data, destination.size);
        });
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        failure_.reset();
        const auto size = static_cast<std::uint64_t>(page_size());
        const auto limit = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (id >= limit / size) return ScratchStatus::programming_error;
        const auto end = (id + 1) * size;
        if (id >= extent_) {
            const auto growth = end - extent_ * size;
            // Byte admission precedes ensure_size/claim, just as in disk mode.
            const auto grow = [&] {
                file_.ensure_size(end, command_scratch_arena_needed_bytes(end));
            };
            if (budget_ && !budget_->reserve(growth)) {
                // A simultaneous share crossing must still carry the arena's
                // typed verdict. This bounded call cannot claim any chunks.
                return end > capacity_ ? perform(grow) : ScratchStatus::io_error;
            }
            const auto status = perform(grow);
            if (status != ScratchStatus::ok) {
                if (budget_) budget_->release(growth);
                return status;
            }
            // A following mapping/copy failure may still leave real extent.
            extent_ = id + 1;
        }
        return perform([&] { file_.write(id * size, source.data, source.size); });
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        failure_.reset();
        return durability == ScratchDurability::buffered
            ? ScratchStatus::ok : ScratchStatus::programming_error;
    }

private:
    template<class Operation> ScratchStatus perform(Operation operation) const noexcept {
        try {
            operation();
            return ScratchStatus::ok;
        } catch (const ReportScratchIoError& error) {
            // Copy the validated failure object, not its English or decoded
            // JSON. Both the status and the original arena verdict survive.
            failure_ = error;
            return ScratchStatus::io_error;
        } catch (const std::bad_alloc&) {
            failure_ = scratch_out_of_memory_failure();
            return ScratchStatus::io_error;
        } catch (...) {
            // All backing faults return io_error. Caller contract
            // violations are checked before reaching ScratchFile.
            return ScratchStatus::io_error;
        }
    }
    ScratchFile file_{ScratchFile::IoRaiser::report_sink};
    mutable std::optional<ReportScratchIoError> failure_;
    std::uint64_t extent_ = 0;
    const std::uint64_t capacity_;
    CommandScratchByteCap budget_;
};

class CommandScratchFile final : public ScratchBacking {
public:
    CommandScratchFile(std::size_t size, ScratchIoObserver observer)
        : ScratchBacking(ScratchBackingKind::disk, size, observer) {
        const char* selected = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        std::string refusal;
        int directory_error = 0;
        std::string path = resolve_scratch_directory(selected ? selected : "", refusal,
                                                     directory_error);
        if (path.empty())
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(directory_error, std::generic_category(),
                    scratch_with_detail("command scratch create", refusal.c_str(), directory_error)));
        const auto create_message = scratch_create_message("command scratch create", path, refusal);
        if (path.back() != '/') path += '/';
        path += "pineforge-command-XXXXXX";
        if (mode() == QuotaMode::managed)
            quota_ = std::make_unique<ScratchFileQuota>(QuotaArtifact::table);
        const int opened = ::mkstemp(path.data());
        if (opened < 0) {
            const int error = errno;
            if (quota_) quota_->create_failed();
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), create_message));
        }
        try {
            if (quota_) {
                const int error = quota_->created(opened);
                if (error != 0)
                    throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                        std::system_error(error, std::generic_category(), "command scratch backing"));
            }
        } catch (...) {
            ::close(opened);
            ::unlink(path.c_str());
            throw;
        }

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
        if (quota_) quota_->unlinked();
        do {
            result = ::fcntl(opened, F_SETFD, FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            const int closed = ::close(opened);
            if (closed == 0 && quota_) quota_->closed();
            throw coded<std::system_error>(RunFailureCode::scratch_io_failed, {},
                std::system_error(error, std::generic_category(), "command scratch cloexec"));
        }
        fd_ = opened;
    }

    ~CommandScratchFile() noexcept override {
        // Retrying close after EINTR could close a descriptor reused by another
        // thread on hosts where the first close already released it.
        const int result = ::close(fd_);
        if (result == 0 && quota_) quota_->closed();
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
            if (count < 0) return command_scratch_io_status(errno);
            if (static_cast<std::size_t>(count) != chunk) return ScratchStatus::io_error;
            transferred += static_cast<std::size_t>(count);
        }
        return ScratchStatus::ok;
    }

    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        off_t offset, end;
        if (!page_offsets(id, offset, end)) return ScratchStatus::programming_error;
        // CachedScratchBacking has already checked its local logical byte
        // budget and selected a real spill before reaching this disk hook.
        if (!quota_) return write_disk_page(id, source, offset, end, nullptr);
        try {
            ScratchFileQuota::Growth reservation(*quota_, static_cast<std::uint64_t>(end));
            const auto status = write_disk_page(id, source, offset, end, &reservation);
            if (status == ScratchStatus::ok) reservation.finish();
            return status;
        } catch (...) {
            // The active quota endpoint retains the original typed refusal
            // for the status-only owner. No denied sizing/write syscall ran.
            return ScratchStatus::io_error;
        }
    }

    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        if (durability == ScratchDurability::buffered) return ScratchStatus::ok;
        int result;
        do {
#if defined(__APPLE__)
            // macOS has no fdatasync and its fsync stops short of stable storage: both modes use F_FULLFSYNC, fsync if it fails.
            result = ::fcntl(fd_, F_FULLFSYNC) != -1
                ? 0 : ::fsync(fd_);
#else
            result = durability == ScratchDurability::data ? ::fdatasync(fd_) : ::fsync(fd_);
#endif
        } while (result < 0 && errno == EINTR);
        return result == 0 ? ScratchStatus::ok : command_scratch_io_status(errno);
    }

private:
    ScratchStatus write_disk_page(ScratchPageId id, ScratchBytes source, off_t offset, off_t end,
                                  ScratchFileQuota::Growth* reservation) noexcept {
        if (id >= extent_) {
            int result;
            do {
                result = ::ftruncate(fd_, end);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                const int error = errno;
                return command_scratch_io_status(error);
            }
            // ftruncate exposes zero-filled holes. Keep the real new extent
            // even if the following positional write fails or is short.
            extent_ = id + 1;
            if (reservation) reservation->truncated(static_cast<std::uint64_t>(end));
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
            if (count < 0) return command_scratch_io_status(errno);
            if (static_cast<std::size_t>(count) != chunk) return ScratchStatus::io_error;
            transferred += static_cast<std::size_t>(count);
        }
        return ScratchStatus::ok;
    }

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
    std::unique_ptr<ScratchFileQuota> quota_;
};

} // namespace

pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept {
    const auto previous = command_scratch_observer;
    command_scratch_observer = observer;
    return previous;
}

ScratchIoObserver set_command_scratch_physical_io_observer(ScratchIoObserver observer) noexcept {
    const auto previous = command_scratch_physical_observer;
    command_scratch_physical_observer = observer;
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
    return make_command_scratch_backing(page_size, CachedScratchBacking::kCacheBytes);
}

std::unique_ptr<ScratchBacking> make_command_scratch_backing(
    std::size_t page_size, CacheCapacity capacity) {
    const auto payload = capacity.payload_bytes(page_size);
    return make_command_scratch_backing(page_size, payload, payload);
}

std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size, std::size_t resident_bytes) {
    return make_command_scratch_backing(page_size, resident_bytes, CachedScratchBacking::kCacheBytes);
}

std::unique_ptr<ScratchBacking> make_command_scratch_backing(
    std::size_t page_size, std::size_t resident_bytes, std::size_t cache_bytes) {
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
    // Only the cache decorator owns the logical observer (it charges hits too).
    // The independent diagnostic observer sees inner operations, never hits.
    // Its optional allocation may bypass caching. Disk byte admission still
    // belongs to the decorator; arena admission still belongs to the inner.
    if (const auto* arena = ScratchArena::attached())
        return std::make_unique<CachedScratchBacking>(
            std::make_unique<CommandScratchArena>(
                page_size, command_scratch_physical_observer, std::move(budget), arena->stats()),
            command_scratch_observer, 0, CommandScratchByteCap{}, cache_bytes);
    return std::make_unique<CachedScratchBacking>(
        std::make_unique<CommandScratchFile>(page_size, command_scratch_physical_observer),
        command_scratch_observer, resident_bytes, std::move(budget), cache_bytes);
}

} // namespace pineforge::detail
