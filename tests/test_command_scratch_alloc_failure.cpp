#include "command_scratch_arena_fixture.hpp"
#include "global_allocation_replacement.hpp"

#include <pineforge/run_failure.hpp>
#include <pineforge/run_failure_codes.hpp>
#include <pineforge/source/exit_scratch.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
using namespace command_arena_test;
using pineforge::RunFailureCode;
namespace exit_scratch = pineforge::source::detail;

constexpr auto kAllocationFaultDisabled = std::numeric_limits<std::size_t>::max();

struct Environment {
    const char* name;
    bool present;
    std::string previous;

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
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
};

void require_code(const std::exception& error, RunFailureCode expected) {
    const auto observed = pineforge::classify_run_failure(error).code;
    if (observed != expected)
        throw std::runtime_error(std::string("expected ")
            + pineforge::run_failure_code_name(expected) + ", observed "
            + pineforge::run_failure_code_name(observed));
    require(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr,
            "the exception must carry a typed run failure");
}

template<class Operation>
ScratchStatus fail_next_allocation(Operation operation) {
    require(!global_allocation::refusing
                && global_allocation::fail_after == kAllocationFaultDisabled,
            "allocation fault starts disarmed");
    const auto before = global_allocation::refused;
    struct Disarm {
        ~Disarm() { global_allocation::fail_after = kAllocationFaultDisabled; }
    } disarm;

    global_allocation::fail_after = 0;
    const auto status = operation();
    const bool consumed = global_allocation::fail_after == kAllocationFaultDisabled;
    global_allocation::fail_after = kAllocationFaultDisabled;

    const auto faults = global_allocation::refused - before;
    if (faults != 1)
        throw std::runtime_error("expected one allocation refusal, observed "
                                 + std::to_string(faults));
    require(consumed, "the one-shot allocation fault must disarm itself");
    return status;
}

void fill_arena(ScratchFile& filler) {
    auto* arena = ScratchArena::attached();
    filler.open("command-allocation-filler");
    filler.ensure_size(arena->stats().bytes_total);
    const auto full = arena->stats();
    require(full.chunks_free == 0 && full.bytes_used == full.bytes_total,
            "real filler owns every arena chunk before arming");
}

void require_arena_exhausted(const ReportScratchIoError* failure) {
    require(failure != nullptr, "arena exhaustion carries its original exception");
    require_code(*failure, RunFailureCode::report_sink_failed);
    const auto& args = failure->run_failure().args;
    require(args && args->find("\"reason\":\"arena_exhausted\"") != std::string::npos,
            "arena exhaustion retains its typed reason");
    require(args->find("\"store\":\"command\"") != std::string::npos,
            "arena exhaustion identifies the command store");
}

void backing_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    FailedWrites failures;
    ObserverRegistration observation(failures);
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{};

    // Bounded directories and window tables are preallocated. The first
    // allocation here builds the exhaustion diagnostic inside ensure_size,
    // before a ReportScratchIoError exists for perform() to preserve.
    const auto status = fail_next_allocation([&]() noexcept {
        return backing->write_page(0, {image.data(), image.size()});
    });
    require(status == ScratchStatus::io_error, "allocation failure returns io_error");
    require(failures.io_errors == 1 && failures.requested_extent == kPageBytes,
            "the injected fault reaches exactly the selected backing page write");
    require(backing->page_count() == 0 && failures.snapshot.chunks_free == 0
                && failures.snapshot.bytes_used == kSmallArenaBytes,
            "the failed write neither claims a chunk nor publishes an extent");
    const auto* failure = backing->carried_failure();
    require(failure != nullptr,
            "expected out_of_memory, observed null carried failure");
    require_code(*failure, RunFailureCode::out_of_memory);
}

void store_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    FailedWrites failures;
    ObserverRegistration observation(failures);
    auto backing = make_command_scratch_backing(kPageBytes);
    CommandScratchStore store(config(), backing.get());
    require(store.status() == ScratchStatus::ok, "open the real command store before arming");
    require(store.begin() == ScratchStatus::ok, "begin the real store before arming");
    require(store.stats().page_writes == 0, "setup has not written any backing page");

    // An empty commit writes superblock page 1. Workspace allocation and
    // transaction setup have finished; only this commit is armed.
    const auto status = fail_next_allocation([&]() noexcept {
        return store.commit(ScratchDurability::buffered);
    });
    require(status == ScratchStatus::io_error, "store page-write failure returns io_error");
    require(store.stats().page_writes == 1 && failures.io_errors == 1
                && failures.requested_extent == 2 * kPageBytes,
            "the injected fault reaches the commit's single backing page write");

    bool caught = false;
    try {
        exit_scratch::exit_scratch_check(status, store);
    } catch (const std::exception& error) {
        caught = true;
        // Use the production consumer boundary, including its status-only
        // fallback, so losing the carrier reports the actual wrong code.
        require_code(error, RunFailureCode::out_of_memory);
        require(store.carried_failure() != nullptr,
                "the store retains the typed allocation failure for its consumer");
        require_code(*store.carried_failure(), RunFailureCode::out_of_memory);
    }
    require(caught, "the consumer must throw the store's allocation failure");
}

void unarmed_operations() {
    ArenaAttachment attachment(kSmallArenaBytes);
    const auto refused_before = global_allocation::refused;
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{}, read{};
    image.fill(std::byte{0x5a});
    const auto allocations_before_write = global_allocation::allocations;
    const auto write_status = backing->write_page(0, {image.data(), image.size()});
    require(write_status == ScratchStatus::ok, "unarmed page write succeeds with a free chunk");
    require(global_allocation::allocations == allocations_before_write,
            "healthy page I/O has no setup allocation for the hook to intercept");
    require(backing->read_page(0, {read.data(), read.size()}) == ScratchStatus::ok
                && read == image && backing->carried_failure() == nullptr,
            "unarmed page bytes round-trip without a carried failure");

    auto store_backing = make_command_scratch_backing(kPageBytes);
    CommandScratchStore store(config(), store_backing.get());
    require(store.status() == ScratchStatus::ok && store.begin() == ScratchStatus::ok,
            "unarmed store opens and begins");
    const auto allocations_before_commit = global_allocation::allocations;
    const auto status = store.commit(ScratchDurability::buffered);
    require(status == ScratchStatus::ok, "the same unarmed commit succeeds with a free chunk");
    require(global_allocation::allocations == allocations_before_commit,
            "healthy commit page I/O has no setup allocation for the hook to intercept");
    exit_scratch::exit_scratch_check(status, store);
    require(store.stats().page_writes == 1 && store_backing->page_count() == 2
                && !store.transaction_active() && store.carried_failure() == nullptr,
            "the successful control really writes and publishes the commit page");
    require(global_allocation::refused == refused_before, "unarmed operations inject no fault");
}

void unarmed_exhaustion_diagnostic() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    const auto refused_before = global_allocation::refused;
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{};
    require(backing->write_page(0, {image.data(), image.size()}) == ScratchStatus::io_error,
            "without injection the same full-arena write is genuine exhaustion");
    require_arena_exhausted(backing->carried_failure());

    auto store_backing = make_command_scratch_backing(kPageBytes);
    CommandScratchStore store(config(), store_backing.get());
    require(store.status() == ScratchStatus::ok && store.begin() == ScratchStatus::ok,
            "open and begin the unarmed full-arena store");
    const auto status = store.commit(ScratchDurability::buffered);
    require(status == ScratchStatus::io_error, "the unarmed full-arena commit is exhausted");
    require_arena_exhausted(store.carried_failure());
    bool caught = false;
    try {
        exit_scratch::exit_scratch_check(status, store);
    } catch (const ReportScratchIoError& error) {
        caught = true;
        require_arena_exhausted(&error);
    }
    require(caught, "the consumer preserves the unarmed arena exception");
    require(global_allocation::refused == refused_before, "genuine exhaustion injects no fault");
}

void arena_share_exhaustion() {
    ArenaAttachment attachment(kSmallArenaBytes);
    auto backing = make_command_scratch_backing(kPageBytes);
    std::array<std::byte, kPageBytes> image{};
    require(backing->write_page(0, {image.data(), image.size()}) == ScratchStatus::ok,
            "small-arena control first claims a real command chunk");
    const auto before = ScratchArena::attached()->stats();
    const auto first_page_past_chunk = ScratchArena::kChunkBytes / kPageBytes;
    require(backing->write_page(first_page_past_chunk, {image.data(), image.size()})
                == ScratchStatus::io_error,
            "writing past the small arena's command share returns io_error");
    require_arena_exhausted(backing->carried_failure());
    const auto after = ScratchArena::attached()->stats();
    require(before.bytes_used == ScratchArena::kChunkBytes && before.chunks_free == 3
                && after.bytes_used == before.bytes_used
                && after.high_water == before.high_water && after.chunks_free == 3,
            "a share crossing preserves free chunks and the original exhaustion verdict");
}

void disk_directory_io_failure() {
    require(ScratchArena::attached() == nullptr, "disk control starts without an attached arena");
    // The existing invalid-directory fixture also uses /dev/null to force
    // ENOTDIR regardless of privileges, without changing directory modes.
    Environment directory("PINEFORGE_COMMAND_SCRATCH_DIR", "/dev/null");
    bool caught = false;
    try {
        (void)make_command_scratch_backing(kPageBytes);
    } catch (const std::system_error& error) {
        caught = true;
        require_code(error, RunFailureCode::scratch_io_failed);
        require(error.code() == std::error_code(ENOTDIR, std::generic_category()),
                "a genuine invalid scratch directory preserves ENOTDIR");
        require(std::string(error.what()).find("command scratch create") == 0
                    && std::string(error.what()).find("Not a directory") != std::string::npos,
                (std::string("the disk failure retains its command-create operation and the system text; observed what(): ")
                    + error.what()).c_str());
    }
    require(caught, "an unusable scratch directory must fail");
}
} // namespace

int main() {
    try {
        Environment byte_cap("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr);
        Environment window_cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
        struct Row { const char* name; void (*run)(); };
        const std::array<Row, 6> rows{{
            {"backing_allocation_failure", backing_allocation_failure},
            {"store_allocation_failure", store_allocation_failure},
            {"unarmed_operations", unarmed_operations},
            {"unarmed_exhaustion_diagnostic", unarmed_exhaustion_diagnostic},
            {"arena_share_exhaustion", arena_share_exhaustion},
            {"disk_directory_io_failure", disk_directory_io_failure},
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
        std::fprintf(stderr, "FAIL command scratch allocation setup: %s\n", error.what());
        return 1;
    }
}
