#include "command_scratch_arena_fixture.hpp"
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/command_scratch_cache.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/run_failure.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace command_arena_test;
using pineforge::NativeExecutionConsumer;
using pineforge::RunFailureCode;

constexpr auto kDisabled = std::numeric_limits<std::size_t>::max();

// The older header kept kMaxSlots private; its minimum cached page is 512 bytes.
// Retain that geometry oracle so the inline-table parent reaches the assertions.
template<class Cache = CachedScratchBacking, std::size_t Slots = Cache::kMaxSlots>
constexpr std::size_t cache_slot_array_bytes(int) { return Slots * sizeof(ScratchPageId); }
constexpr std::size_t cache_slot_array_bytes(...) {
    return CachedScratchBacking::kCacheBytes / 512 * sizeof(ScratchPageId);
}

struct AllocationFault {
    const std::size_t before = global_allocation::refused;
    unsigned arms = 0;
    bool clean = true;

    void arm() noexcept {
        clean = clean && !global_allocation::refusing
            && global_allocation::fail_after == kDisabled;
        ++arms;
        global_allocation::fail_after = 0;
    }
    void finish() {
        const bool consumed = global_allocation::fail_after == kDisabled;
        global_allocation::fail_after = kDisabled;
        require(clean && arms == 1, "arm exactly once after operation setup");
        const auto faults = global_allocation::refused - before;
        if (faults != 1)
            throw std::runtime_error("expected one allocation refusal, observed "
                                     + std::to_string(faults));
        require(consumed, "the one-shot allocation fault consumed and disarmed");
    }
    ~AllocationFault() { global_allocation::fail_after = kDisabled; }
};

struct Environment {
    const char* name;
    const bool present;
    const std::string previous;
    Environment(const char* key, const char* value)
        : name(key), present(std::getenv(key) != nullptr),
          previous(present ? std::getenv(key) : "") {
        require((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0,
                "set scratch test environment");
    }
    ~Environment() {
        if (present) ::setenv(name, previous.c_str(), 1);
        else ::unsetenv(name);
    }
};

enum class EntryKind { run, exit };

// Observe successful setup allocations with the existing allocator callback.
// No ordinal countdown is armed across owner, file or cache construction.
struct WorkspaceBoundary {
    enum class Next { cache, slot_table, store_object, workspace, done };
    EntryKind kind;
    AllocationFault* fault = nullptr;
    bool fail_slot_table = false;
    Next next = Next::cache;
    unsigned boundaries = 0;
    bool slot_table_matches = true;
    bool store_object_matches = true;
    std::size_t workspace_bytes = 0;

    void before_workspace() noexcept {
        ++boundaries;
        if (fault) {
            // The observed allocation already succeeded. The FIRST new after
            // this callback is CommandScratchStore's nothrow workspace.
            fault->arm();
            next = Next::done;
        } else {
            next = Next::workspace;
        }
    }
    void allocated(std::size_t bytes) noexcept {
        switch (next) {
        case Next::cache:
            if (bytes == CachedScratchBacking::kCacheBytes) {
                if (fault && fail_slot_table) {
                    // The page buffer succeeded; the next new is the slot table.
                    ++boundaries;
                    fault->arm();
                    next = Next::done;
                } else next = Next::slot_table;
            }
            break;
        case Next::slot_table:
            slot_table_matches = bytes == cache_slot_array_bytes(0);
            if (!slot_table_matches) next = Next::done;
            else if (kind == EntryKind::run) next = Next::store_object;
            else before_workspace();
            break;
        case Next::store_object:
            store_object_matches = bytes == sizeof(CommandScratchStore);
            if (store_object_matches) before_workspace();
            else next = Next::done;
            break;
        case Next::workspace:
            workspace_bytes = bytes;
            next = Next::done;
            break;
        case Next::done:
            break;
        }
    }
    void check() const {
        require(boundaries == 1 && slot_table_matches && store_object_matches && next == Next::done,
                "observe the final setup allocation immediately before the workspace");
    }
};

struct AllocationObservation {
    static inline WorkspaceBoundary* current = nullptr;
    static void allocated(std::size_t bytes) noexcept { current->allocated(bytes); }
    explicit AllocationObservation(WorkspaceBoundary& boundary) {
        require(!current && !global_allocation::recording
                    && !global_allocation::on_recorded
                    && !global_allocation::refusing
                    && global_allocation::fail_after == kDisabled,
                "begin observing with every allocation fault disarmed");
        current = &boundary;
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = &allocated;
        global_allocation::recording = true;
    }
    ~AllocationObservation() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        global_allocation::live_bytes = 0;
        current = nullptr;
    }
};

struct RunScratchEntry {
    // The borrowed slot outlives its production owner.
    CommandScratchStore* borrowed = nullptr;
    std::unique_ptr<pineforge::IExecutionConsumer> owner =
        pineforge::make_native_execution_consumer();

    void begin() {
        pineforge::as_native_consumer(*owner).provision_run_command_scratch(
            NativeExecutionConsumer::CommandScratchBinding::Trade,
            pineforge::source::PineStrategyHost::trade_scratch_order(), borrowed);
    }
    CommandScratchStore& store() {
        require(borrowed != nullptr, "the real run entry publishes its borrowed store");
        return *borrowed;
    }
};

class ExitScratchEntry final : public pineforge::source::PineStrategyHost {
public:
    ExitScratchEntry() {
        configure_pine_strategy(pineforge::source::PineStrategyConfig{});
        attach_pine_execution_adapter();
        args_.bars = bars_.data();
        args_.n = static_cast<int>(bars_.size());
        args_.input_tf = "1";
        args_.script_tf = "1";
    }
    void on_source_bar(const pineforge::Bar&) override {}
    void begin() { prepare_native_begin(args_); }
    CommandScratchStore& store() {
        require(native_state().kind == pineforge::NativeLifecycleKind::Ready,
                "the real source begin prepares a ready run");
        return pineforge::source::detail::exit_scratch(adapter_).store;
    }
private:
    const std::array<pineforge::Bar, 2> bars_{{
        {100, 101, 99, 100, 1, 1736121600000LL},
        {100, 101, 99, 100, 1, 1736121660000LL},
    }};
    pineforge::NativeBeginArgs args_;
};

template<class Entry>
std::exception_ptr enter(Entry& entry, WorkspaceBoundary& boundary) {
    AllocationObservation observation(boundary);
    try { entry.begin(); }
    catch (...) { return std::current_exception(); }
    return {};
}

void require_memory(std::exception_ptr failure) {
    require(failure != nullptr, "the production run entry must throw its allocation failure");
    try { std::rethrow_exception(failure); }
    catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        require(typed != nullptr, "the thrown exception must carry RunFailureInfo");
        const auto observed = typed->run_failure().code;
        if (observed != RunFailureCode::out_of_memory)
            throw std::runtime_error(std::string("expected out_of_memory, observed ")
                + pineforge::run_failure_code_name(observed));
        require(observed != RunFailureCode::scratch_io_failed,
                "scratch allocation failure must not become scratch_io_failed");
    }
}

template<class Entry>
void workspace_allocation_failure(EntryKind kind) {
    require(!ScratchArena::attached(), "run entry uses a healthy production disk backing");
    {
        Entry control;
        WorkspaceBoundary boundary{kind};
        const auto refused = global_allocation::refused;
        if (const auto error = enter(control, boundary)) std::rethrow_exception(error);
        boundary.check();
        auto& store = control.store();
        require(store.status() == ScratchStatus::ok && !store.carried_failure()
                    && store.record_count() == 0 && !store.transaction_active(),
                "the same unarmed production entry opens a healthy empty store");
        require(boundary.workspace_bytes == store.stats().resident_bytes
                    && boundary.workspace_bytes > sizeof(CommandScratchStore),
                "the first allocation after setup is exactly the store workspace");
        require(global_allocation::refused == refused,
                "the unarmed production entry refuses no allocation");
    }

    const bool slot_table_faults[] = {true, false};
    for (const bool fail_slot_table : slot_table_faults) {
        Entry failed;
        AllocationFault fault;
        WorkspaceBoundary boundary{kind, &fault, fail_slot_table};
        const auto error = enter(failed, boundary);
        fault.finish();
        boundary.check();
        require_memory(error);
    }
}

void run_scratch_workspace_allocation_failure() {
    workspace_allocation_failure<RunScratchEntry>(EntryKind::run);
}

void exit_scratch_workspace_allocation_failure() {
    workspace_allocation_failure<ExitScratchEntry>(EntryKind::exit);
}
} // namespace

int main() {
    try {
        Environment byte_cap("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr);
        Environment window_cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
        Environment command_directory("PINEFORGE_COMMAND_SCRATCH_DIR", ".");
        Environment directory("TMPDIR", ".");
        struct Row { const char* name; void (*run)(); };
        const std::array<Row, 2> rows{{
            {"run_scratch_workspace_allocation_failure", run_scratch_workspace_allocation_failure},
            {"exit_scratch_workspace_allocation_failure", exit_scratch_workspace_allocation_failure},
        }};
        int failures = 0;
        for (const auto& row : rows) {
            try {
                row.run();
                std::printf("PASS %s\n", row.name);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "FAIL %s: %s\n", row.name, error.what());
            } catch (...) {
                ++failures;
                std::fprintf(stderr, "FAIL %s: unknown exception\n", row.name);
            }
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL run scratch allocation setup: %s\n", error.what());
        return 1;
    }
}
