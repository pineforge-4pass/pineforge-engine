// 1.7.0 review F-03 (argon cross-read of lane b1): a read of the spilled book
// that fails inside the noexcept continuation fold under broker_state_hash()
// ends the run with the typed scratch failure; it is never answered as a hash
// the failed read cut short, and a run whose report the host records does not
// complete with status 0 over it.
//
// broker_state_hash() folds the continuation hash, which walks the whole live
// book; with no script point latched (always so under the default
// NativeReportPolicy::HostRecorded) NativeStrategyHost's projection computes
// it then. Round 1 made the fold carry a failed spilled read to the core
// instead of meeting noexcept, but the projection never threw it back: the
// caller got the truncated hash. record_script_report_point, the first
// throwing point after every script calculation, threw a carried failure only
// under KernelRecorded and returned before it under every other policy.
//
// Fault injection is test_continuation_hash_spilled_read_failure's: the
// book's first spill opens unlinked scratch files this process holds, found
// as the descriptors that appear across the submit that makes it, and
// truncating them makes every later read short (EIO). The test opens no path.
//
// The run: bar 1 places 4,200 resting buy limits (the book spills); the last
// bar's callback truncates the spill files and asks for broker_state_hash().
// Default row: the call must not return, and the run must end Failed with
// scratch_io_failed. `--swallowed` row: the host catches whatever the call
// throws and returns normally, as a host may; the failure is still carried,
// so the bar's report point must end the run Failed with scratch_io_failed
// instead of letting it complete.
//
// Source-free, and only the public NativeStrategyHost API of the previous
// epoch, so this TU compiles against the unfixed head; there the hash is
// answered and the run completes (both rows fail).
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
    explicit FaultHost(bool swallow) : swallow_(swallow) {}

    std::vector<int> spill_files;     // the descriptors the book's first spill opened
    std::size_t book = 0;
    std::size_t rejected = 0;
    bool injected = false;
    bool asked = false;               // the last bar's callback reached the call
    bool hash_answered = false;       // broker_state_hash() returned after the fault
    bool swallowed = false;           // it threw, and this host caught it

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
        if (bar_ == kBars && !spill_files.empty()) {
            // From here on every read of the book's spill files is short.
            for (const int fd : spill_files) {
                if (::ftruncate(fd, 0) == 0) injected = true;
            }
            asked = true;
            // No script point is latched under HostRecorded, so the
            // projection folds the continuation now and meets the failed
            // read. Fixed, the failure comes back to this caller, typed.
            if (!swallow_) {
                (void) broker_state_hash();
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
    }

private:
    bool swallow_;
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

}  // namespace

int main(int argc, char** argv) {
    const bool swallow = argc > 1 && std::strcmp(argv[1], "--swallowed") == 0;
    try {
        FaultHost host(swallow);
        const auto setup = host.configure_native(spec());
        CHECK(setup.status == NativeSetupStatus::Applied);
        const std::vector<Bar> bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        // Setup: the book spilled, its first spill opened scratch files this
        // process holds, the fault was injected into them, and the last bar's
        // callback asked for the hash.
        CHECK(host.rejected == 0);
        CHECK(host.book == kRows);
        CHECK(host.book > kResidentRows);
        CHECK(!host.spill_files.empty());
        CHECK(host.injected);
        CHECK(host.asked);
        // The hash cut short by the failed read is never answered.
        CHECK(!host.hash_answered);
        if (swallow) CHECK(host.swallowed);
        // The run ends with the typed failure, not status 0.
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        const RunFailureValue failure = run_failure_value_of(host);
        if (failure.code != RunFailureCode::scratch_io_failed) {
            std::fprintf(stderr, "run failure %s: %s\n", run_failure_code_name(failure.code),
                         host.last_error().c_str());
        }
        CHECK(failure.code == RunFailureCode::scratch_io_failed);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "broker state hash read failure: %s\n", error.what());
    }
    std::printf("test_broker_state_hash_spilled_read_failure%s: %d checks, %d failures\n",
                swallow ? " --swallowed" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
