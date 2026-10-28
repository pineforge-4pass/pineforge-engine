// SUMMARY_ONLY keeps the logical trade total and the exact
// folds and materializes no TradeC array.
//
// A run with T closed trades used to fill `new TradeC[T]` inside
// fill_trades_section (104 B per row on LP64) before metrics read it. The
// summary route folds the rows in report order instead, one at a time, and
// returns total_trades = T with trades = NULL / trades_len = 0.
//
// The fixture asserts its real population (closed trades) before it asserts
// the bound. FULL is the reference: identical fixture, identical tape, same
// metrics bits (the 768-byte pf_metrics_t compares equal) and same net_profit.
//
// RED before the lane: SUMMARY_ONLY does not exist and the report allocates the
// T-row array; the measured peak live bytes during report construction scales
// with T. GREEN after: a fixed bound independent of T.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/report_mode.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

// The FULL report's array alone is 104 * 100000 = 10.4 MB; a bounded summary
// construction stays far below this, with only the metric folds and one
// transient row alive at a time.
constexpr std::size_t kPeakReportBytes = 256u * 1024u;
constexpr int kTrades = 100000;

constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

std::size_t* g_peak = nullptr;

struct Peak {
    std::size_t bytes = 0;
};

NativeRunSpec close_spec() {
    NativeRunSpec spec;
    spec.identity = {"o1w-tradeout-r13", 1};
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
    // No margin/risk block: every accepted entry/exit is about the trade rows.
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = false;
    return spec;
}

// Distinct entry and exit prices per round trip, so every trade's PnL is a
// non-zero value and a fold that ignored its input cannot pass.
std::vector<Bar> round_trip_tape(int trades) {
    // One round trip needs two bars at the established cadence; the slack is
    // for a slower fill engine, so the fixture reaches its target population
    // instead of stopping short (the target is asserted, not assumed).
    const int bars = 2 * trades + 256;
    std::vector<Bar> tape;
    tape.reserve(static_cast<std::size_t>(bars));
    for (int i = 0; i < bars; ++i) {
        const double base = 100.0 + static_cast<double>(i % 61) * 0.125
                          + static_cast<double>(i) * 0.0001;
        tape.push_back(Bar{base, base + 0.25, base - 0.25, base + 0.125, 1000.0,
                           kT0 + i * kMinute});
    }
    return tape;
}

// Self-synchronizing: close whatever the book holds, otherwise open the next
// unit. The cadence of the fill engine is not assumed; the fixture asserts the
// accepted and realized populations it actually reached.
class RoundTripHost final : public NativeStrategyHost {
public:
    explicit RoundTripHost(int target) : target_(target) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (position_qty_ != 0.0) {
            (void)submit({no::Flatten{}, "out", ""});
            return;
        }
        if (accepted_entries_ >= target_) return;
        const auto result = submit({no::Transact{1.0}, "in", ""});
        if (result.status == no::SubmitStatus::Accepted && result.handle.has_value())
            ++accepted_entries_;
        else
            rejected_ = true;
    }

    std::size_t closed_trades() const { return trades_.size(); }
    int accepted_entries() const { return accepted_entries_; }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool rejected() const { return rejected_; }

private:
    int target_;
    int accepted_entries_ = 0;
    bool rejected_ = false;
};

template<class Host>
bool run_round_trips(Host& host) {
    const auto setup = host.configure_native(close_spec());
    if (setup.status != NativeSetupStatus::Applied) return false;
    const auto bars = round_trip_tape(kTrades);
    host.run(bars.data(), static_cast<int>(bars.size()));
    return host.last_error().empty();
}

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

Peak measure_report_peak(const BacktestEngine& host, ReportEnvelopeV1& envelope) {
    Peak peak;
    g_peak = &peak.bytes;
    global_allocation::recording = true;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = [](std::size_t size) {
        (void)size;
        if (global_allocation::live_bytes > *g_peak)
            *g_peak = global_allocation::live_bytes;
    };
    const int status = host.fill_report_envelope_v1(&envelope);
    global_allocation::on_recorded = nullptr;
    global_allocation::recording = false;
    g_peak = nullptr;
    CHECK(status == PF_REPORT_MODE_ACCEPTED);
    return peak;
}

}  // namespace

int main() {
    // FULL: the reference report, with its owning arrays.
    RoundTripHost full(kTrades);
    CHECK(run_round_trips(full));
    CHECK(!full.rejected());
    CHECK(full.accepted_entries() == kTrades);                        // population
    CHECK(full.closed_trades() == static_cast<std::size_t>(kTrades)); // population
    CHECK(full.flat());
    ReportC reference{};
    full.fill_report(&reference);
    CHECK(reference.total_trades == kTrades);
    CHECK(reference.trades_len == kTrades);
    CHECK(std::fabs(reference.net_profit) > 0.0);  // the fold saw its input

    // SUMMARY_ONLY: the same run without the row array.
    RoundTripHost summary(kTrades);
    CHECK(summary.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED);
    CHECK(run_round_trips(summary));
    CHECK(!summary.rejected());
    CHECK(summary.accepted_entries() == kTrades);
    CHECK(summary.closed_trades() == static_cast<std::size_t>(kTrades));
    ReportEnvelopeV1 envelope{};
    envelope.metadata.struct_size = sizeof(pf_report_metadata_v1);
    envelope.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    const Peak peak = measure_report_peak(summary, envelope);
    CHECK(envelope.metadata.complete == 1);
    CHECK(envelope.metadata.mode == PF_REPORT_MODE_SUMMARY_ONLY);
    CHECK(envelope.metadata.trades.logical_count == kTrades);
    CHECK(envelope.metadata.trades.captured == 0);
    CHECK(envelope.metadata.trades.external == 0);
    CHECK(envelope.report.total_trades == kTrades);  // logical total
    CHECK(envelope.report.trades == nullptr);        // no TradeC array
    CHECK(envelope.report.trades_len == 0);
    CHECK(envelope.report.equity_curve == nullptr);  // no resident curve
    CHECK(envelope.report.equity_curve_len == 0);
    CHECK(reference.equity_curve_len > 0);
    CHECK(envelope.metadata.equity_curve.logical_count == reference.equity_curve_len);
    CHECK(envelope.metadata.equity_curve.captured == 0);

    // Exact equivalence with the owning-array route: values, not a redefinition.
    CHECK(same_bits(envelope.report.net_profit, reference.net_profit));
    CHECK(std::memcmp(&envelope.report.metrics, &reference.metrics,
                      sizeof(pf_metrics_t)) == 0);

    std::fprintf(stderr, "R13 trades=%d peak=%zu bytes\n", kTrades, peak.bytes);
    CHECK(peak.bytes <= kPeakReportBytes);

    BacktestEngine::free_report(&envelope.report);
    BacktestEngine::free_report(&reference);
    std::fprintf(stderr, "test_report_trades_summary_flat: %zu passed, %zu failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
