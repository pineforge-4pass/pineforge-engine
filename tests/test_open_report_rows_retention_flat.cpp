// RED witness on the unchanged tree: a run that ends with P
// physical lots open must not retain one report row per lot.
//
// This fixture deliberately uses NO new report API, so it compiles and runs on
// the pre-lane tree. It runs the same P-lot fixture twice:
//   * without `report_open_position_at_end`, so no terminal row is produced;
//   * with it, so the kernel produces the report-only rows.
// Every other allocation is the same in both runs (same spec, same tape, same
// lots), so the peak-live-byte difference is exactly what the run retains for
// the terminal rows.
//
// On the unchanged tree that difference is P rows of `Trade` plus their four
// strings (capacity growth can double it), i.e. hundreds of kilobytes at 1,000
// lots. After O1W R12 the producer pins one fixed-size mark and every row is
// rebuilt on demand, so the difference is one transient row's worth. The
// populations are asserted before the bound.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

std::size_t passed = 0;
std::size_t failed = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++passed;                                                           \
        } else {                                                                \
            ++failed;                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__,  \
                         #condition);                                           \
        }                                                                       \
    } while (0)

// 1,000 retained `Trade` rows (plus four strings each, plus vector capacity
// growth) cannot fit under this; one rebuilt row and the fixed mark can.
constexpr std::size_t kRetainedGrowthBytes = 64u * 1024u;
constexpr int kLots = 1000;

constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

std::size_t* g_peak = nullptr;

struct Peak {
    std::size_t bytes = 0;
};

NativeRunSpec hold_spec(bool report_open_at_end) {
    NativeRunSpec spec;
    spec.identity = {"o1w-tradeout-r12-retention", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = report_open_at_end;
    return spec;
}

std::vector<Bar> rising_tape(int bars) {
    std::vector<Bar> tape;
    tape.reserve(static_cast<std::size_t>(bars));
    for (int i = 0; i < bars; ++i) {
        const double base = 100.0 + static_cast<double>(i) * 0.015625;
        tape.push_back(Bar{base, base + 0.5, base - 0.5, base + 0.25, 1000.0,
                           kT0 + i * kMinute});
    }
    return tape;
}

class HoldHost final : public NativeStrategyHost {
public:
    explicit HoldHost(int target) : target_(target) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (submitted_ >= target_) return;
        const auto result = submit({no::Transact{1.0}, "lot", ""});
        if (result.status != no::SubmitStatus::Accepted || !result.handle.has_value()) {
            rejected_ = true;
            return;
        }
        ++submitted_;
    }

    std::size_t open_lots() const { return pyramid_entries_.size(); }
    std::size_t report_rows() const { return range_end_trades_.size(); }
    bool rejected() const { return rejected_; }

private:
    int target_;
    int submitted_ = 0;
    bool rejected_ = false;
};

Peak run_peak(HoldHost& host, bool report_open_at_end) {
    const auto setup = host.configure_native(hold_spec(report_open_at_end));
    CHECK(setup.status == NativeSetupStatus::Applied);
    const auto bars = rising_tape(kLots + 4);
    Peak peak;
    g_peak = &peak.bytes;
    global_allocation::recording = true;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = [](std::size_t size) {
        (void)size;
        if (global_allocation::live_bytes > *g_peak)
            *g_peak = global_allocation::live_bytes;
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    global_allocation::on_recorded = nullptr;
    global_allocation::recording = false;
    g_peak = nullptr;
    CHECK(host.last_error().empty());
    return peak;
}

}  // namespace

int main() {
    HoldHost without(kLots);
    const Peak baseline = run_peak(without, false);
    CHECK(!without.rejected());
    CHECK(without.open_lots() == static_cast<std::size_t>(kLots));  // population
    CHECK(without.report_rows() == 0);

    HoldHost marked(kLots);
    const Peak retained = run_peak(marked, true);
    CHECK(!marked.rejected());
    CHECK(marked.open_lots() == static_cast<std::size_t>(kLots));   // population
    CHECK(marked.report_rows() == static_cast<std::size_t>(kLots)); // logical rows

    const std::size_t growth = retained.bytes > baseline.bytes
        ? retained.bytes - baseline.bytes : 0;
    std::fprintf(stderr,
                 "R12 retention: baseline=%zu marked=%zu growth=%zu bytes for %d lots\n",
                 baseline.bytes, retained.bytes, growth, kLots);
    CHECK(growth <= kRetainedGrowthBytes);
    std::fprintf(stderr, "test_open_report_rows_retention_flat: %zu passed, %zu failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
