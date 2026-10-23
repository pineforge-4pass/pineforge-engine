// S4E A-3. Fork isolation turns a noexcept teardown abort into a failing row.
// Compile and execute on the lab box, including the sanitizers profile.
#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using pineforge::RunFailureCode;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TeardownBacking final : public pd::ScratchBacking {
public:
    TeardownBacking(pd::FixedMemoryScratchBacking& inner, bool arena)
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner), arena_(arena),
          failure_(ENOSPC, "teardown erase",
                   {{"reason", "arena_exhausted"}, {"store", "command"},
                    {"arena_bytes", std::int64_t{4096}},
                    {"high_water_bytes", std::int64_t{4096}}}) {}
    bool fail = false;
    std::uint64_t failed_writes = 0;
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return fail && arena_ ? &failure_ : nullptr;
    }
protected:
    pd::ScratchStatus do_read_page(pd::ScratchPageId id,
                                  pd::ScratchBuffer bytes) const noexcept override {
        return inner_.read_page(id, bytes);
    }
    pd::ScratchStatus do_write_page(pd::ScratchPageId id,
                                   pd::ScratchBytes bytes) noexcept override {
        if (fail) {
            ++failed_writes;
            return pd::ScratchStatus::io_error;
        }
        return inner_.write_page(id, bytes);
    }
    pd::ScratchStatus do_sync(pd::ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }
private:
    pd::FixedMemoryScratchBacking& inner_;
    bool arena_;
    pd::ReportScratchIoError failure_;
};

struct Unwind {};
struct VersionScope {
    sd::ExitScratchVersionsProduction& versions;
    ~VersionScope() { versions.release_scope(1); }
};

int child_case(bool versions_case, bool unwind, bool arena) {
    constexpr std::size_t page_size = 512;
    std::vector<std::byte> bytes(1024 * page_size);
    pd::FixedMemoryScratchBacking memory({bytes.data(), bytes.size()}, page_size);
    TeardownBacking backing(memory, arena);
    pd::CommandScratchStore store{
        {2, page_size, 1, 1, sd::exit_scratch_key_order(),
         pd::ScratchOpenMode::create_empty}, &backing};
    require(store.status() == pd::ScratchStatus::ok, "open teardown fixture");
    sd::ExitScratchContext context{store, {}};
    sd::ExitScratchVersionsProduction versions(store);
    bool unwound = false;
    try {
        if (versions_case) {
            pineforge::native_order::Request request{};
            const auto handle = versions.request(1, request);
            require(handle != 0 && store.record_count() == 2,
                    "commit the version and its sequence before injection");
            try {
                VersionScope scope{versions};
                backing.fail = true;
                if (unwind) throw Unwind{};
            } catch (const Unwind&) {
                unwound = true;
            }
            // A carried teardown failure must be raised at this existing,
            // non-noexcept public provider boundary.
            (void)versions.request(2, request);
        } else {
            try {
                sd::ExitScratchIndex index(context);
                index.put(7, 11);
                require(store.record_count() == 1, "commit one scope row before injection");
                backing.fail = true;
                if (unwind) throw Unwind{};
            } catch (const Unwind&) {
                unwound = true;
            }
            require(unwind, "normal scope teardown must raise before the caller can return success");
            sd::ExitScratchIndex next(context);
            next.put(13, 17);
        }
    } catch (const std::exception& error) {
        require(backing.failed_writes > 0, "the fault must reach teardown writes");
        require(!unwind || unwound, "teardown must preserve the active exception");
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == (arena ? RunFailureCode::report_sink_failed
                                      : RunFailureCode::scratch_io_failed),
                "teardown must retain the typed scratch failure");
        if (arena) {
            require(failure.args
                    && failure.args->find("\"reason\":\"arena_exhausted\"") != std::string::npos,
                    "retain the arena exhaustion reason");
            const auto* io = dynamic_cast<const pd::ReportScratchIoError*>(&error);
            require(io && io->code().value() == ENOSPC, "retain the backing errno");
        }
        return 0;
    }
    throw std::runtime_error("teardown failure was ignored");
}

struct ProductionRowsFixture {
    static constexpr std::size_t page_size = 512;
    std::vector<std::byte> bytes;
    pd::FixedMemoryScratchBacking memory;
    TeardownBacking backing;
    pd::CommandScratchStore store;
    sd::ExitScratchVersionsProduction versions;
    sd::ExitScratchContext context;

    explicit ProductionRowsFixture(bool arena)
        : bytes(1024 * page_size), memory({bytes.data(), bytes.size()}, page_size),
          backing(memory, arena),
          store({2, page_size, 1, 1, sd::exit_scratch_key_order(),
                 pd::ScratchOpenMode::create_empty}, &backing),
          versions(store), context{store, {}, &versions} {
        require(store.status() == pd::ScratchStatus::ok, "open production rows fixture");
    }
};

std::uint64_t seed_rows(ProductionRowsFixture& fixture,
                       sd::ExitScratchRows<std::uint64_t>& rows) {
    pineforge::native_order::Request request{};
    const auto version = fixture.versions.request(rows.scope(), request);
    rows.push_back(version);
    require(version != 0 && rows.size() == 1 && rows.front() == version
            && fixture.store.record_count() == 3,
            "commit a real row, version and sequence in the same production scope");
    require(!fixture.versions.teardown_failed() && !fixture.context.teardown_failure.pending()
            && fixture.backing.failed_writes == 0, "seed before injecting any fault");
    return version;
}

void require_typed_failure(const std::exception& error, bool arena) {
    const auto failure = pineforge::classify_run_failure(error);
    require(failure.code == (arena ? RunFailureCode::report_sink_failed
                                  : RunFailureCode::scratch_io_failed),
            "production boundary must retain the typed scratch failure");
    if (arena) {
        require(failure.args
                && failure.args->find("\"reason\":\"arena_exhausted\"") != std::string::npos,
                "production boundary must retain the arena exhaustion reason");
        const auto* io = dynamic_cast<const pd::ReportScratchIoError*>(&error);
        require(io && io->code().value() == ENOSPC, "production boundary must retain errno");
    } else {
        const auto* io = dynamic_cast<const sd::ExitScratchError*>(&error);
        require(io && io->status() == pd::ScratchStatus::io_error,
                "status-only fault must remain an ExitScratchError with io_error");
    }
}

void require_next_scope_failure(ProductionRowsFixture& fixture, bool arena) {
    const auto next_scope = fixture.context.next_scope;
    bool entered = false;
    bool caught = false;
    try {
        sd::ExitScratchRows<std::uint64_t> next(fixture.context);
        entered = true;
    } catch (const std::exception& error) {
        require_typed_failure(error, arena);
        caught = true;
    }
    require(caught && !entered && fixture.context.next_scope == next_scope,
            "the next scratch entry must rethrow before constructing a scope");
}

int production_rows_case(bool unwind, bool arena) {
    ProductionRowsFixture fixture(arena);
    std::uint64_t version = 0;
    bool injected = false;
    bool caught = false;
    bool unwound = false;
    try {
        try {
            sd::ExitScratchRows<std::uint64_t> rows(fixture.context);
            version = seed_rows(fixture, rows);
            fixture.backing.fail = true;
            injected = true;
            if (unwind) throw Unwind{};
        } catch (const Unwind&) {
            unwound = true;
        }
    } catch (const std::exception& error) {
        require(injected && !unwind, "only normal production scope exit may throw here");
        require_typed_failure(error, arena);
        caught = true;
    }
    // The header makes normal exit a throwing boundary. During unwinding the
    // original exception must survive and the next boundary must raise instead.
    require(injected && fixture.backing.failed_writes > 0, "fault before rows scope end");
    require(unwind ? (unwound && !caught) : (caught && !unwound),
            "production destructor must obey the uncaught-exceptions rule");
    require(fixture.versions.teardown_failed(), "production release_scope must retain the fault");
    require(!fixture.context.teardown_failure.pending(),
            "a failed version release must skip the scope's own erase");

    // Disable new backing faults: both boundaries must use the retained failure,
    // without touching the store or trying to allocate another scope/version.
    fixture.backing.fail = false;
    const auto before = fixture.store.stats();
    require_next_scope_failure(fixture, arena);
    bool provider_caught = false;
    try {
        (void)fixture.versions.request(version);
    } catch (const std::exception& error) {
        require_typed_failure(error, arena);
        provider_caught = true;
    }
    require(provider_caught, "the next production provider entry must rethrow");
    const auto after = fixture.store.stats();
    require(after.page_reads == before.page_reads && after.page_writes == before.page_writes
            && after.syncs == before.syncs, "retained failure must precede all new store I/O");
    return 0;
}

int clear_rows_case() {
    ProductionRowsFixture fixture(false);
    bool clear_threw = false;
    bool caught = false;
    try {
        sd::ExitScratchRows<std::uint64_t> rows(fixture.context);
        (void)seed_rows(fixture, rows);
        fixture.backing.fail = true;
        try {
            rows.clear();
        } catch (const std::exception& error) {
            require_typed_failure(error, false);
            require(fixture.backing.failed_writes > 0, "fault must reach clear writes");
            require(fixture.context.teardown_failure.pending()
                    && !fixture.versions.teardown_failed(), "clear retains the scope's fault");
            require(rows.size() == 1, "failed clear must not report an empty selection");
            clear_threw = true;
            throw; // Exercise the production destructor while clear's error unwinds.
        }
    } catch (const std::exception& error) {
        require(clear_threw, "clear itself must throw, before scope destruction");
        require_typed_failure(error, false);
        caught = true;
    }
    require(caught && fixture.context.teardown_failure.pending(),
            "clear's scratch_io_failed must survive scope unwinding");
    require(!fixture.versions.teardown_failed(),
            "a pending clear failure must prevent a second version teardown fault");
    fixture.backing.fail = false;
    const auto before = fixture.store.stats();
    require_next_scope_failure(fixture, false);
    const auto after = fixture.store.stats();
    require(after.page_reads == before.page_reads && after.page_writes == before.page_writes
            && after.syncs == before.syncs, "clear's retained failure must precede new store I/O");
    return 0;
}

template<class Case> bool run_child(const char* name, bool unwind, bool arena, Case run) {
    const pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        try {
            const int result = run();
            ::_exit(result);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "teardown child: %s\n", error.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);
    const bool ok = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("teardown case=%s unwind=%d arena=%d status=%d %s\n",
                name, unwind, arena, status, ok ? "PASS" : "FAIL");
    return ok;
}
} // namespace

int main() {
    int failures = 0;
    for (const bool versions : {false, true}) {
        for (const bool unwind : {false, true}) {
            for (const bool arena : {false, true}) {
                if (!run_child(versions ? "version-release" : "index", unwind, arena,
                               [=] { return child_case(versions, unwind, arena); }))
                    ++failures;
            }
        }
    }
    for (const bool unwind : {false, true}) {
        for (const bool arena : {false, true}) {
            if (!run_child("production-rows", unwind, arena,
                           [=] { return production_rows_case(unwind, arena); }))
                ++failures;
        }
    }
    if (!run_child("clear-rows", true, false, [] { return clear_rows_case(); })) ++failures;
    return failures == 0 ? 0 : 1;
}
