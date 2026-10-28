// Truncating real spill files must preserve the failure code, text, and callback
// position through continuation hashing, broker hashing, and commands.
// Reuse descriptor truncation from the spilled hash tests; reads always reach
// the backing file rather than returning synthetic success.
// Dormant fast matching cannot observe faults in data it does not read.
// These cases explicitly read damaged data and propagate the real failure
// through public run/stream calls or the carried-failure guard.
// Printed failure traces allow comparison of eager and dormant runs.
#include "../src/native_execution_consumer.hpp"
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#if defined(PINEFORGE_TEST_DORMANT_JOIN_REACHABLE) && !defined(PINEFORGE_TEST_DORMANT_BOOK_PATCHED)
#define PINEFORGE_TEST_DORMANT_BOOK_PATCHED 1
#endif

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr std::size_t resident = 4096;

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

std::vector<int> unlinked_descriptors() {
    std::vector<int> out;
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 4096L) : 1024L;
    for (int fd = 3; fd < end; ++fd) {
        struct stat st{};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 0)
            out.push_back(fd);
    }
    return out;
}

enum class Mode { Continuation, Broker, Carried, Promotion };

class Host final : public NativeStrategyHost {
public:
    explicit Host(Mode mode, bool enabled) : mode_(mode), enabled_(enabled) {
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        consumer().set_match_dormant(enabled);
#endif
    }
    std::size_t input_bars = 0;
    std::size_t script_bars = 0;
    std::size_t tick_callbacks = 0;
    bool injected = false;
    bool answered = false;
    bool carried = false;
    std::vector<int> book_files;

    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { ++input_bars; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++script_bars;
        if (script_bars == 1) {
            for (std::size_t i = 0; i < resident; ++i) put(i);
        } else if (script_bars == 2) {
            // The resident rows have matched/joined. The next submission
            // spills a row and opens exactly the files to fault.
            const auto before = unlinked_descriptors();
            put(resident);
            for (const int fd : unlinked_descriptors())
                if (std::find(before.begin(), before.end(), fd) == before.end())
                    book_files.push_back(fd);
            for (std::size_t i = resident + 1; i < resident + 512; ++i) put(i);
        } else if (script_bars == 3
                   && mode_ != Mode::Carried && mode_ != Mode::Promotion) {
            inject_and_read();
        }
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        ++tick_callbacks;
        if ((mode_ == Mode::Carried || mode_ == Mode::Promotion) && !injected)
            inject_and_read();
    }
private:
    void inject_and_read() {
        require(consumer().request_core().book_spilled(), "spill read fault book did not spill");
        require(!book_files.empty(), "spill read fault first spill opened no observable file");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        if (enabled_) {
            require(consumer().dormant_rows() > 0, "spill read fault did not reach a dormant book");
            require(consumer().fast_points() > 0, "spill read fault never followed fast matching");
        }
#endif
        if (mode_ == Mode::Promotion) {
            // Prove the selected command must decode a spilled target before
            // cutting its file. A lookup reads once; promotion itself does not
            // increment this public lookup census.
            const auto reads = consumer().request_core().book_lookup_reads().rows;
            require(static_cast<bool>(consumer().request_core().hold_live(victim_)),
                    "spilled promotion target missing before read fault");
            require(consumer().request_core().book_lookup_reads().rows == reads + 1,
                    "promotion target was resident instead of spilled");
        }
        for (const int fd : book_files) {
            require(::ftruncate(fd, 0) == 0, "spill read fault could not truncate a book file");
            injected = true;
        }
        if (mode_ == Mode::Continuation) {
            (void)native_continuation_hash();
            answered = true;
        } else if (mode_ == Mode::Broker) {
            (void)broker_state_hash();
            answered = true;
        } else if (mode_ == Mode::Promotion) {
            // A real command must promote/decode its target. Dormant-only
            // setup degradation must never swallow this genuine read error.
            (void)cancel(victim_);
            answered = native_state().kind != NativeLifecycleKind::Failed;
        } else {
            // The noexcept kernel fold carries the REAL read failure.
            // on_native_tick precedes matching at this observed point:
            // unlike a script-bar callback, no report-point rethrow can
            // surface the carry before the matcher's carried-failure guard.
            (void)consumer().continuation_hash();
            try {
                consumer().request_core().rethrow_book_read_failure();
            } catch (const std::exception&) {
                carried = true;
            }
            require(carried, "noexcept continuation fold did not carry the failed spill read");
        }
    }
    void put(std::size_t i) {
        no::Request r{no::Transact{(i & 1) ? -1.0 : 1.0}, "rest", ""};
        r.trigger = no::Limit{(i & 1) ? 200.0 : 50.0};
        const auto result = submit(r);
        require(result.handle.has_value(), "spill read fault request placement refused");
        if (i + 1 == resident) victim_ = *result.handle;
    }
    Mode mode_;
    bool enabled_;
    no::RequestHandle victim_{};
};

struct FailureTrace {
    RunFailureCode code;
    NativeFailureCode native;
    NativeFailureOperation operation;
    std::size_t input, script, ticks;
    std::string text;
    bool operator==(const FailureTrace& other) const {
        return code == other.code && native == other.native && operation == other.operation
            && input == other.input && script == other.script && ticks == other.ticks
            && text == other.text;
    }
};

FailureTrace run(Mode mode, const char* name, bool enabled = true) {
    Host h(mode, enabled);
    NativeRunSpec s;
    s.identity = {"c3-dormant-read-failure", 1};
    s.input_tf = "5"; s.script_tf = "5";
    s.tickerid = "TEST:DORMANT"; s.timezone = "UTC"; s.session = "24x7";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1;
    // The carried row injects inside on_native_tick, immediately before
    // matching. HostRecorded still rethrows carries at a script report point.
    s.report_policy = NativeReportPolicy::HostRecorded;
    require(h.configure_native(s).status == NativeSetupStatus::Applied, "spill read fault run configuration refused");
    std::array<Bar, 5> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100, 100, 100, 100, 1,
                   1736121600000LL + static_cast<std::int64_t>(i) * 300000};
    if (mode == Mode::Carried || mode == Mode::Promotion) {
        require(h.stream_begin(bars.data(), 3, "5", "5"), "spill command or carried-failure stream warmup failed");
        require(!h.stream_push_tick({1736122500000LL, 1, 100, 1}),
                "command or carried spill read failure did not fail the observed matching point");
    } else {
        h.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(h.injected && !h.answered, "spill read fault was absent or the damaged operation returned successfully");
    require(h.native_state().kind == NativeLifecycleKind::Failed, "spill read failure did not fail the run");
    const auto failure = run_failure_value_of(h);
    require(failure.code == RunFailureCode::scratch_io_failed, h.last_error().c_str());
    require(h.script_bars == 3, "spill read failure moved to a later script callback");
    if (mode == Mode::Carried || mode == Mode::Promotion) {
        require((mode == Mode::Promotion || h.carried)
                    && h.input_bars == 3 && h.tick_callbacks == 1,
                "command or carried spill read failure missed the first observed matching point");
    } else {
        require(h.input_bars == 3, "public hash spill read failure moved from the fault callback");
    }
    std::printf("%s input=%zu script=%zu ticks=%zu failure=%s native=%u operation=%u text=%s\n",
                name, h.input_bars, h.script_bars, h.tick_callbacks, run_failure_code_name(failure.code),
                static_cast<unsigned>(h.native_state().failure.code),
                static_cast<unsigned>(h.native_state().failure.operation), h.last_error().c_str());
    return {failure.code, h.native_state().failure.code, h.native_state().failure.operation,
            h.input_bars, h.script_bars, h.tick_callbacks, h.last_error()};
}
} // namespace

int main() {
    try {
        run(Mode::Continuation, "continuation");
        run(Mode::Broker, "broker");
        run(Mode::Carried, "carried");
        const auto eager = run(Mode::Promotion, "promotion-eager", false);
        const auto dormant = run(Mode::Promotion, "promotion-dormant", true);
        require(eager == dormant, "spilled promotion read failure changed the eager failure code or callback");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_read_failure: %s\n", e.what());
        return 1;
    }
}
