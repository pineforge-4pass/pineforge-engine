#include "command_scratch_arena_fixture.hpp"

#include <pineforge/array.hpp>
#include <pineforge/detail/script_store.hpp>
#include <pineforge/map.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/script_string.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <system_error>

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

std::shared_ptr<MapStoreHolder> commit_script_store() {
    pineforge::detail::o1::Shape shape;
    // Retain more than an arena chunk of dirty pages until the final commit.
    // Large map preparation may spill; its measured extent sizes the arena.
    shape.resident_pages = pineforge::detail::o1::kMaxResidentPages;
    return std::make_shared<MapStoreHolder>(
        pineforge::detail::make_script_scratch_backing(), shape);
}

enum class CommitStep { setup, put, commit, complete };

struct CommitDriveResult {
    std::exception_ptr failure;
    CommitStep step = CommitStep::setup;
    std::uint64_t completed = 0;
    pineforge::detail::o1::Stats before_commit{}, after_commit{};
    ScratchArenaStats exhausted{};
};

template<class Growth>
CommitDriveResult drive_commit(const std::shared_ptr<MapStoreHolder>& holder,
                               std::uint64_t count, const std::string& payload) {
    CommitDriveResult result;
    auto& store = *holder->store;
    auto transaction = store.begin();
    // Construct inside the fresh transaction so no pre-existing page needs a
    // write-through undo image. Keep the owner alive until after rollback:
    // array cleanup then sees a dead root instead of a poisoned transaction.
    std::unique_ptr<Growth> growth;
    try {
        growth = std::make_unique<Growth>(holder);
        result.step = CommitStep::put;
        for (std::uint64_t index = 0; index < count; ++index) {
            growth->grow(index, payload);
            ++result.completed;
        }
        result.before_commit = store.stats();
        result.step = CommitStep::commit;
        transaction->commit();
        result.step = CommitStep::complete;
        result.after_commit = store.stats();
    } catch (...) {
        result.failure = std::current_exception();
        if (const auto* arena = ScratchArena::attached())
            result.exhausted = arena->stats();
        transaction->rollback();
    }
    if (!result.failure) growth->verify(count, payload);
    return result;
}

template<class Growth>
bool check_commit_case(const char* name) {
    try {
        require(ScratchArena::attached() == nullptr,
                "commit sizing control starts without an attached arena");
        const std::string payload(kPayloadBytes, 'v');
        CommitDriveResult control;
        std::uint64_t page_bytes = 0, count = 0;
        {
            auto holder = commit_script_store();
            require(holder->backing->kind() == ScratchBackingKind::disk,
                    "commit sizing control uses the production disk backing");
            page_bytes = holder->backing->page_size();
            require(page_bytes == payload.size()
                        && ScratchArena::kChunkBytes % page_bytes == 0,
                    "commit workload adds one backing page of payload per growth");
            count = ScratchArena::kChunkBytes / page_bytes;
            require(holder->store->stats().bytes_resident > count * page_bytes,
                    "the fixed resident workspace exceeds the retained payload");
            control = drive_commit<Growth>(holder, count, payload);
            require(!control.failure && control.step == CommitStep::complete
                        && control.completed == count,
                    "the identical disk sequence completes every put and its commit");
        }
        // Cover every write made during construction and puts, but refuse the
        // next whole arena chunk needed only by commit's dirty-page flush.
        const auto pages_per_chunk = ScratchArena::kChunkBytes / page_bytes;
        const auto chunks = control.before_commit.pages_spilled == 0 ? 1
            : (control.before_commit.pages_spilled + pages_per_chunk - 1)
                / pages_per_chunk;
        const auto arena_bytes = chunks * ScratchArena::kChunkBytes;
        require(control.before_commit.bytes_backing <= arena_bytes
                    && control.after_commit.bytes_backing > arena_bytes,
                "disk stats prove only commit crosses the derived arena boundary");
        std::printf("%s: disk control completed %llu puts and commit; "
                    "before=%llu, committed=%llu, arena=%llu bytes\n", name,
                    static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(control.before_commit.bytes_backing),
                    static_cast<unsigned long long>(control.after_commit.bytes_backing),
                    static_cast<unsigned long long>(arena_bytes));
        std::fflush(stdout);

        CommitDriveResult exhausted;
        {
            ArenaAttachment attachment(arena_bytes);
            auto* arena = ScratchArena::attached();
            require(arena != nullptr && arena->stats().bytes_supplied == arena_bytes,
                    "commit witness attaches the disk-derived arena size");
            auto holder = commit_script_store();
            exhausted = drive_commit<Growth>(holder, count, payload);
            require(exhausted.failure != nullptr
                        && exhausted.step == CommitStep::commit
                        && exhausted.completed == count,
                    "every put returns before the marked commit alone throws");
            require(exhausted.before_commit.bytes_backing
                        == control.before_commit.bytes_backing
                        && exhausted.before_commit.bytes_live
                            == control.before_commit.bytes_live,
                    "arena and disk complete the identical put phase");
            require(exhausted.exhausted.bytes_used == arena_bytes
                        && exhausted.exhausted.high_water == arena_bytes
                        && exhausted.exhausted.chunks_free == 0,
                    "commit exhausts the attached arena without disk fallback");
        }
        typed_exhaustion(exhausted.failure);
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
    } catch (...) {
        std::fprintf(stderr, "%s: unexpected non-standard exception\n", name);
    }
    return false;
}

struct FileSizeLimit {
    struct rlimit previous_limit{};
    struct sigaction previous_signal{};
    bool limit_changed = false;
    bool signal_changed = false;

    explicit FileSizeLimit(bool refuse_growth) {
        if (::getrlimit(RLIMIT_FSIZE, &previous_limit) != 0)
            throw std::system_error(errno, std::generic_category(), "read file-size limit");
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        if (::sigemptyset(&ignored.sa_mask) != 0
            || ::sigaction(SIGXFSZ, &ignored, &previous_signal) != 0)
            throw std::system_error(errno, std::generic_category(), "ignore SIGXFSZ");
        signal_changed = true;
        if (refuse_growth) {
            auto limit = previous_limit;
            // Preserve the hard limit so an unprivileged process can restore.
            limit.rlim_cur = 0;
            if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
                const int error = errno;
                restore();
                throw std::system_error(error, std::generic_category(), "set file-size limit");
            }
            limit_changed = true;
        }
    }
    ~FileSizeLimit() { restore(); }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

    void restore() noexcept {
        const int limit_error =
            limit_changed && ::setrlimit(RLIMIT_FSIZE, &previous_limit) != 0 ? errno : 0;
        const int signal_error =
            signal_changed && ::sigaction(SIGXFSZ, &previous_signal, nullptr) != 0 ? errno : 0;
        if (limit_error != 0 || signal_error != 0) {
            std::fprintf(stderr, "file-size guard restore failed: limit errno=%d, signal errno=%d\n",
                         limit_error, signal_error);
            // Never continue into another row with a poisoned process limit.
            std::terminate();
        }
        limit_changed = signal_changed = false;
    }
};

std::string disk_failure_details(const std::exception& error) {
    const auto classified = pineforge::classify_run_failure(error);
    const auto* io = dynamic_cast<const std::system_error*>(&error);
    return std::string("code=") + pineforge::run_failure_code_name(classified.code)
        + "; errno=" + (io ? std::to_string(io->code().value())
                                + " (" + io->code().message() + ")" : "unavailable")
        + "; exception=" + error.what();
}

void typed_scratch_io(const std::exception_ptr& failure) {
    require(failure != nullptr, "the zero file-size limit must throw");
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        const auto* info = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        const auto classified = pineforge::classify_run_failure(error);
        if (classified.code != pineforge::RunFailureCode::report_sink_failed || !info) {
            throw std::runtime_error("expected typed report_sink_failed; "
                                     + disk_failure_details(error));
        }
        const auto* io = dynamic_cast<const std::system_error*>(&error);
        if (!io || io->code() != std::error_code(EFBIG, std::generic_category())) {
            throw std::runtime_error("expected disk errno EFBIG; "
                                     + disk_failure_details(error));
        }
        const auto& carried = info->run_failure();
        require(carried.code == pineforge::RunFailureCode::report_sink_failed,
                "the disk exception itself carries report_sink_failed");
        require(carried.args != nullptr
                    && carried.args->find("\"reason\":\"scratch_io\"") != std::string::npos,
                "the disk exception retains typed scratch_io arguments");
    }
}

enum class DiskStep { setup, growth, commit, complete };

struct DiskDriveResult {
    std::exception_ptr failure;
    DiskStep step = DiskStep::setup;
    std::uint64_t attempted = 0, completed = 0, bytes_backing = 0;
};

template<class Growth>
DiskDriveResult drive_disk(Store& store, Growth& growth, std::uint64_t count,
                          const std::string& payload, bool refuse_growth) {
    DiskDriveResult result;
    auto transaction = store.begin();
    try {
        // Unwinding restores both process settings before rollback or cleanup.
        FileSizeLimit limit(refuse_growth);
        require(ScratchArena::attached() == nullptr, "disk growth has no attached arena");
        result.step = DiskStep::growth;
        for (std::uint64_t index = 0; index < count; ++index) {
            ++result.attempted;
            growth.grow(index, payload);
            ++result.completed;
        }
        result.bytes_backing = store.stats().bytes_backing;
        result.step = DiskStep::commit;
        transaction->commit();
        result.step = DiskStep::complete;
    } catch (...) {
        result.failure = std::current_exception();
        result.bytes_backing = store.stats().bytes_backing;
        transaction->rollback();
    }
    return result;
}

template<class Growth>
bool check_disk_case(const char* name) {
    try {
        require(ScratchArena::attached() == nullptr, "disk control has no attached arena");
        const std::string payload(kPayloadBytes, 'v');
        pineforge::detail::o1::Stats initial{};
        std::uint64_t count = 0;
        {
            auto holder = script_store();
            require(holder->backing->kind() == ScratchBackingKind::disk,
                    "file-size control uses the production disk backing");
            Growth growth(holder);
            initial = holder->store->stats();
            const auto page_bytes = holder->backing->page_size();
            require(page_bytes == payload.size(),
                    "the disk workload adds one backing page of payload per growth");
            // Exceed the store's fixed resident workspace before final commit.
            count = 2 * initial.bytes_resident / page_bytes + 1;
            const auto control = drive_disk(*holder->store, growth, count, payload, false);
            if (control.failure) std::rethrow_exception(control.failure);
            require(control.step == DiskStep::complete && control.completed == count,
                    "the identical sequence completes without lowering the file-size limit");
            require(control.bytes_backing > initial.bytes_backing
                        && holder->store->stats().bytes_backing > 0,
                    "disk control spills during growth and retains a nonempty backing");
            growth.verify(count, payload);
            std::printf("%s: disk control completed %llu growth operations\n", name,
                        static_cast<unsigned long long>(count));
            std::fflush(stdout);
        }
        DiskDriveResult refused;
        {
            require(ScratchArena::attached() == nullptr,
                    "file-size refusal has no attached arena");
            auto holder = script_store();
            require(holder->backing->kind() == ScratchBackingKind::disk,
                    "file-size refusal uses the production disk backing");
            Growth growth(holder);
            const auto before = holder->store->stats();
            require(before.bytes_resident == initial.bytes_resident
                        && before.bytes_backing == initial.bytes_backing,
                    "control and refusal start with the identical disk workspace and extent");
            refused = drive_disk(*holder->store, growth, count, payload, true);
        }
        typed_scratch_io(refused.failure);
        require(refused.step == DiskStep::growth
                    && refused.attempted == refused.completed + 1,
                "the marked growth operation throws before final commit");
        require(refused.bytes_backing == initial.bytes_backing,
                "the zero file-size limit refuses the first backing extension");
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, disk_failure_details(error).c_str());
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
    failures += check_commit_case<ObjectGrowth>("object store commit") ? 0 : 1;
    failures += check_commit_case<StringGrowth>("string value commit") ? 0 : 1;
    failures += check_commit_case<ArrayGrowth>("array element growth commit") ? 0 : 1;
    failures += check_commit_case<MapPayloadGrowth>("map insert payload commit") ? 0 : 1;
    failures += check_commit_case<MapGrowth>("map insert commit") ? 0 : 1;
    failures += check_disk_case<ObjectGrowth>("object store file-size limit") ? 0 : 1;
    failures += check_disk_case<StringGrowth>("string value file-size limit") ? 0 : 1;
    failures += check_disk_case<MapGrowth>("map insert file-size limit") ? 0 : 1;
    return failures == 0 ? 0 : 1;
}
