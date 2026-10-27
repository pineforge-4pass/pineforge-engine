// CR-150 O1W R12: the range-end report rows are produced one at a time.
//
// A completed run with P physical lots still open reports one report-only row
// per lot. Before this lane the producer appended every row to
// `range_end_trades_` (a std::vector<Trade>, four strings per row), so the
// engine's resident report storage grew with P. The rows are now rebuilt from
// the frozen terminal mark and the live lot, and SUMMARY_ONLY assembles the
// report without an owning TradeC array.
//
// The fixture asserts its real population (open lots) before it asserts the
// bound. The reference row is an independent copy of the old producer loop:
// the same non-mutating close builder, called with the same terminal facts, in
// the same lot order. The FULL report's rows must equal it, and the
// SUMMARY_ONLY fold and metrics must equal the FULL report bit for bit.
//
// RED before the lane: SUMMARY_ONLY does not exist and the producer retains P
// rows with their strings, so the peak live bytes during report construction
// grows with P. GREEN after: a fixed bound independent of P.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "equity_writer_test_access.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/report_mode.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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

// A per-lot row copy cannot fit under this: one TradeC row is 104 B (LP64)
// and a Trade carries four strings, so even 1,000 retained rows exceed it,
// while the single row being rebuilt at a time does not.
constexpr std::size_t kPeakReportBytes = 256u * 1024u;

constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

std::size_t* g_peak = nullptr;

// Progress for the long populations: one stderr line every 10,000 steps
// (population, step, elapsed since the program started), so a run that is slow
// can be told from a run that has stopped advancing. It reads no engine state.
const std::chrono::steady_clock::time_point g_progress_start = std::chrono::steady_clock::now();
constexpr long kProgressEvery = 10000;
void progress(const char* phase, int population, long step) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_progress_start).count();
    std::fprintf(stderr, "R12 progress phase=%s population=%d step=%ld elapsed_ms=%lld\n",
                 phase, population, step, static_cast<long long>(elapsed));
}

struct Peak {
    std::size_t bytes = 0;
};

NativeRunSpec hold_spec() {
    NativeRunSpec spec;
    spec.identity = {"o1w-tradeout-r12", 1};
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
    // No margin/risk block at all: the fixture is about report rows, not
    // admission, and the default spec surface opens without a gate.
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    return spec;
}

// A rising tape with a distinct open and close, so each lot's mark PnL is a
// non-zero, distinct value and a fold that ignored its input cannot pass.
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
        if (++bars_seen_ % kProgressEvery == 0) progress("bar", target_, bars_seen_);
        if (submitted_ >= target_) return;
        const auto result = submit({no::Transact{1.0}, "lot", ""});
        if (result.status != no::SubmitStatus::Accepted || !result.handle.has_value()) {
            rejected_ = true;
            return;
        }
        ++submitted_;
    }

    std::size_t open_lots() const { return pyramid_entries_.size(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool rejected() const { return rejected_; }

    // The independent reference: the old producer loop, restated here from the
    // terminal facts the kernel records. It shares only the non-mutating close
    // builder every close uses; no lazy-source code is called.
    Trade reference_row(std::size_t index) const {
        const PyramidEntry& lot = pyramid_entries_[index];
        const double mark_price = bar_fill_price(current_bar_.close);
        const std::int64_t mark_time =
            equity_writer_test::last(*this) ? equity_writer_test::last(*this)->time_ms
                                          : current_bar_.timestamp;
        const double fx = active_account_currency_fx();
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = mark_time;
        context.interval_index = bar_index_;
        context.account_fx = fx;
        if (!std::isnan(fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = fold_exit_trail_peak_;
        Trade row = build_close_trade_with_costs(
            lot, lot.qty, mark_price, position_side_ == PositionSide::LONG,
            allocated_entry_commission(lot, lot.qty),
            calc_commission_at(mark_price, lot.qty, fx), context);
        row.open_at_end = true;
        return row;
    }

private:
    int target_;
    long bars_seen_ = 0;
    int submitted_ = 0;
    bool rejected_ = false;
};

template<class Host>
bool run_hold(Host& host, int target) {
    const auto setup = host.configure_native(hold_spec());
    if (setup.status != NativeSetupStatus::Applied) return false;
    const auto bars = rising_tape(target + 4);
    host.run(bars.data(), static_cast<int>(bars.size()));
    return host.last_error().empty();
}

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// Compare one report row's numeric view with the independent reference row.
void check_row(const TradeC& row, const Trade& expected) {
    CHECK(row.entry_time == expected.entry_time);
    CHECK(row.exit_time == expected.exit_time);
    CHECK(same_bits(row.entry_price, expected.entry_price));
    CHECK(same_bits(row.exit_price, expected.exit_price));
    CHECK(same_bits(row.qty, expected.qty));
    CHECK(same_bits(row.pnl, expected.pnl));
    CHECK(same_bits(row.pnl_pct, expected.pnl_pct));
    CHECK(row.is_long == (expected.is_long ? 1 : 0));
    CHECK(row.entry_bar_index == expected.entry_bar_index);
    CHECK(row.exit_bar_index == expected.exit_bar_index);
    CHECK(row.open_at_end == 1);
    CHECK(same_bits(row.max_runup, expected.max_runup));
    CHECK(same_bits(row.max_drawdown, expected.max_drawdown));
    CHECK(same_bits(row.commission, expected.commission));
    CHECK(expected.open_at_end);
}

// The report-indexed identity row (the legacy borrowed-pointer accessor).
void check_identity(const Trade& identity, const Trade& expected) {
    CHECK(identity.entry_incarnation == expected.entry_incarnation);
    CHECK(identity.entry_id == expected.entry_id);
    CHECK(identity.exit_id == expected.exit_id);
    CHECK(identity.exit_comment == expected.exit_comment);
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

// FULL: the rows themselves, against the independent old loop.
void full_population(int lots) {
    HoldHost full(lots);
    CHECK(run_hold(full, lots));
    CHECK(!full.rejected());
    CHECK(full.open_lots() == static_cast<std::size_t>(lots));  // real population
    CHECK(!full.flat());
    ReportC report{};
    full.fill_report(&report);
    CHECK(report.total_trades == lots);
    CHECK(report.trades_len == lots);
    if (report.trades_len == lots) {
        // Identity strings travel through the legacy accessor; checking a
        // bounded prefix is enough to pin the boundary without retaining a
        // materialized row per lot in the fixture itself.
        const int identity_rows = std::min(lots, 256);
        for (int i = 0; i < lots; ++i) {
            if (i != 0 && i % kProgressEvery == 0) progress("full_rows", lots, i);
            const auto index = static_cast<std::size_t>(i);
            const Trade expected = full.reference_row(index);
            check_row(report.trades[i], expected);
            if (i < identity_rows) check_identity(full.get_report_trade(i), expected);
        }
    }
    BacktestEngine::free_report(&report);
}

// SUMMARY_ONLY: the logical total and the exact folds, no row array.
void summary_population(int lots) {
    HoldHost summary(lots);
    CHECK(summary.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED);
    CHECK(run_hold(summary, lots));
    CHECK(!summary.rejected());
    CHECK(summary.open_lots() == static_cast<std::size_t>(lots));
    ReportEnvelopeV1 envelope{};
    envelope.metadata.struct_size = sizeof(pf_report_metadata_v1);
    envelope.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    const Peak peak = measure_report_peak(summary, envelope);
    CHECK(envelope.metadata.complete == 1);
    CHECK(envelope.metadata.mode == PF_REPORT_MODE_SUMMARY_ONLY);
    CHECK(envelope.metadata.trades.logical_count == lots);
    CHECK(envelope.metadata.trades.captured == 0);
    CHECK(envelope.metadata.trades.external == 0);
    CHECK(envelope.report.total_trades == lots);  // logical, not resident
    CHECK(envelope.report.trades == nullptr);
    CHECK(envelope.report.trades_len == 0);

    // The exact fold: the same left fold the old producer and the FULL report
    // accumulate, over the reference rows in lot order.
    double expected_net = 0.0;
    for (int i = 0; i < lots; ++i) {
        if (i != 0 && i % kProgressEvery == 0) progress("summary_rows", lots, i);
        expected_net += summary.reference_row(static_cast<std::size_t>(i)).pnl;
    }
    CHECK(same_bits(envelope.report.net_profit, expected_net));
    CHECK(std::fabs(envelope.report.net_profit) > 0.0);  // the fold saw its input

    // The bound. The terminal rows must not be retained: one rebuilt row and
    // the fixed folds are the only live allocation at report construction.
    std::fprintf(stderr, "R12 lots=%d peak=%zu bytes\n", lots, peak.bytes);
    CHECK(peak.bytes <= kPeakReportBytes);
    BacktestEngine::free_report(&envelope.report);
}

void summary_matches_full_bits() {
    constexpr int lots = 64;
    HoldHost full(lots);
    CHECK(run_hold(full, lots));
    ReportC reference{};
    full.fill_report(&reference);

    HoldHost summary(lots);
    CHECK(summary.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == PF_REPORT_MODE_ACCEPTED);
    CHECK(run_hold(summary, lots));
    ReportEnvelopeV1 envelope{};
    envelope.metadata.struct_size = sizeof(pf_report_metadata_v1);
    envelope.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    CHECK(summary.fill_report_envelope_v1(&envelope) == PF_REPORT_MODE_ACCEPTED);
    CHECK(envelope.report.total_trades == reference.total_trades);
    CHECK(same_bits(envelope.report.net_profit, reference.net_profit));
    CHECK(std::memcmp(&envelope.report.metrics, &reference.metrics,
                      sizeof(pf_metrics_t)) == 0);
    BacktestEngine::free_report(&envelope.report);
    BacktestEngine::free_report(&reference);
}

enum class ReportBranch { FULL, SUMMARY_ONLY, FULL_SUMMARY_BITS };

struct PopulationBranch {
    ReportBranch mode;
    int lots;
    int slice;
};

constexpr int kSliceCount = 4;
// This is the execution population, in the original no-argument order.
// Keep each large population whole, including its ordered reference fold.
constexpr PopulationBranch kPopulationBranches[] = {
    {ReportBranch::FULL, 1, 1},
    {ReportBranch::SUMMARY_ONLY, 1, 3},
    {ReportBranch::FULL, 1000, 1},
    {ReportBranch::SUMMARY_ONLY, 1000, 3},
    {ReportBranch::FULL, 100000, 2},
    {ReportBranch::SUMMARY_ONLY, 100000, 4},
    {ReportBranch::FULL_SUMMARY_BITS, 64, 3},
};

}  // namespace

int main(int argc, char** argv) {
    int slice = 0;  // No argument runs the complete original population.
    if (argc != 1) {
        if (argc != 3 || std::strcmp(argv[1], "--slice") != 0
                || std::strlen(argv[2]) != 3
                || argv[2][0] < '1' || argv[2][0] > '0' + kSliceCount
                || argv[2][1] != '/' || argv[2][2] != '0' + kSliceCount) {
            std::fprintf(stderr, "usage: %s [--slice I/%d], 1 <= I <= %d\n",
                         argv[0], kSliceCount, kSliceCount);
            return 2;
        }
        slice = argv[2][0] - '0';
    }

    std::size_t selected = 0;
    for (const auto& branch : kPopulationBranches) {
        if (slice != 0 && branch.slice != slice) continue;
        ++selected;
        const std::size_t passed_before = passed;
        const std::size_t failed_before = failed;
        const char* mode = branch.mode == ReportBranch::FULL ? "FULL"
            : branch.mode == ReportBranch::SUMMARY_ONLY ? "SUMMARY_ONLY" : "FULL_SUMMARY_BITS";
        std::fprintf(stderr, "test_open_report_rows_flat: slice=%d/%d mode=%s lots=%d begin\n",
                     branch.slice, kSliceCount, mode, branch.lots);
        switch (branch.mode) {
        case ReportBranch::FULL:
            full_population(branch.lots);
            break;
        case ReportBranch::SUMMARY_ONLY:
            summary_population(branch.lots);
            break;
        case ReportBranch::FULL_SUMMARY_BITS:
            CHECK(branch.lots == 64);
            summary_matches_full_bits();
            break;
        }
        std::fprintf(stderr,
                     "test_open_report_rows_flat: slice=%d/%d mode=%s lots=%d "
                     "passed=%zu failed=%zu\n",
                     branch.slice, kSliceCount, mode, branch.lots,
                     passed - passed_before, failed - failed_before);
    }
    if (selected == 0) {
        std::fprintf(stderr, "test_open_report_rows_flat: empty slice\n");
        return 2;
    }
    std::fprintf(stderr, "test_open_report_rows_flat: %zu passed, %zu failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
