// 1.7.0 review B1-04: a read of the spilled book that fails inside the
// noexcept continuation hash does not end the process. The fold is noexcept
// (it feeds broker_state_hash and the per-bar capture) and walks the whole
// live book; a spilled row is read back from the book's scratch files, and
// such a read throws on an I/O error. Before the fix that throw met noexcept
// and std::terminate ended the process. The run must instead end failed with
// the typed scratch failure the rest of the release uses, scratch_io_failed.
//
// Fault injection is the seam test_disk_id_table uses: the scratch files are
// unlinked regular files this process holds open (file mode, no arena), and
// truncating one to zero bytes makes every later positional read of it short,
// which ScratchFile reports as EIO. The files the book's first spill opens are
// found as the descriptors that appear across the submit that makes it (the
// 4,097th live row; kBookMax = 4,096 in src/native_order.cpp). Only those
// descriptors are touched: the test opens no path and follows no file name.
//
// The run: bar 1 places 4,200 resting buy limits (the book spills); bar 2's
// callback truncates the book's spill files and then asks the kernel for the
// continuation hash, whose walk of the book meets the failed read.
//
// Source-free, and only the public NativeStrategyHost API of the previous
// epoch, so this TU compiles against the unfixed head; there the hash's
// noexcept ends the process with std::terminate (the row fails by abort).
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    std::vector<int> spill_files;     // the descriptors the book's first spill opened
    std::size_t book = 0;
    std::size_t rejected = 0;
    bool injected = false;
    bool hash_answered = false;       // the hash returned to the callback after the fault

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
        if (bar_ == 2 && !spill_files.empty()) {
            // From here on every read of the book's spill files is short.
            for (const int fd : spill_files) {
                if (::ftruncate(fd, 0) == 0) injected = true;
            }
            // The noexcept fold walks the spilled book and meets the failed
            // read. Fixed, the failure comes back to this caller, typed.
            (void) native_continuation_hash();
            hash_answered = true;
        }
    }

private:
    int bar_ = 0;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    std::vector<Bar> bars;
    for (int index = 0; index != 3; ++index) {
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
    spec.identity = {"b1-04-hash-read-failure", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:B104";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    return spec;
}

}  // namespace

int main() {
    try {
        FaultHost host;
        const auto setup = host.configure_native(spec());
        CHECK(setup.status == NativeSetupStatus::Applied);
        const std::vector<Bar> bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        // Setup: the book spilled, and its first spill opened scratch files
        // this process holds; the fault was injected into them.
        CHECK(host.rejected == 0);
        CHECK(host.book == kRows);
        CHECK(host.book > kResidentRows);
        CHECK(!host.spill_files.empty());
        CHECK(host.injected);
        // The process is still here. The failed read came back to the reader
        // that asked for the hash, and the run ended with the typed failure.
        CHECK(!host.hash_answered);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        const RunFailureValue failure = run_failure_value_of(host);
        if (failure.code != RunFailureCode::scratch_io_failed) {
            std::fprintf(stderr, "run failure %s: %s\n", run_failure_code_name(failure.code),
                         host.last_error().c_str());
        }
        CHECK(failure.code == RunFailureCode::scratch_io_failed);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "continuation hash read failure: %s\n", error.what());
    }
    std::printf("test_continuation_hash_spilled_read_failure: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
