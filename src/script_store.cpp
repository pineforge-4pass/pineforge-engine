#include <pineforge/detail/script_store.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/checked_settings.hpp>
#include <pineforge/map_capacity.hpp>
#include <pineforge/ta.hpp>

#include <cerrno>
#include <limits>
#include <new>
#include <optional>
#include <unistd.h>

namespace pineforge::detail {
namespace {

thread_local std::uint64_t map_pair_default = 0;
thread_local MapBarContext map_bar_context{};

// The existing scratch file owns medium selection, I/O charging and disk
// cache policy. This adapter adds only a page extent, not a second page cache
// or a logical capacity policy. Explicit run owners supply their own backing.
class ScriptScratchBacking final : public ScratchBacking {
public:
    ScriptScratchBacking() : ScratchBacking(selected_kind(), 4096) {
        file_.bound_cache();
        file_.open("pineforge-script-state");
    }
    std::uint64_t page_count() const noexcept override { return extent_; }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return failure_ ? &*failure_ : nullptr;
    }
protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer output) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        return perform([&] { file_.read(id * page_size(), output.data, output.size); });
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes input) noexcept override {
        if (id >= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / page_size())
            return ScratchStatus::io_error;
        const auto result = perform([&] {
            file_.write(id * page_size(), input.data, input.size);
        });
        if (result == ScratchStatus::ok && id >= extent_) extent_ = id + 1;
        return result;
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        if (durability == ScratchDurability::buffered) return ScratchStatus::ok;
        // An explicitly attached arena has no private disk descriptor.
        if (file_.arena_backed()) return ScratchStatus::programming_error;
        int result;
        do { result = ::fsync(file_.descriptor()); } while (result != 0 && errno == EINTR);
        return result == 0 ? ScratchStatus::ok : ScratchStatus::io_error;
    }
private:
    static ScratchBackingKind selected_kind() {
        const auto* arena = ScratchArena::attached();
        return arena && arena->stats().backend == ScratchArenaBackend::sealed_memfd
            ? ScratchBackingKind::memfd : ScratchBackingKind::disk;
    }
    template <class Function>
    ScratchStatus perform(Function function) const noexcept {
        failure_.reset();
        try {
            function();
            return ScratchStatus::ok;
        } catch (const ReportScratchIoError& error) {
            failure_ = error;
            return ScratchStatus::io_error;
        } catch (const std::bad_alloc&) {
            failure_ = scratch_out_of_memory_failure();
            return ScratchStatus::io_error;
        } catch (...) {
            return ScratchStatus::io_error;
        }
    }
    mutable ScratchFile file_;
    mutable std::optional<ReportScratchIoError> failure_;
    std::uint64_t extent_ = 0;
};

} // namespace

std::unique_ptr<ScratchBacking> make_script_scratch_backing() {
    return std::make_unique<ScriptScratchBacking>();
}

std::uint64_t map_pair_limit() noexcept { return map_pair_default; }

std::uint64_t exchange_map_pair_limit(std::uint64_t maximum) noexcept {
    const auto previous = map_pair_default;
    map_pair_default = maximum;
    return previous;
}

int set_map_pair_limit_checked(const std::int64_t* maximum,
                               char* error, std::size_t capacity) noexcept {
    return checked_settings::boundary(error, capacity, [&] {
        checked_settings::require(!maximum || *maximum >= 0,
                                  "max map pairs must not be negative");
        map_pair_default = maximum ? static_cast<std::uint64_t>(*maximum) : 0;
    });
}

[[noreturn]] void map_capacity_error(std::uint64_t maximum, std::uint64_t current) {
    std::int64_t bar = map_bar_context.native_bar ? *map_bar_context.native_bar : 0;
    if (map_bar_context.use_ambient_context) {
        const auto& context = ta::bar_context();
        if (context.installed) bar = context.bar_index;
    }
    throw MapCapacityError(maximum, current, bar);
}

MapBarContext push_map_bar_context(const int* native_bar) noexcept {
    const auto previous = map_bar_context;
    if (native_bar) {
        map_bar_context.native_bar = native_bar;
        map_bar_context.use_ambient_context = false;
    } else {
        map_bar_context.use_ambient_context = true;
    }
    return previous;
}

void restore_map_bar_context(MapBarContext previous) noexcept {
    map_bar_context = previous;
}

} // namespace pineforge::detail

namespace pineforge {

MapCapacityError::MapCapacityError(std::uint64_t maximum, std::uint64_t current,
                                   std::int64_t bar)
    : coded<std::runtime_error>(
          RunFailureCode::pine_map_capacity_exceeded,
          {{"code", code}, {"maxSize", maximum}, {"currSize", current}, {"bar_index", bar}},
          "Error on bar " + std::to_string(bar) + ": A map cannot have more than " +
              std::to_string(maximum) + " key-value pairs. The current size is " +
              std::to_string(current) + "."),
      maxSize(maximum), currSize(current), bar_index(bar) {}

} // namespace pineforge
