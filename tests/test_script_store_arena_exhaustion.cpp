#include "command_scratch_arena_fixture.hpp"

#include <pineforge/array.hpp>
#include <pineforge/detail/script_store.hpp>
#include <pineforge/map.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/script_string.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using command_arena_test::ArenaAttachment;
using command_arena_test::require;
using pineforge::detail::MapStoreHolder;
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchArenaStats;
using pineforge::detail::ScratchBackingKind;
using pineforge::detail::o1::Store;

// Independent fixture sizes: one MiB is the smallest attachable arena.
constexpr std::uint64_t kArenaBytes = 1024 * 1024;
constexpr std::size_t kPayloadBytes = 4096;

std::shared_ptr<MapStoreHolder> script_store() {
    return std::make_shared<MapStoreHolder>(
        pineforge::detail::make_script_scratch_backing());
}

struct ObjectGrowth {
    Store& store;
    pineforge::detail::o1::Id last = 0;

    explicit ObjectGrowth(const std::shared_ptr<MapStoreHolder>& holder)
        : store(*holder->store) {}

    void grow(std::uint64_t, const std::string& payload) {
        last = store.create(payload.size(), 1);
        require(store.write(last, 0, payload.data(), payload.size()) == payload.size(),
                "object write transfers its complete payload");
    }
    void verify(std::uint64_t, const std::string& payload) const {
        std::array<char, kPayloadBytes> bytes{};
        require(store.read(last, 0, bytes.data(), bytes.size()) == bytes.size()
                    && std::memcmp(bytes.data(), payload.data(), bytes.size()) == 0,
                "disk object control retains the last payload");
    }
};

struct StringGrowth {
    pineforge::ScriptString value;

    explicit StringGrowth(const std::shared_ptr<MapStoreHolder>& holder)
        : value(*holder->store, "seed") {}

    void grow(std::uint64_t, const std::string& payload) { value.append(payload); }
    void verify(std::uint64_t count, const std::string& payload) const {
        require(value.size() == 4 + count * payload.size()
                    && value[0] == 's' && value[value.size() - 1] == payload.back(),
                "disk string control retains the seed and appended bytes");
    }
};

struct ArrayGrowth {
    pineforge::ScriptArray<std::string> values;

    explicit ArrayGrowth(const std::shared_ptr<MapStoreHolder>& holder)
        : values(*holder->store, 1, std::string("seed")) {}

    void grow(std::uint64_t, const std::string& payload) { values.push(payload); }
    void verify(std::uint64_t count, const std::string& payload) const {
        require(values.size() == count + 1 && values.get(0) == "seed"
                    && values.get(count) == payload,
                "disk array control retains the seed and new elements");
    }
};

// This is the real encoder used by PineMap::put for an external string value.
// Retain every encoded object in the Store until its run owner is destroyed.
// It isolates insert payload growth without the map's fixed slot preparation.
struct MapPayloadGrowth {
    Store& store;
    pineforge::detail::MapStringRep last{};

    explicit MapPayloadGrowth(const std::shared_ptr<MapStoreHolder>& holder)
        : store(*holder->store) {}

    void grow(std::uint64_t, const std::string& payload) {
        last = pineforge::detail::map_encode<std::string>(store, payload);
    }
    void verify(std::uint64_t, const std::string& payload) const {
        require(pineforge::detail::map_string_decode(store, last) == payload,
                "disk map payload control retains the encoded string");
    }
};

struct MapGrowth {
    pineforge::detail::MapStoreScope scope;
    pineforge::PineMap<std::int64_t, std::string> values;

    explicit MapGrowth(const std::shared_ptr<MapStoreHolder>& holder)
        : scope(holder),
          values(pineforge::PineMap<std::int64_t, std::string>::new_()) {
        (void)values.put(-1, "seed");
    }
    void grow(std::uint64_t index, const std::string& payload) {
        (void)values.put(static_cast<std::int64_t>(index), payload);
    }
    void verify(std::uint64_t count, const std::string& payload) const {
        require(values.size() == static_cast<int>(count + 1)
                    && values.get(-1) == "seed"
                    && values.get(static_cast<std::int64_t>(count - 1)) == payload,
                "disk map control retains the seed and inserted pairs");
    }
};

struct DriveResult {
    std::exception_ptr failure;
    std::uint64_t attempted = 0;
    std::uint64_t completed = 0;
    ScratchArenaStats exhausted{};
};

template<class Growth>
DriveResult drive(Store& store, Growth& growth, std::uint64_t limit,
                  const std::string& payload) {
    DriveResult result;
    // Construct owning containers before this transaction. Roll back before
    // their destructors, especially ScriptArray's terminating cleanup path.
    auto transaction = store.begin();
    try {
        for (std::uint64_t index = 0; index < limit; ++index) {
            ++result.attempted;
            growth.grow(index, payload);
            ++result.completed;
        }
        transaction->commit();
    } catch (...) {
        // Capture the original exception and physical witness before rollback
        // or destructor I/O can replace the backing's last carried failure.
        result.failure = std::current_exception();
        if (const auto* arena = ScratchArena::attached())
            result.exhausted = arena->stats();
        transaction->rollback();
    }
    return result;
}

void typed_exhaustion(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        const auto* info = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        const auto classified = pineforge::classify_run_failure(error);
        const std::string code = pineforge::run_failure_code_name(classified.code);
        const bool bare_runtime_error =
            dynamic_cast<const std::runtime_error*>(&error) != nullptr && info == nullptr;
        if (code != "report_sink_failed") {
            throw std::runtime_error(
                "typed-code assertion: expected report_sink_failed, observed " + code
                + "; bare runtime_error=" + (bare_runtime_error ? "yes" : "no")
                + "; exception=" + error.what());
        }
        require(code != "engine_unclassified_error",
                "arena exhaustion must never classify as engine_unclassified_error");
        require(!bare_runtime_error,
                "arena exhaustion must not escape as a runtime_error without a run failure");
        require(info != nullptr, "arena exhaustion carries RunFailureInfo");
        const auto& carried = info->run_failure();
        require(std::string(pineforge::run_failure_code_name(carried.code))
                    == "report_sink_failed",
                "the exception itself carries report_sink_failed");
        require(carried.args != nullptr
                    && carried.args->find("\"reason\":\"arena_exhausted\"")
                        != std::string::npos,
                "the exception retains typed arena_exhausted arguments");
    }
}

std::uint64_t map_arena_bytes(const std::string& payload) {
    require(ScratchArena::attached() == nullptr, "map sizing control has no attached arena");
    auto holder = script_store();
    require(holder->backing->kind() == ScratchBackingKind::disk,
            "map sizing control uses the production disk backing");
    MapGrowth growth(holder);
    const auto control = drive(*holder->store, growth, 1, payload);
    require(!control.failure && control.completed == 1,
            "disk map preparation and first insertion complete without a throw");
    growth.verify(1, payload);
    const auto extent = holder->backing->page_count() * holder->backing->page_size();
    require(extent > kArenaBytes, "a prepared map needs more than the minimum arena");
    // A map prepares all 50,000 slots. Use the smallest whole-MiB arena
    // covering actual disk preparation plus one committed insertion, so the
    // arena witness cannot fail at map.new_() or its first growth operation.
    return ((extent + kArenaBytes - 1) / kArenaBytes) * kArenaBytes;
}

template<class Growth>
bool check_case(const char* name, std::uint64_t arena_bytes = kArenaBytes) {
    try {
        const std::string payload(kPayloadBytes, 'v');
        if (arena_bytes == 0) arena_bytes = map_arena_bytes(payload);
        DriveResult exhausted;
        {
            ArenaAttachment attachment(arena_bytes);
            auto* arena = ScratchArena::attached();
            require(arena != nullptr && arena->stats().bytes_supplied == arena_bytes,
                    "the real arena is attached at the requested size");
            auto holder = script_store();
            Growth growth(holder);
            // Exceed arena and resident workspace together, so dirty cache
            // pages cannot defer the first exhaustion until final commit.
            const auto volume = arena_bytes + holder->store->stats().bytes_resident;
            exhausted = drive(*holder->store, growth,
                              2 * volume / kPayloadBytes + 1, payload);
            require(exhausted.failure != nullptr && exhausted.completed > 0
                        && exhausted.attempted == exhausted.completed + 1,
                    "successful growth precedes exhaustion inside a container operation");
            require(exhausted.exhausted.bytes_used == arena_bytes
                        && exhausted.exhausted.high_water == arena_bytes
                        && exhausted.exhausted.chunks_free == 0,
                    "the failed operation filled the attached arena, without disk fallback");
        }
        require(ScratchArena::attached() == nullptr,
                "the volume control starts after arena teardown");
        {
            auto holder = script_store();
            require(holder->backing->kind() == ScratchBackingKind::disk,
                    "the unattached volume control uses the production disk backing");
            Growth growth(holder);
            // Include the operation that exhausted the arena, not just the
            // successful prefix. Use the identical driver, payload and shape.
            const auto control = drive(*holder->store, growth, exhausted.attempted, payload);
            require(!control.failure && control.completed == exhausted.attempted,
                    "the same volume including the refused operation completes on disk");
            growth.verify(control.completed, payload);
            std::printf("%s: disk control completed %llu growth operations\n", name,
                        static_cast<unsigned long long>(control.completed));
            std::fflush(stdout);
        }
        // Deliberately after the control: the unfixed tree must fail here,
        // while still showing that the identical disk workload completed.
        typed_exhaustion(exhausted.failure);
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
    } catch (...) {
        std::fprintf(stderr, "%s: unexpected non-standard exception\n", name);
    }
    return false;
}
} // namespace

int main() {
    unsigned failures = 0;
    failures += check_case<ObjectGrowth>("object store") ? 0 : 1;
    failures += check_case<StringGrowth>("string value") ? 0 : 1;
    failures += check_case<ArrayGrowth>("array element growth") ? 0 : 1;
    failures += check_case<MapPayloadGrowth>("map insert payload") ? 0 : 1;
    failures += check_case<MapGrowth>("map insert", 0) ? 0 : 1;
    return failures == 0 ? 0 : 1;
}
