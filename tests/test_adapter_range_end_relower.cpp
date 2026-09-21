#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R5 audit lane Q6 — the range-end report rows (first-round duplicate D5,
// design §1.8 RP5) measured: what the kernel's own producer now answers for
// both sides, and what TradingView's range-end REPORT SHAPE keeps on top of
// it in the adapter.
//
// The duplicate the second audit named was the row loop: the kernel's
// NativeExecutionConsumer::record_open_position_report_rows and the adapter's
// PineStrategyHost::scheduler_record_range_end each ran their own
// build_close_trade_with_costs loop with the same open_at_end = true. That
// loop is now ONE generic producer,
// NativeExecutionConsumer::append_open_position_report_rows, which both call;
// the rows below are the executed proof that both sides get the same rows out
// of it.
//
// What is RETAINED in the adapter, and measured here, is everything around
// that loop. TradingView's range-end report is not a mark-to-market row: it
// re-marks the equity curve's last point off the NET row P&L, re-folds every
// extreme from the re-marked curve, re-sorts the same-bar bracket exits and
// suppresses the owner's exit-bar path fold for a projection — and it does
// all of that at THREE marks on the terminal bar, not once at run end. None
// of that is a generic capability a bare host wants, so it stays adapter
// policy; each scenario below names the divergence it causes.
//
// Two halves, one tape per scenario:
//
//   * ADAPTER: a PineStrategyHost driven the way generated code drives it.
//     Every adapter expectation is a DIFFERENTIAL literal harvested from the
//     adapter as it stood on this lane's base, origin/main b8e7976e
//     ("Integration: gap wave B on gap wave A ..."), by compiling this same
//     translation unit against that library and running it with PF_DUMP=1.
//     The adapter after this lane must reproduce them bit for bit — that is
//     this TU's byte-identity proof for the re-lowering, beside the corpus.
//
//   * KERNEL: a bare NativeStrategyHost on the same tape with
//     report_policy = KernelRecorded and report_open_position_at_end = true,
//     i.e. the producer the adapter can never select. Its outcome is pinned
//     too, and each scenario asserts the exact way the two differ.
//
// Scenario keys name the rows of docs/design/native-feature-parity.md §3.7:
//   RE1 the rows themselves, RE2 the equity re-mark, RE3 the extreme
//   re-fold, RE4 the flat control, SW the structural witness (Pine's
//   projected spec cannot reach the kernel producer).
//
// Hand arithmetic throughout: point value 1, account fx 1, slippage 0,
// capital 100000, 100 units a lot, so every number below is exact in binary.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;
bool dumping = false;

#define CHECK(expr) do {                                                       \
    if (expr) ++passed; else {                                                 \
        ++failed; std::printf("FAIL %d %s\n", __LINE__, #expr);                \
    }                                                                          \
} while (0)

// 2025-03-31 00:00 UTC, hourly.
constexpr std::int64_t kT0 = 1743379200000LL;
constexpr std::int64_t kHour = 3600000LL;

Bar bar(int hour, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, kT0 + static_cast<std::int64_t>(hour) * kHour};
}
Bar flat(int hour, double price) { return bar(hour, price, price, price, price); }

struct Tape {
    std::vector<Bar> bars;
    explicit Tape(int hours, double price = 100.0) {
        for (int h = 0; h < hours; ++h) bars.push_back(flat(h, price));
    }
    Tape& at(int hour, double price) { bars[static_cast<std::size_t>(hour)] = flat(hour, price); return *this; }
};

// The whole observable range-end outcome as one canonical string: every
// report row the run ends with (the closed rows first, then the range-end
// rows the producer appended), the equity curve's LAST point, and the four
// extremes the report reads. A harvested literal therefore pins the rows and
// the report shape at once.
std::string state_text(const BacktestEngine& engine, const std::vector<pf_equity_point_t>& curve,
                       double max_eq, double min_eq, double max_dd, double max_ru,
                       int closed) {
    char buf[512];
    std::string out;
    std::snprintf(buf, sizeof buf, "closed=%d report=%d | ", closed, engine.report_trade_count());
    out += buf;
    for (int i = 0; i < engine.report_trade_count(); ++i) {
        const Trade& t = engine.get_report_trade(i);
        std::snprintf(buf, sizeof buf,
                      "%s %.17g @%lld:%.17g->%lld:%.17g pnl=%.17g comm=%.17g ru=%.17g dd=%.17g oae=%d | ",
                      t.is_long ? "L" : "S", t.qty,
                      static_cast<long long>((t.entry_time - kT0) / kHour), t.entry_price,
                      static_cast<long long>((t.exit_time - kT0) / kHour), t.exit_price,
                      t.pnl, t.commission, t.max_runup, t.max_drawdown, t.open_at_end ? 1 : 0);
        out += buf;
    }
    if (curve.empty()) {
        out += "curve=empty | ";
    } else {
        const pf_equity_point_t& last = curve.back();
        std::snprintf(buf, sizeof buf, "last[%lld] eq=%.17g op=%.17g | ",
                      static_cast<long long>((last.time_ms - kT0) / kHour),
                      last.equity, last.open_profit);
        out += buf;
    }
    std::snprintf(buf, sizeof buf, "maxeq=%.17g mineq=%.17g maxdd=%.17g maxru=%.17g",
                  max_eq, min_eq, max_dd, max_ru);
    out += buf;
    return out;
}

// ----------------------------------------------------------------- ADAPTER
// Fixed quantity, process_orders_on_close so a market entry fills at its own
// bar's close, pyramiding 2 so a scenario can carry two physical lots into
// the range end.
class Probe : public source::PineStrategyHost {
public:
    explicit Probe(double commission_percent) {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
        pyramiding_ = 2;
    }
    std::string outcome() const {
        return state_text(*this, equity_curve_, max_equity_, min_equity_,
                          max_drawdown_, max_runup_, static_cast<int>(trades_.size()));
    }
    int hour() const { return pine_bar_index(); }
};

// RE1/RE2/RE3: two lots opened at the closes of bars 0 and 2, both carried
// into the range end.
class TwoLots final : public Probe {
public:
    explicit TwoLots(double fee) : Probe(fee) {}
    void on_source_bar(const Bar&) override {
        if (hour() == 0) strategy_entry("L1", true);
        if (hour() == 2) strategy_entry("L2", true);
    }
};

// RE4: the flat control. The lot is closed before the feed ends, so neither
// side has a position to report and the adapter's re-mark never runs.
class FlatAtEnd final : public Probe {
public:
    FlatAtEnd() : Probe(0.0) {}
    void on_source_bar(const Bar&) override {
        if (hour() == 0) strategy_entry("L1", true);
        if (hour() == 2) strategy_close("L1");
    }
};

template <typename HostT, typename... A>
std::string run_adapter(const Tape& tape, A&&... args) {
    HostT host(static_cast<A&&>(args)...);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    if (!host.last_error().empty()) return "error: " + host.last_error();
    return host.outcome();
}

// ------------------------------------------------------------------ KERNEL
// The bare host: the requests it makes at each hour, and the kernel's own
// range-end producer selected through the spec.
struct KernelHost final : NativeStrategyHost {
    std::vector<std::vector<no::Request>> plan;
    int hour = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int h = hour++;
        if (h < static_cast<int>(plan.size())) {
            for (const auto& request : plan[static_cast<std::size_t>(h)]) (void)submit(request);
        }
    }
    std::string outcome() const {
        return state_text(*this, equity_curve_, max_equity_, min_equity_,
                          max_drawdown_, max_runup_, static_cast<int>(trades_.size()));
    }
};

NativeRunSpec kernel_spec(const char* key, double fee_percent) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "60";
    s.script_tf = "60";
    s.tickerid = "TEST:Q6";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::Percent;
    s.fee_value = fee_percent;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    // The producer the adapter can never select: its own project() declares
    // KernelRecordedAtHostMarks, which gates record_open_position_report_rows
    // out (src/native_execution_consumer.cpp).
    s.report_policy = NativeReportPolicy::KernelRecorded;
    s.report_open_position_at_end = true;
    return s;
}

no::Request tx(double units, const char* label) { return {no::Transact{units}, label, ""}; }
no::Request flatten(const char* label) { return {no::Flatten{}, label, ""}; }

std::string run_kernel(const NativeRunSpec& s, const Tape& tape,
                       std::vector<std::vector<no::Request>> plan) {
    KernelHost host;
    host.plan = std::move(plan);
    if (host.configure_native(s).status != NativeSetupStatus::Applied)
        return "configure_native refused";
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    if (!host.last_error().empty()) return "error: " + host.last_error();
    return host.outcome();
}

std::vector<std::vector<no::Request>> plan_of(int hours) {
    return std::vector<std::vector<no::Request>>(static_cast<std::size_t>(hours));
}

// -------------------------------------------------------------- reporting
struct Expected {
    const char* name;
    const char* adapter;  // harvested from b8e7976e
    const char* kernel;   // the kernel producer's own outcome on the same tape
};

void verify(const Expected& e, const std::string& adapter, const std::string& kernel) {
    if (dumping) {
        std::printf("DUMP %s adapter=%s\n", e.name, adapter.c_str());
        std::printf("DUMP %s kernel=%s\n", e.name, kernel.c_str());
        return;
    }
    if (adapter != e.adapter)
        std::printf("FAIL %s adapter:\n  got      %s\n  expected %s\n",
                    e.name, adapter.c_str(), e.adapter);
    CHECK(adapter == e.adapter);
    if (kernel != e.kernel)
        std::printf("FAIL %s kernel:\n  got      %s\n  expected %s\n",
                    e.name, kernel.c_str(), e.kernel);
    CHECK(kernel == e.kernel);
}

} // namespace

#include "adapter_range_end_relower_expectations.inc"

int main() {
    dumping = std::getenv("PF_DUMP") != nullptr;

    // RE1. No fee. Two lots carried into the range end: 100 @100 and 100 @102,
    // marked at bar 3's close of 105.
    {
        Tape t(4);
        t.at(2, 102.0).at(3, 105.0);
        auto plan = plan_of(4);
        plan[0].push_back(tx(100.0, "L1"));
        plan[2].push_back(tx(100.0, "L2"));
        verify(kRE1, run_adapter<TwoLots>(t, 0.0), run_kernel(kernel_spec("RE1", 0.0), t, plan));
    }

    // RE2. The same tape with a 0.1 % commission. The rows are net of the
    // round trip on both sides; the adapter's equity re-mark then carries
    // that net into the curve's last point, where the run's own mark had the
    // GROSS open profit.
    {
        Tape t(4);
        t.at(2, 102.0).at(3, 105.0);
        auto plan = plan_of(4);
        plan[0].push_back(tx(100.0, "L1"));
        plan[2].push_back(tx(100.0, "L2"));
        verify(kRE2, run_adapter<TwoLots>(t, 0.1), run_kernel(kernel_spec("RE2", 0.1), t, plan));
    }

    // RE3. A dip before the range end, with the fee on: the adapter resets
    // the four extremes and re-folds them from the re-marked curve, so its
    // drawdown/runup answer the re-marked last point; the kernel leaves the
    // extremes exactly as the run sampled them.
    {
        Tape t(6);
        t.at(1, 90.0).at(2, 102.0).at(3, 108.0).at(4, 96.0).at(5, 105.0);
        auto plan = plan_of(6);
        plan[0].push_back(tx(100.0, "L1"));
        plan[2].push_back(tx(100.0, "L2"));
        verify(kRE3, run_adapter<TwoLots>(t, 0.1), run_kernel(kernel_spec("RE3", 0.1), t, plan));
    }

    // RE4. The flat control: the position is closed at bar 2, so no range-end
    // row exists on either side and the adapter's re-mark never runs.
    {
        Tape t(4);
        t.at(2, 102.0).at(3, 105.0);
        auto plan = plan_of(4);
        plan[0].push_back(tx(100.0, "L1"));
        plan[2].push_back(flatten("L1"));
        verify(kRE4, run_adapter<FlatAtEnd>(t), run_kernel(kernel_spec("RE4", 0.0), t, plan));
    }

    // SW. The structural witness: Pine's projected spec declares
    // KernelRecordedAtHostMarks and leaves report_open_position_at_end false,
    // so the kernel's range-end producer is gated out of every adapter run —
    // the reason the adapter keeps a range-end function at all.
    {
        Tape t(4);
        t.at(2, 102.0).at(3, 105.0);
        TwoLots host(0.0);
        host.run(t.bars.data(), static_cast<int>(t.bars.size()));
        CHECK(host.last_error().empty());
        const auto state = host.native_state();
        CHECK(state.spec != nullptr);
        if (state.spec) {
            CHECK(state.spec->report_policy == NativeReportPolicy::KernelRecordedAtHostMarks);
            CHECK(state.spec->report_open_position_at_end == false);
        }
    }

    if (!dumping) {
        std::printf("%s: %d passed, %d failed\n",
                    failed ? "FAILED" : "PASSED", passed, failed);
    }
    return failed ? 1 : 0;
}
