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
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

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

void check_failure_code(const FaultHost& host) {
    const RunFailureValue failure = run_failure_value_of(host);
    if (failure.code != RunFailureCode::scratch_io_failed) {
        std::fprintf(stderr, "run failure %s: %s\n", run_failure_code_name(failure.code),
                     host.last_error().c_str());
    }
    CHECK(failure.code == RunFailureCode::scratch_io_failed);
}

}  // namespace

int main(int argc, char** argv) {
    Mode mode = Mode::Hash;
    const char* row = "";
    if (argc > 1 && std::strcmp(argv[1], "--swallowed") == 0) {
        mode = Mode::Swallowed;
        row = " --swallowed";
    } else if (argc > 1 && std::strcmp(argv[1], "--base-projection") == 0) {
        mode = Mode::BaseProjection;
        row = " --base-projection";
    } else if (argc > 1 && std::strcmp(argv[1], "--c-entry") == 0) {
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
}
