#pragma once

#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdint>

namespace pineforge {

// A configured map's first refused DISTINCT insertion. Counts are owned by
// the map; replacing a key never reaches this exception.
class MapCapacityError : public coded<std::runtime_error> {
public:
    MapCapacityError(std::uint64_t maximum, std::uint64_t current, std::int64_t bar);
    static constexpr const char* code = "RE10070";
    const std::uint64_t maxSize;
    const std::uint64_t currSize;
    const std::int64_t bar_index;
};

namespace detail {

// The thread's creation policy. A host captures it before its constructor
// creates maps and reinstalls it around script preparation and callbacks.
// Every map stores its own copy. Zero means no pair limit.
std::uint64_t map_pair_limit() noexcept;
std::uint64_t exchange_map_pair_limit(std::uint64_t maximum) noexcept;
int set_map_pair_limit_checked(const std::int64_t* maximum,
                               char* error, std::size_t capacity) noexcept;
[[noreturn]] void map_capacity_error(std::uint64_t maximum, std::uint64_t current);

// Borrowed call context, never durable state. Native callbacks use their own
// engine's live bar index; a Pine evaluation selects its installed TA context.
// Scope restoration prevents a nested engine from borrowing its parent's bar.
struct MapBarContext {
    const int* native_bar = nullptr;
    bool use_ambient_context = true;
};

MapBarContext push_map_bar_context(const int* native_bar) noexcept;
void restore_map_bar_context(MapBarContext previous) noexcept;

class MapBarContextScope {
public:
    // nullptr selects Pine context while retaining the native fallback.
    explicit MapBarContextScope(const int* native_bar = nullptr) noexcept
        : previous_(push_map_bar_context(native_bar)) {}
    MapBarContextScope(const MapBarContextScope&) = delete;
    MapBarContextScope& operator=(const MapBarContextScope&) = delete;
    ~MapBarContextScope() { restore_map_bar_context(previous_); }

private:
    MapBarContext previous_;
};

class MapPairLimitScope {
public:
    explicit MapPairLimitScope(std::uint64_t maximum) noexcept
        : previous_(exchange_map_pair_limit(maximum)) {}
    MapPairLimitScope(const MapPairLimitScope&) = delete;
    MapPairLimitScope& operator=(const MapPairLimitScope&) = delete;
    ~MapPairLimitScope() { exchange_map_pair_limit(previous_); }

private:
    std::uint64_t previous_;
};

} // namespace detail
} // namespace pineforge
