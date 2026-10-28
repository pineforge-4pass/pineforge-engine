// 1.7.0 review F-03 (argon cross-read of lane b1; verifier rev-read-verifyb1):
// a read of the spilled book that fails inside the noexcept continuation fold
// is never answered as a hash the failed read cut short, by any reader of the
// fold: it ends the run with the typed scratch failure, or, after the run,
// answers the typed failure of the read.
//
// broker_state_hash() folds the continuation hash, which walks the whole live
// book; with no script point latched (always so under the default
// NativeReportPolicy::HostRecorded) the host's projection computes it then.
// Round 1 made the fold carry a failed spilled read to the core instead of
// meeting noexcept, but the readers never threw it back: the caller got the
// truncated hash. record_script_report_point, the first throwing point after
// every script calculation, threw a carried failure only under KernelRecorded
// and returned before it under every other policy. Round 3 fixed the native
// host's projection and that report point; the base engine's projection
// (BacktestEngine::broker_state_hash_projection) still folded with no rethrow,
// and the C entry points had no exception barrier, so a query after the run
// unwound through C.
//
// Fault injection is test_continuation_hash_spilled_read_failure's: the
// book's first spill opens unlinked scratch files this process holds, found
// as the descriptors that appear across the submit that makes it, and
// truncating them makes every later read short (EIO). The test opens no path.
//
// The run: bar 1 places 4,200 resting buy limits (the book spills). Rows:
//   default            the last bar's callback truncates the spill files and
//                      asks for broker_state_hash(): the call must not return,
//                      and the run must end Failed with scratch_io_failed.
//   --swallowed        the same, but the host catches whatever the call
//                      throws and returns normally, as a host may; the failure
//                      is still carried, so the bar's report point must end
//                      the run Failed with scratch_io_failed.
//   --base-projection  the callback asks the base engine's projection
//                      (BacktestEngine::broker_state_hash_projection, the
//                      generic fold a host reaches as its base): the same
//                      answer as the default row.
//   --c-entry          the run completes over a readable book; then the spill
//                      files are truncated and the C entry points
//                      strategy_broker_state_hash and strategy_stream_state_hash
//                      are asked: neither may let an exception out, each
//                      answers 0, and the handle's failure record names
//                      scratch_io_failed.
//
// Only the public NativeStrategyHost API and the public C API of the previous
// epoch, so this TU compiles against the unfixed head; there the default and
// the swallowed row fail at c7e3cc256, the base-projection row answers the
// hash, and the C entry points let the typed failure unwind out of C.
//
// Round 5: the Pine host's three readers of the fold, the twin row _pine_host
// (its own executable, built with PINEFORGE_B1_PINE_HOST: see the Pine part
// below). It is registered only when the source layer is built, so the four
// rows above stay source-free and keep running in the kernel-only profile.
//
// Each twin row is its own executable (the dedicated scratch matching fixture:
// file names every row after a source of its own), built from this body with
// PINEFORGE_B1_TWIN_FLAG set to the flag the row stands for.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#ifdef PINEFORGE_B1_PINE_HOST
#include <pineforge/source/pine_strategy_host.hpp>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)

// The book's resident bound (kBookMax in src/native_order.cpp): the submit
// that makes the 4,097th live row is the book's first spill.
constexpr std::size_t kResidentRows = 4096;
constexpr std::size_t kRows = 4200;
constexpr int kBars = 3;

enum class Mode { Hash, Swallowed, BaseProjection, CEntry };

// The unlinked regular files among this process's low descriptors: the
// scratch files (mkstemp, then unlink) the kernel holds open.
std::vector<int> unlinked_descriptors() {
    std::vector<int> found;
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 4096L) : 1024L;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0) {
            found.push_back(fd);
        }
    }
    return found;
}

no::Request limit_buy(double price) {
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{price};
    return request;
}

class FaultHost final : public NativeStrategyHost {
public:
    explicit FaultHost(Mode mode) : mode_(mode) {}

    std::vector<int> spill_files;     // the descriptors the book's first spill opened
    std::size_t book = 0;
    std::size_t rejected = 0;
    bool injected = false;
    bool asked = false;               // the last bar's callback reached the call
    bool hash_answered = false;       // the projection returned after the fault
    bool swallowed = false;           // it threw, and this host caught it

    // From here on every read of the book's spill files is short.
    void inject() {
        for (const int fd : spill_files) {
            if (::ftruncate(fd, 0) == 0) injected = true;
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 1) {
            std::vector<int> before;
            for (std::size_t i = 0; i != kRows; ++i) {
                if (i == kResidentRows) before = unlinked_descriptors();
                if (!submit(limit_buy(50.0 - static_cast<double>(i % 100) * 0.25)).handle) {
                    ++rejected;
                }
                if (i == kResidentRows) {
                    for (const int fd : unlinked_descriptors()) {
                        if (std::find(before.begin(), before.end(), fd) == before.end()) {
                            spill_files.push_back(fd);
                        }
                    }
                }
            }
            book = native_working_requests().size();
            return;
        }
        // The C entry row reads after the run, over a book the run read whole.
        if (bar_ != kBars || spill_files.empty() || mode_ == Mode::CEntry) return;
        inject();
        asked = true;
        // No script point is latched under HostRecorded, so the projection
        // folds the continuation now and meets the failed read. Fixed, the
        // failure comes back to this caller, typed.
        if (mode_ == Mode::Hash) {
            (void) broker_state_hash();
            hash_answered = true;
            return;
        }
        if (mode_ == Mode::BaseProjection) {
            // The generic projection, which a host that overrides none folds
            // through, called on this host as its base.
            (void) BacktestEngine::broker_state_hash_projection();
            hash_answered = true;
            return;
        }
        try {
            (void) broker_state_hash();
            hash_answered = true;
        } catch (...) {
            swallowed = true;
        }
    }

private:
    Mode mode_;
    int bar_ = 0;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    std::vector<Bar> bars;
    for (int index = 0; index != kBars; ++index) {
        Bar bar{};
        bar.open = 100.0;
        bar.high = 100.0;
        bar.low = 100.0;
        bar.close = 100.0;
        bar.volume = 1.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 300000;
        bars.push_back(bar);
    }
    return bars;
}

NativeRunSpec spec() {
    NativeRunSpec spec;
    spec.identity = {"f-03-broker-hash-read-failure", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:F03";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    // The policy under which no script point is latched and the kernel
    // records no report point of its own (the default, named here).
    spec.report_policy = NativeReportPolicy::HostRecorded;
    return spec;
}

template <class Host>
void check_failure_code(const Host& host) {
    const RunFailureValue failure = run_failure_value_of(host);
    if (failure.code != RunFailureCode::scratch_io_failed) {
        std::fprintf(stderr, "run failure %s: %s\n", run_failure_code_name(failure.code),
                     host.last_error().c_str());
    }
    CHECK(failure.code == RunFailureCode::scratch_io_failed);
}

#ifdef PINEFORGE_B1_PINE_HOST
// The Pine host's readers (src/source/pine_strategy_host.cpp): its projection
// (broker_state_hash_projection, which folds when no script point is
// latched), the continuation capture (capture_script_continuation_hash, at a
// batch run's last bar without recording) and the scheduler's row
// (scheduler_record_broker_hash, at every script bar with recording on). Each
// one folds, then rethrows a carried failure before it answers, latches or
// records the value.
//
// The run: four script bars on the chart timeframe. Bar 1 places kResidentRows
// resting strategy.order limits (the book's resident bound, so the journal
// has paged already); bar 2 places the rest, and the 4,097th live row is the
// book's first spill: its files are the unlinked descriptors that appear
// across bar 2's order calls. At the fault bar the script truncates them,
// then:
//   Projection  (bar 3) the script asks broker_state_hash() with no script
//               point latched: the Pine projection folds. Fixed, the call
//               throws; the script catches it, as a host may.
//   Capture     (bar 4, the last) the script asks nothing; the last bar's
//               capture is the first fold. Fixed, it throws before it
//               latches, and the latch keeps what it held: nothing.
//   Record      (bar 3; recording on) the scheduler's row is the first fold
//               after the script. Fixed, it throws before the latch and the
//               row, so the fault bar records no row.
// Every case ends the run Failed with scratch_io_failed. Before round 4
// (28a665131) the run failed too, at the native report point after the
// script calculation (round 3), but each reader first answered the hash the
// failed read cut short: the projection returned it, the capture latched it as
// valid, the scheduler recorded it as the fault bar's row. So each case reads
// what its reader answered, latched or recorded.
//
// Between the script and the capture on the last bar, the scheduler's range
// end returns at once on a flat position and the adapter's close hook passes
// its quiet gates (process_orders_on_close is off), so neither reads the book.
enum class PineMode { Projection, Capture, Record };

constexpr int kPineBars = 4;

class PineFaultHost final : public source::PineStrategyHost {
public:
    explicit PineFaultHost(PineMode mode) : mode_(mode) {
        set_syminfo_timezone("UTC+0");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 1.0e9;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(mode == PineMode::Record);
    }

    std::vector<int> spill_files;     // the descriptors the book's first spill opened
    std::size_t book = 0;
    bool injected = false;
    bool asked = false;               // the fault bar's script reached its step
    bool latched_at_fault = true;     // a script point was latched at the fault
    std::size_t rows_at_fault = 0;    // Record: the rows recorded before the fault bar
    bool hash_answered = false;       // Projection: the call returned after the fault
    bool threw = false;               // Projection: it threw, and the script caught it

    // What the readers left once the run is over.
    bool latched_after_run() const { return last_script_continuation_valid_; }
    std::size_t recorded_rows() const { return broker_state_hashes_.size(); }

    void on_source_bar(const Bar&) override {
        ++bar_;
        if (bar_ == 1) {
            for (std::size_t i = 0; i != kResidentRows; ++i) place(i);
            return;
        }
        if (bar_ == 2) {
            const std::vector<int> before = unlinked_descriptors();
            for (std::size_t i = kResidentRows; i != kRows; ++i) place(i);
            for (const int fd : unlinked_descriptors()) {
                if (std::find(before.begin(), before.end(), fd) == before.end()) {
                    spill_files.push_back(fd);
                }
            }
            return;
        }
        if (bar_ != (mode_ == PineMode::Capture ? kPineBars : kPineBars - 1)) return;
        book = native_working_requests().size();
        if (spill_files.empty()) return;
        latched_at_fault = last_script_continuation_valid_;
        rows_at_fault = broker_state_hashes_.size();
        for (const int fd : spill_files) {
            if (::ftruncate(fd, 0) == 0) injected = true;
        }
        asked = true;
        if (mode_ != PineMode::Projection) return;
        try {
            (void) broker_state_hash();
            hash_answered = true;
        } catch (...) {
            threw = true;
        }
    }

private:
    void place(std::size_t i) {
        strategy_order("rest" + std::to_string(i), true, 1.0,
                       50.0 - static_cast<double>(i % 100) * 0.25);
    }

    PineMode mode_;
    int bar_ = 0;
};

void run_pine(PineMode mode, const char* name) {
    const int before = failures;
    try {
        PineFaultHost host(mode);
        std::vector<Bar> bars;
        const std::int64_t t0 = 1736121600000LL;
        for (int index = 0; index != kPineBars; ++index) {
            bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                            t0 + static_cast<std::int64_t>(index) * 300000});
        }
        host.run(bars.data(), static_cast<int>(bars.size()));
        // Setup: every order rests, the book spilled during bar 2's orders
        // (if no descriptor appeared across them, the row fails here, and
        // does not test a site it never reached), and the fault bar's script
        // truncated the spill files with nothing latched.
        CHECK(host.book == kRows);
        CHECK(!host.spill_files.empty());
        CHECK(host.injected);
        CHECK(host.asked);
        // The run ends with the typed failure, not status 0.
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        check_failure_code(host);
        if (mode == PineMode::Projection) {
            // The projection folded (nothing latched) and never answered.
            CHECK(!host.latched_at_fault);
            CHECK(host.threw);
            CHECK(!host.hash_answered);
        } else if (mode == PineMode::Capture) {
            // The capture threw before its latch: the latch holds what it held.
            CHECK(!host.latched_at_fault);
            CHECK(!host.latched_after_run());
        } else {
            // Rows were recorded before the fault; the fault bar recorded none.
            CHECK(host.rows_at_fault > 0);
            CHECK(host.recorded_rows() == host.rows_at_fault);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "pine host %s: %s\n", name, error.what());
    }
    if (failures != before) std::fprintf(stderr, "pine host %s: red\n", name);
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef PINEFORGE_B1_TWIN_FLAG
    const char* const flag = PINEFORGE_B1_TWIN_FLAG;
    (void) argc;
    (void) argv;
#else
    const char* const flag = argc > 1 ? argv[1] : "";
#endif
#ifdef PINEFORGE_B1_PINE_HOST
    (void) flag;
    run_pine(PineMode::Projection, "projection");
    run_pine(PineMode::Capture, "capture");
    run_pine(PineMode::Record, "record");
    std::printf("test_broker_state_hash_spilled_read_failure_pine_host: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
#else
    Mode mode = Mode::Hash;
    const char* row = "";
    if (std::strcmp(flag, "--swallowed") == 0) {
        mode = Mode::Swallowed;
        row = " --swallowed";
    } else if (std::strcmp(flag, "--base-projection") == 0) {
        mode = Mode::BaseProjection;
        row = " --base-projection";
    } else if (std::strcmp(flag, "--c-entry") == 0) {
        mode = Mode::CEntry;
        row = " --c-entry";
    }
    try {
        FaultHost host(mode);
        const auto setup = host.configure_native(spec());
        CHECK(setup.status == NativeSetupStatus::Applied);
        const std::vector<Bar> bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        // Setup: the book spilled, and its first spill opened scratch files
        // this process holds.
        CHECK(host.rejected == 0);
        CHECK(host.book == kRows);
        CHECK(host.book > kResidentRows);
        CHECK(!host.spill_files.empty());
        if (mode == Mode::CEntry) {
            // The run read a healthy book to the end; the book stays live
            // after it, until the next run begins.
            CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
            host.inject();
            CHECK(host.injected);
            bool escaped = false;
            std::uint64_t broker = 1;
            std::uint64_t stream = 1;
            try {
                broker = strategy_broker_state_hash(static_cast<BacktestEngine*>(&host));
            } catch (...) {
                escaped = true;
            }
            try {
                stream = strategy_stream_state_hash(static_cast<BacktestEngine*>(&host));
            } catch (...) {
                escaped = true;
            }
            // Nothing unwinds through C; the cut hash is not the answer; the
            // failed read is the handle's typed failure.
            CHECK(!escaped);
            CHECK(broker == 0);
            CHECK(stream == 0);
            check_failure_code(host);
        } else {
            // The fault was injected and the last bar's callback asked.
            CHECK(host.injected);
            CHECK(host.asked);
            // The hash cut short by the failed read is never answered.
            CHECK(!host.hash_answered);
            if (mode == Mode::Swallowed) CHECK(host.swallowed);
            // The run ends with the typed failure, not status 0.
            CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
            check_failure_code(host);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "broker state hash read failure: %s\n", error.what());
    }
    std::printf("test_broker_state_hash_spilled_read_failure%s: %d checks, %d failures\n",
                row, checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
