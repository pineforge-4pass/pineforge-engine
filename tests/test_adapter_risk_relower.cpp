#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R5 audit lane N12 — the adapter's TradingView strategy.risk.* rules measured
// against the kernel's generic risk limits (L9, NativeRunSpec::risk).
//
// Two halves, one tape per scenario:
//
//   * ADAPTER: a PineStrategyHost that applies the strategy.risk.* rule the
//     way generated code does — as a statement inside on_source_bar, every
//     bar — and whose outcome is the report rows plus the final book. Every
//     adapter expectation is a DIFFERENTIAL literal harvested from the
//     adapter as it stood on main 683a82f ("Docs: the subscription pump's
//     chronology against the script interval (R5 lane L6d, item 3)"), the
//     tree every R5 lane merged into and the base of this lane, by compiling
//     this same translation unit against that library and running it with
//     PF_DUMP=1. The adapter after this lane must reproduce them bit for bit.
//
//   * KERNEL: a bare NativeStrategyHost driving the kernel through the
//     closest NativeRiskLimits declaration on the same tape. Its outcome is
//     pinned too, and each scenario asserts the exact way the two differ.
//     Those differences are the measurement behind every "retained, why"
//     entry of docs/design/native-feature-parity.md §3.6 and the "Risk
//     limits" section of docs/pages/native-engine.md.
//
// Scenario keys name the §1.4 rows: MG10 drawdown, MG11 consecutive loss
// days, MG12 intraday loss, MG13 filled orders per day, MG14 the risk day,
// PS the max position size gate. SW is the structural witness: Pine's risk
// statements reach the adapter after configure_native has already fixed and
// digested the run spec, so the kernel ledger stays inert under every one of
// them (native_risk_state() never has a day).
//
// Hand arithmetic throughout: point value 1, account fx 1, no fee, no
// slippage, capital 100000, so every equity below is exact in binary.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;
bool dumping = false;

#define CHECK(expr) do {                                                       \
    if (expr) ++passed; else {                                                 \
        ++failed; std::printf("FAIL %d %s\n", __LINE__, #expr);                \
    }                                                                          \
} while (0)

// 2025-03-31 00:00 UTC: the tape starts at a UTC midnight so the calendar day
// rolls at bar 24, 48, 72 of an hourly tape.
constexpr std::int64_t kT0 = 1743379200000LL;
constexpr std::int64_t kHour = 3600000LL;

Bar bar(int hour, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, kT0 + static_cast<std::int64_t>(hour) * kHour};
}
Bar flat(int hour, double price) { return bar(hour, price, price, price, price); }

// The first `hours` bars of a tape, flat at `price` unless overridden.
struct Tape {
    std::vector<Bar> bars;
    explicit Tape(int hours, double price = 100.0) {
        for (int h = 0; h < hours; ++h) bars.push_back(flat(h, price));
    }
    Tape& at(int hour, double price) { bars[static_cast<std::size_t>(hour)] = flat(hour, price); return *this; }
    Tape& at(int hour, double o, double h, double l, double c) {
        bars[static_cast<std::size_t>(hour)] = bar(hour, o, h, l, c); return *this;
    }
};

// One closed report row, and the whole observable outcome of a run, spelled
// as one canonical string so a harvested literal pins everything at once:
// every row's side, units, entry/exit hour, entry/exit price, P&L, exit id,
// exit comment and close cause, then the final signed book.
std::string row_text(const Trade& row, int cause) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s %.17g @%lld:%.17g->%lld:%.17g pnl=%.17g id='%s' c='%s' cause=%d",
                  row.is_long ? "L" : "S", row.qty,
                  static_cast<long long>((row.entry_time - kT0) / kHour), row.entry_price,
                  static_cast<long long>((row.exit_time - kT0) / kHour), row.exit_price,
                  row.pnl, row.exit_id.c_str(), row.exit_comment.c_str(), cause);
    return buf;
}

std::string outcome_text(const BacktestEngine& engine, double position) {
    std::string out;
    for (int i = 0; i < engine.report_trade_count(); ++i) {
        out += row_text(engine.get_report_trade(i), engine.closed_trade_close_cause(i));
        out += " | ";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "pos=%.17g", position);
    out += buf;
    return out;
}

// ----------------------------------------------------------------- ADAPTER
// Common configuration: fixed default quantity, no fee, no slippage,
// process_orders_on_close so a market entry fills at its own bar's close.
class Probe : public source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    std::string outcome() const { return outcome_text(*this, physical_position().signed_units); }
    int hour() const { return pine_bar_index(); }
};

// MG10-a. The drawdown limit's sampling cadence. The adapter measures its
// drawdown once per script bar, at the close mark; the kernel also measures
// at the bar's open and after every applied drain. Bar 1 opens 6 points
// under the entry (drawdown 600 against 500) and closes 1 under it.
class DrawdownCadence final : public Probe {
public:
    DrawdownCadence() { pyramiding_ = 2; }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_drawdown(500.0, false);
        if (hour() == 0) strategy_entry("L", true);
        if (hour() == 2) strategy_entry("L", true);
    }
};
Tape drawdown_cadence_tape() { return Tape(4).at(1, 94.0, 100.0, 94.0, 99.0).at(2, 99.0).at(3, 99.0); }

// MG10-b. The halt's scope. TradingView's latch gates a flat or same-side
// entry at its fill and never the opposite entry, so a halted strategy still
// reverses; the kernel's block refuses every request that would open.
class DrawdownReversal final : public Probe {
public:
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_drawdown(500.0, false);
        if (hour() == 0) strategy_entry("L", true);
        if (hour() == 2) strategy_entry("S", false);
        if (hour() == 3) strategy_entry("L", true);
    }
};
Tape drawdown_reversal_tape() { return Tape(5).at(1, 94.0).at(2, 94.0).at(3, 94.0).at(4, 94.0); }

// MG11. The consecutive-loss-days streak. Each "trade" is an entry at one
// bar's close and a close at the next bar's close; the day rolls every 24
// bars. The adapter counts one losing TRADE per new chart day and resets on
// any winning trade at that fill; the kernel settles each day's NET realized
// result when the next day opens.
//
// CL-a: day 1 loses; day 2 wins then loses; day 3 loses. Adapter: 1, then
// 0 -> 1, then 2 = the limit, latched at day 3's second trade. Kernel: day 2
// nets +100 (reset), day 3 nets -100 (streak 1): never blocked.
class LossDaysOrdered final : public Probe {
public:
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_cons_loss_days(2);
        const int h = hour();
        if (h == 0 || h == 24 || h == 26 || h == 48 || h == 50 || h == 72) strategy_entry("L", true);
        if (h == 1 || h == 25 || h == 27 || h == 49 || h == 51 || h == 73) strategy_close("L");
    }
};
Tape loss_days_ordered_tape() {
    Tape t(76);
    t.at(1, 99.0);                 // day 1: -100
    t.at(24, 100.0).at(25, 102.0); // day 2: +200
    t.at(26, 100.0).at(27, 99.0);  //        -100
    t.at(48, 100.0).at(49, 99.0);  // day 3: -100 -> adapter latches here
    t.at(50, 100.0).at(51, 99.0);  //        the adapter refuses this entry
    t.at(72, 100.0).at(73, 101.0); // day 4: +100 if admitted
    return t;
}

// CL-b: the mirror image. Each day loses then wins by less. Adapter: the win
// resets the count every day, never halted. Kernel: each day nets -50, two
// days make the streak 2 = the limit at day 3's open: blocked from then on.
class LossDaysNetted final : public Probe {
public:
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_cons_loss_days(2);
        const int h = hour();
        if (h == 0 || h == 2 || h == 24 || h == 26 || h == 48) strategy_entry("L", true);
        if (h == 1 || h == 3 || h == 25 || h == 27 || h == 49) strategy_close("L");
    }
};
Tape loss_days_netted_tape() {
    Tape t(52);
    t.at(1, 99.0).at(2, 100.0).at(3, 100.5);    // day 1: -100 then +50
    t.at(25, 99.0).at(26, 100.0).at(27, 100.5); // day 2: -100 then +50
    t.at(49, 99.0);                              // day 3: -100 if admitted
    return t;
}

// MG12-a. The intraday loss checked along the bar's path. A long 100 @ 100
// carried into day 2; the day's first bar opens at 100, trades down to 85 and
// closes at 95. The adapter measures the loss at the adverse extreme (1500
// against 1000) and rests its forced close there; the kernel measures at the
// open (0) and the close (500) and sees no breach.
class IntradayPath final : public Probe {
public:
    IntradayPath() { pyramiding_ = 2; }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_loss(1000.0, false);
        if (hour() == 0) strategy_entry("L", true);
        if (hour() == 26) strategy_entry("L", true);  // blocked for the day by TV
        if (hour() == 48) strategy_entry("L", true);  // the next day is open again
    }
};
Tape intraday_path_tape() {
    Tape t(50);
    t.at(24, 100.0, 100.0, 85.0, 95.0);
    for (int h = 25; h < 50; ++h) t.at(h, 95.0);
    return t;
}

// MG12-b. The intraday loss at a bar's open inside the day, plus the
// cancel-pending latch. Day 2 opens at 100 (the day's opening equity marks
// there, on both sides: a gap at the day's first bar is not an intraday
// loss for either) and its second bar opens at 88 (loss 1200 against 1000):
// both sides close the book at 88, under their own ticket. A resting limit
// entry placed on day 1 is withdrawn by the adapter's forced close and left
// working by the kernel, so it fills on day 3 only on the kernel.
class IntradayOpen final : public Probe {
public:
    IntradayOpen() { pyramiding_ = 2; }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_loss(1000.0, false);
        if (hour() == 0) strategy_entry("L", true);
        if (hour() == 23) strategy_entry("L2", true, 80.0);
    }
};
Tape intraday_open_tape() {
    Tape t(50);
    for (int h = 25; h < 48; ++h) t.at(h, 88.0);
    t.at(48, 79.0).at(49, 79.0);
    return t;
}

// MG13. Two filled orders per day, then the forced close. Two one-unit
// entries fill at the closes of bars 0 and 1; the second reaches the limit.
// Bar 1 closes above its open so TradingView books the cap close at that
// bar's HIGH (its "better than the open" rule) while the kernel books its
// own flatten at the point it evaluated, the close. A third entry on the
// same day is refused by both; day 2 is open again for both.
class FillCap final : public Probe {
public:
    FillCap() { default_qty_value_ = 1.0; pyramiding_ = 3; }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_filled_orders(2);
        const int h = hour();
        if (h == 0 || h == 1 || h == 2 || h == 24) strategy_entry("L", true);
        if (h == 25) strategy_close("L");
    }
};
Tape fill_cap_tape() {
    Tape t(26);
    t.at(1, 100.0, 103.0, 99.0, 101.0);
    for (int h = 2; h < 26; ++h) t.at(h, 101.0);
    return t;
}

// MG14. The risk day. The same two fills, on a chart whose timezone is New
// York while the symbol's is UTC: TradingView's cap keys its day on the
// CHART day (04:00 UTC is midnight in New York, so bar 4 is a new day and
// the entry there fills); the kernel keys on the spec timezone's civil date
// and refuses until bar 24.
class FillCapChartDay final : public Probe {
public:
    FillCapChartDay() {
        default_qty_value_ = 1.0;
        pyramiding_ = 4;
        set_chart_timezone("America/New_York");
    }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_filled_orders(2);
        const int h = hour();
        if (h == 0 || h == 1 || h == 4 || h == 24) strategy_entry("L", true);
    }
};
Tape fill_cap_chart_day_tape() { return Tape(26); }

// PS. The position-size gate. The adapter refuses a flat or same-side entry
// at its fill when the LIVE book already holds at least the limit; the
// kernel's max_abs_units refuses a fill whose RESULTING book would exceed
// it. Two-unit entries against a limit of three: the adapter admits the
// second (live 2 < 3, book 4) and refuses the third; the kernel refuses the
// second (4 > 3).
class PositionSize final : public Probe {
public:
    PositionSize() { default_qty_value_ = 2.0; pyramiding_ = 5; }
    void on_source_bar(const Bar&) override {
        set_pine_risk_max_position_size(3.0);
        if (hour() <= 2) strategy_entry("L", true);
    }
};
Tape position_size_tape() { return Tape(4); }

template <typename HostT>
std::string run_adapter(const Tape& tape) {
    HostT host;
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    return host.outcome();
}

// ------------------------------------------------------------------ KERNEL
// The bare host: one rule per script-bar calculation, the requests it makes
// at each hour, the refusals and risk events the kernel reports.
struct KernelHost final : NativeStrategyHost {
    std::vector<std::vector<no::Request>> plan;  // indexed by hour
    std::vector<int> refused;                    // hours of RiskLimit refusals
    std::vector<int> capped;                     // hours of MaxUnits refusals
    int hour = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int h = hour++;
        if (h < static_cast<int>(plan.size())) {
            for (const auto& request : plan[static_cast<std::size_t>(h)]) (void)submit(request);
        }
    }
    std::string outcome() const { return outcome_text(*this, physical_position().signed_units); }
    void collect() {
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* e = std::get_if<no::MatchRejectedEvent>(&*row.command)) {
                const int h = e->cursor.point.interval_index;
                if (e->reason == no::MatchRejectReason::RiskLimit) refused.push_back(h);
                if (e->reason == no::MatchRejectReason::MaxAbsUnits) capped.push_back(h);
            }
        }
    }
};

NativeRunSpec kernel_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    // Reads its whole event record once the run has ended (V19-B).
    s.event_retention = NativeEventRetention::Full;
    s.input_tf = "60";
    s.script_tf = "60";
    s.tickerid = "TEST:N12";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
}

NativeLossLimit loss(double value) { NativeLossLimit l; l.value = value; l.percent = false; return l; }
no::Request tx(double units, const char* label) { return {no::Transact{units}, label, ""}; }
no::Request flatten(const char* label) { return {no::Flatten{}, label, ""}; }
no::Request limit_buy(double units, double price, const char* label) {
    no::Request r{no::Transact{units}, label, ""};
    r.trigger = no::Limit{price};
    return r;
}

struct KernelRun {
    std::string outcome;
    std::vector<int> refused;
    std::vector<int> capped;
    std::size_t risk_events = 0;
    bool blocked_at_end = false;
};

KernelRun run_kernel(const NativeRunSpec& s, const Tape& tape,
                     std::vector<std::vector<no::Request>> plan) {
    KernelHost host;
    host.plan = std::move(plan);
    KernelRun out;
    if (host.configure_native(s).status != NativeSetupStatus::Applied) {
        out.outcome = "configure_native refused";
        return out;
    }
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    if (!host.last_error().empty()) {
        out.outcome = "error: " + host.last_error();
        return out;
    }
    host.collect();
    out.outcome = host.outcome();
    out.refused = host.refused;
    out.capped = host.capped;
    for (const auto& row : host.native_events(0)) {
        if (row.command && std::holds_alternative<no::NativeRiskEvent>(*row.command)) ++out.risk_events;
    }
    out.blocked_at_end = host.native_risk_state().blocked;
    return out;
}

std::vector<std::vector<no::Request>> plan_of(int hours) {
    return std::vector<std::vector<no::Request>>(static_cast<std::size_t>(hours));
}

// -------------------------------------------------------------- reporting
struct Expected {
    const char* name;
    const char* adapter;  // harvested from main 683a82f
    const char* kernel;   // the kernel's own outcome on the same tape
};

void report(const char* name, const std::string& adapter, const KernelRun& kernel) {
    std::printf("DUMP %s adapter=%s\n", name, adapter.c_str());
    std::printf("DUMP %s kernel=%s\n", name, kernel.outcome.c_str());
    std::printf("DUMP %s refused=", name);
    for (int h : kernel.refused) std::printf("%d,", h);
    std::printf(" capped=");
    for (int h : kernel.capped) std::printf("%d,", h);
    std::printf(" risk_events=%zu blocked_at_end=%d\n", kernel.risk_events,
                kernel.blocked_at_end ? 1 : 0);
}

void verify(const Expected& e, const std::string& adapter, const KernelRun& kernel) {
    if (dumping) { report(e.name, adapter, kernel); return; }
    if (adapter != e.adapter) {
        std::printf("FAIL %s adapter:\n  got      %s\n  expected %s\n", e.name, adapter.c_str(), e.adapter);
    }
    CHECK(adapter == e.adapter);
    if (kernel.outcome != e.kernel) {
        std::printf("FAIL %s kernel:\n  got      %s\n  expected %s\n", e.name, kernel.outcome.c_str(), e.kernel);
    }
    CHECK(kernel.outcome == e.kernel);
}

} // namespace

#include "adapter_risk_relower_expectations.inc"

int main() {
    dumping = std::getenv("PF_DUMP") != nullptr;

    // MG10-a: cadence.
    {
        auto s = kernel_spec("n12-dd-cadence");
        NativeRiskLimits limits; limits.max_drawdown = loss(500.0); s.risk = limits;
        auto plan = plan_of(4);
        plan[0] = {tx(100.0, "L")}; plan[2] = {tx(100.0, "L")};
        const auto kernel = run_kernel(s, drawdown_cadence_tape(), plan);
        const auto adapter = run_adapter<DrawdownCadence>(drawdown_cadence_tape());
        verify(kDrawdownCadence, adapter, kernel);
        if (!dumping) {
            // The adapter admits the bar-2 add (book 200); the kernel breached
            // at bar 1's open mark and refuses it (book 100).
            CHECK(adapter.find("pos=200") != std::string::npos);
            CHECK(kernel.outcome.find("pos=100") != std::string::npos);
            CHECK(kernel.refused == std::vector<int>{2});
            CHECK(kernel.risk_events == 1);
        }
    }
    // MG10-b: scope.
    {
        auto s = kernel_spec("n12-dd-reversal");
        NativeRiskLimits limits; limits.max_drawdown = loss(500.0); s.risk = limits;
        auto plan = plan_of(5);
        plan[0] = {tx(100.0, "L")}; plan[2] = {tx(-200.0, "S")}; plan[3] = {tx(200.0, "L")};
        const auto kernel = run_kernel(s, drawdown_reversal_tape(), plan);
        const auto adapter = run_adapter<DrawdownReversal>(drawdown_reversal_tape());
        verify(kDrawdownReversal, adapter, kernel);
        if (!dumping) {
            // Halted at bar 1's close, the adapter still reverses twice (two
            // script-closed rows plus the range-end row of the last long); the
            // kernel refuses both reversals and holds.
            CHECK(std::count(adapter.begin(), adapter.end(), '|') == 3);
            CHECK(adapter.find("L 100 @0:100->2:94 pnl=-600 id='S' c='' cause=1") != std::string::npos);
            CHECK(adapter.find("S 100 @2:94->3:94 pnl=0 id='L' c='' cause=1") != std::string::npos);
            CHECK(std::count(kernel.outcome.begin(), kernel.outcome.end(), '|') == 0);
            CHECK(kernel.refused == (std::vector<int>{2, 3}));
            CHECK(kernel.outcome.find("pos=100") != std::string::npos);
        }
    }
    // MG11 CL-a: per-trade sign, immediate latch.
    {
        auto s = kernel_spec("n12-cl-ordered");
        NativeRiskLimits limits; limits.max_consecutive_loss_days = 2;
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone; s.risk = limits;
        auto plan = plan_of(76);
        for (int h : {0, 24, 26, 48, 50, 72}) plan[static_cast<std::size_t>(h)] = {tx(100.0, "L")};
        for (int h : {1, 25, 27, 49, 51, 73}) plan[static_cast<std::size_t>(h)] = {flatten("X")};
        const auto kernel = run_kernel(s, loss_days_ordered_tape(), plan);
        const auto adapter = run_adapter<LossDaysOrdered>(loss_days_ordered_tape());
        verify(kLossDaysOrdered, adapter, kernel);
        if (!dumping) {
            // The adapter latches at day 3's first loss and books four rows;
            // the kernel's netted streak never reaches 2 and books all six.
            CHECK(std::count(adapter.begin(), adapter.end(), '|') == 4);
            CHECK(std::count(kernel.outcome.begin(), kernel.outcome.end(), '|') == 6);
            CHECK(kernel.refused.empty());
            CHECK(kernel.risk_events == 0);
        }
    }
    // MG11 CL-b: netting.
    {
        auto s = kernel_spec("n12-cl-netted");
        NativeRiskLimits limits; limits.max_consecutive_loss_days = 2;
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone; s.risk = limits;
        auto plan = plan_of(52);
        for (int h : {0, 2, 24, 26, 48}) plan[static_cast<std::size_t>(h)] = {tx(100.0, "L")};
        for (int h : {1, 3, 25, 27, 49}) plan[static_cast<std::size_t>(h)] = {flatten("X")};
        const auto kernel = run_kernel(s, loss_days_netted_tape(), plan);
        const auto adapter = run_adapter<LossDaysNetted>(loss_days_netted_tape());
        verify(kLossDaysNetted, adapter, kernel);
        if (!dumping) {
            // Every day's win resets the adapter's count: five rows, never
            // halted. Two netted losing days block the kernel at day 3's open.
            CHECK(std::count(adapter.begin(), adapter.end(), '|') == 5);
            CHECK(std::count(kernel.outcome.begin(), kernel.outcome.end(), '|') == 4);
            CHECK(kernel.refused == std::vector<int>{48});
            CHECK(kernel.risk_events == 1);
            CHECK(kernel.blocked_at_end);
        }
    }
    // MG12-a: the path extreme.
    {
        auto s = kernel_spec("n12-il-path");
        NativeRiskLimits limits; limits.max_intraday_loss = loss(1000.0);
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock; s.risk = limits;
        auto plan = plan_of(50);
        plan[0] = {tx(100.0, "L")}; plan[26] = {tx(100.0, "L")}; plan[48] = {tx(100.0, "L")};
        const auto kernel = run_kernel(s, intraday_path_tape(), plan);
        const auto adapter = run_adapter<IntradayPath>(intraday_path_tape());
        verify(kIntradayPath, adapter, kernel);
        if (!dumping) {
            // The adapter closes at the extreme (85, -1500) under its own
            // ticket, refuses the day's later entry and re-enters on day 3;
            // the kernel never breaches and holds 300 units by the end.
            CHECK(adapter.find("->24:85 pnl=-1500 id='' c='Close Position (Max intraday Loss)' cause=4") != std::string::npos);
            CHECK(std::count(kernel.outcome.begin(), kernel.outcome.end(), '|') == 0);
            CHECK(kernel.outcome.find("pos=300") != std::string::npos);
            CHECK(kernel.risk_events == 0);
        }
    }
    // MG12-b: the open, the ticket, the cancel-pending latch.
    {
        auto s = kernel_spec("n12-il-open");
        NativeRiskLimits limits; limits.max_intraday_loss = loss(1000.0);
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock; s.risk = limits;
        auto plan = plan_of(50);
        plan[0] = {tx(100.0, "L")}; plan[23] = {limit_buy(100.0, 80.0, "L2")};
        const auto kernel = run_kernel(s, intraday_open_tape(), plan);
        const auto adapter = run_adapter<IntradayOpen>(intraday_open_tape());
        verify(kIntradayOpen, adapter, kernel);
        if (!dumping) {
            // Same money, different ticket: both close 100 @ 88 for -1200
            // (the kernel stamps a fill with the next interval's time).
            CHECK(adapter.find("@0:100->25:88 pnl=-1200 id='' c='Close Position (Max intraday Loss)' cause=4") != std::string::npos);
            CHECK(kernel.outcome.find("->26:88 pnl=-1200 id='__kernel_risk__' c='Risk limit'") != std::string::npos);
            // The adapter withdrew the resting L2 with its forced close; the
            // kernel left it working and it filled at 80 on day 3.
            CHECK(adapter.find("pos=0") != std::string::npos);
            CHECK(kernel.outcome.find("pos=100") != std::string::npos);
            CHECK(kernel.risk_events == 1);
        }
    }
    // MG13: the cap.
    {
        auto s = kernel_spec("n12-cap");
        NativeRiskLimits limits; limits.max_fills_per_day = 2;
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock; s.risk = limits;
        auto plan = plan_of(26);
        for (int h : {0, 1, 2, 24}) plan[static_cast<std::size_t>(h)] = {tx(1.0, "L")};
        plan[25] = {flatten("X")};
        const auto kernel = run_kernel(s, fill_cap_tape(), plan);
        const auto adapter = run_adapter<FillCap>(fill_cap_tape());
        verify(kFillCap, adapter, kernel);
        if (!dumping) {
            // TradingView's cap close is booked at bar 1's high (103) under
            // the fill-cap ticket; the kernel's flatten at the close (101).
            CHECK(adapter.find("->1:103 pnl=3 id='' c='Close Position (Max number of filled orders in one day)' cause=5") != std::string::npos);
            CHECK(kernel.outcome.find("@1:100->2:101 pnl=1 id='__kernel_risk__' c='Risk limit'") != std::string::npos);
            CHECK(kernel.refused == std::vector<int>{2});
            // Day 2's close is that day's second fill: the kernel breaches
            // again on a flat book (an event, nothing to flatten).
            CHECK(kernel.risk_events == 2);
        }
    }
    // MG14: the risk day.
    {
        auto s = kernel_spec("n12-cap-chart-day");
        s.chart_timezone = "America/New_York";
        NativeRiskLimits limits; limits.max_fills_per_day = 2;
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock; s.risk = limits;
        auto plan = plan_of(26);
        for (int h : {0, 1, 4, 24}) plan[static_cast<std::size_t>(h)] = {tx(1.0, "L")};
        const auto kernel = run_kernel(s, fill_cap_chart_day_tape(), plan);
        const auto adapter = run_adapter<FillCapChartDay>(fill_cap_chart_day_tape());
        verify(kFillCapChartDay, adapter, kernel);
        if (!dumping) {
            // Bar 4 is 00:00 New York: a new chart day for the adapter, whose
            // entry fills and whose bar-24 fill is that chart day's second, so
            // the cap closes the book again (four cap rows, flat). The kernel
            // is still on the same UTC day at bar 4 (refused) and only bar
            // 24's entry fills.
            CHECK(std::count(adapter.begin(), adapter.end(), '|') == 4);
            CHECK(adapter.find("L 1 @4:100->24:100 pnl=0 id='' c='Close Position (Max number of filled orders in one day)' cause=5") != std::string::npos);
            CHECK(adapter.find("pos=0") != std::string::npos);
            CHECK(kernel.outcome.find("pos=1") != std::string::npos);
            CHECK(kernel.refused == std::vector<int>{4});
        }
    }
    // PS: max position size vs max_abs_units.
    {
        auto s = kernel_spec("n12-position-size");
        s.max_abs_units = 3.0;
        auto plan = plan_of(4);
        for (int h : {0, 1, 2}) plan[static_cast<std::size_t>(h)] = {tx(2.0, "L")};
        const auto kernel = run_kernel(s, position_size_tape(), plan);
        const auto adapter = run_adapter<PositionSize>(position_size_tape());
        verify(kPositionSize, adapter, kernel);
        if (!dumping) {
            CHECK(adapter.find("pos=4") != std::string::npos);
            CHECK(kernel.outcome.find("pos=2") != std::string::npos);
            CHECK(kernel.capped == (std::vector<int>{1, 2}));
        }
    }
    // SW: the structural witness. Pine's statements arrive after the spec is
    // fixed: under every rule above the adapter host's kernel ledger stayed
    // inert — no day, no peak, no block — while the adapter's own rule acted.
    if (!dumping) {
        DrawdownReversal host;
        const auto tape = drawdown_reversal_tape();
        host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
        const auto ledger = host.native_risk_state();
        CHECK(!ledger.blocked);
        CHECK(!ledger.has_day);
        CHECK(ledger.peak_equity == 0.0);
        const std::string witness = host.outcome();
        if (witness != kDrawdownReversal.adapter) {
            std::printf("FAIL SW witness outcome:\n  got      %s\n  expected %s\n", witness.c_str(),
                        kDrawdownReversal.adapter);
        }
        CHECK(witness == kDrawdownReversal.adapter);
    }

    std::printf("%d checks, %d failures\n", passed + failed, failed);
    return failed == 0 ? 0 : 1;
}
