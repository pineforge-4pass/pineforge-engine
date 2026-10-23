// S4E A-3. Fork isolation turns a noexcept teardown abort into a failing row.
// Compile and execute on the lab box, including the sanitizers profile.
#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
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
} // namespace

int main() {
    int failures = 0;
    for (const bool versions : {false, true}) {
        for (const bool unwind : {false, true}) {
            for (const bool arena : {false, true}) {
                const pid_t child = ::fork();
                if (child < 0) return 1;
                if (child == 0) {
                    try {
                        const int result = child_case(versions, unwind, arena);
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
                std::printf("teardown versions=%d unwind=%d arena=%d status=%d %s\n",
                            versions, unwind, arena, status, ok ? "PASS" : "FAIL");
                if (!ok) ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
