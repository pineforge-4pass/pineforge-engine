/*
 * test_native_risk_limits.cpp — R5 lane L9: the generic native risk limits.
 *
 * Everything here is opt-in through NativeRunSpec::risk. The first scenario
 * pins that a spec WITHOUT the block books the same fills, the same report and
 * the same run-spec fold as the pre-L9 tree, so the adapter — which never sets
 * the field — is byte-identical by construction.
 *
 * Hand arithmetic throughout (point_value = 1, account fx = 1, no fee, no
 * slippage), so every equity below is exact in binary:
 *   equity(P)  = capital + realized + dir * (P - entry) * units
 *   drawdown   = running peak of equity - equity(P)
 *   intraday   = the day's opening equity - equity(P)
 * Limits are measured at three points of each script bar: its open, its own
 * close calculation, and after each applied drain.
 */
#include "native_current_fixture.hpp"

#include <pineforge/pineforge.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr const char* kRiskLabel = "__kernel_risk__";

// ── Portable pins ───────────────────────────────────────────────────────
// A raw native_continuation_hash() constant is NOT portable: the consumer
// folds the run's resolved timezone identity — the zoneinfo root and zone file
// paths of the machine that ran it — so the same run hashes differently here
// and on each CI runner. These two are machine-independent: the run-spec fold
// itself, and a digest over the finished report. Both are observed for the
// risk-free spec of scenario 1. Both are observed BEFORE this lane, on a clean
// build of engine main (b0cec54, this tree's parent commit), by a scratch probe
// carrying that scenario verbatim, and this tree reproduces both — together
// with the same event count and, on one machine, the same raw
// native_continuation_hash(). The neutrality claim is those equalities plus
// the ones below: `risk` folds nothing while it is unset, and declaring a
// block — even one with no limit in it — is what moves the fold.
constexpr std::uint64_t kNeutralSpecDigest = 15437506464986222338ull;
constexpr std::uint64_t kNeutralReportDigest = 4693577342710743061ull;
constexpr std::size_t kNeutralEventCount = 39;

// ── Tape and spec ───────────────────────────────────────────────────────
// One price per bar (O=H=L=C), so a bar's open, its path and its close are the
// same number and every mark is unambiguous.
Bar flat_bar(std::int64_t timestamp, double price) {
    return {price, price, price, price, 1.0, timestamp};
}

constexpr std::int64_t kHour = 3600000LL;
constexpr std::int64_t kDay = 86400000LL;

std::vector<Bar> hourly(const std::vector<double>& prices, std::int64_t first = T) {
    std::vector<Bar> bars;
    bars.reserve(prices.size());
    for (std::size_t i = 0; i < prices.size(); ++i) {
        bars.push_back(flat_bar(first + static_cast<std::int64_t>(i) * kHour, prices[i]));
    }
    return bars;
}

NativeRunSpec risk_spec(const char* key, const char* tf = "60",
                        const char* session = "24x7") {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = tf;
    s.script_tf = tf;
    s.tickerid = "TEST:RISK";
    s.timezone = "UTC";
    s.session = session;
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
}

NativeLossLimit loss(double value, bool percent = false) {
    NativeLossLimit limit;
    limit.value = value;
    limit.percent = percent;
    return limit;
}

// ── Host ────────────────────────────────────────────────────────────────
// One literal host: a rule per script-bar calculation, the risk ledger
// recorded at every bar, and every applied fill kept in order.
struct RiskHost final : Host {
    std::function<void(RiskHost&, int)> rule;
    int bars = 0;
    std::vector<NativeRiskState> ledger;
    std::vector<std::string> applied_labels;

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        const int index = bars++;
        ledger.push_back(native_risk_state());
        if (rule) rule(*this, index);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        Host::on_native_applied(event, context);
        applied_labels.push_back(event.request().label);
    }
};

void drive(RiskHost& host, const NativeRunSpec& s, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

std::vector<no::NativeRiskEvent> risk_events(const RiskHost& host) {
    return events<no::NativeRiskEvent>(host);
}

// The script-bar indices at which an opening was refused for `reason`.
std::vector<int> refused_bars(const RiskHost& host, no::MatchRejectReason reason) {
    std::vector<int> out;
    for (const auto& row : events<no::MatchRejectedEvent>(host)) {
        if (row.reason == reason) out.push_back(row.cursor.point.interval_index);
    }
    return out;
}

// The script-bar indices at which a request carrying `label` filled.
std::vector<int> filled_bars(const RiskHost& host, const char* label) {
    std::vector<int> out;
    for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
        if (row.request().label == label) out.push_back(row.cursor.point.interval_index);
    }
    return out;
}

// ── Report digest ───────────────────────────────────────────────────────
struct Fnv1a {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    }
    void u(std::uint64_t v) { bytes(&v, sizeof v); }
    void i(std::int64_t v) { bytes(&v, sizeof v); }
    void d(double v) { bytes(&v, sizeof v); }
};

struct Report {
    ReportC c{};
    explicit Report(const BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

std::uint64_t report_digest(const ReportC& report) {
    Fnv1a f;
    f.i(report.total_trades);
    f.i(report.trades_len);
    f.d(report.net_profit);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.pnl); f.d(t.pnl_pct);
        f.i(t.is_long);
        f.d(t.max_runup); f.d(t.max_drawdown);
        f.d(t.qty); f.d(t.commission);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.i(t.open_at_end);
    }
    f.i(report.input_bars_processed);
    f.i(report.script_bars_processed);
    f.bytes(&report.metrics, sizeof report.metrics);
    f.i(report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        f.u(report.broker_state_hash[i]);
    return f.h;
}

// ------------------------------------------------------------------- 1
// A spec with no risk block books exactly what it booked before L9: the same
// fills, the same report, the same run-spec fold. A round trip plus a refused
// opening, so admission, matching and settlement all take part.
void round_trip_rule(RiskHost& host, int index) {
    switch (index) {
    case 0: (void)put(host, tx(10.0, "enter")); break;
    case 2: (void)put(host, tx(10.0, "add")); break;
    case 4: (void)put(host, flat("exit")); break;
    default: break;
    }
}

void neutral_without_risk() {
    auto s = risk_spec("l9-neutral");
    s.max_abs_units = 15.0;        // the existing cap refuses the add
    RiskHost host;
    host.rule = round_trip_rule;
    drive(host, s, hourly({100.0, 101.0, 102.0, 103.0, 104.0, 105.0}));

    CHECK(events<no::ExecutionAppliedEvent>(host).size() == 2);
    CHECK(refused_bars(host, no::MatchRejectReason::MaxAbsUnits) == std::vector<int>{2});
    CHECK(host.trade_count() == 1);
    const auto count = host.native_events(0).size();
    if (count != kNeutralEventCount) {
        std::printf("  neutral event count %zu, pinned %zu\n", count, kNeutralEventCount);
    }
    CHECK(count == kNeutralEventCount);
    {
        Report report(host);
        const auto digest = report_digest(report.c);
        if (digest != kNeutralReportDigest) {
            std::printf("  neutral report digest %llu, pinned %llu\n",
                        static_cast<unsigned long long>(digest),
                        static_cast<unsigned long long>(kNeutralReportDigest));
        }
        CHECK(digest == kNeutralReportDigest);
    }
    // The portable half: the run-spec fold this run applies is pinned, stating
    // `risk` as absent keeps it exactly there, and declaring a block moves it.
    const auto digest = native_run_spec_digest(s);
    if (digest != kNeutralSpecDigest) {
        std::printf("  neutral spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kNeutralSpecDigest));
    }
    CHECK(digest == kNeutralSpecDigest);
    auto stated = s;
    stated.risk.reset();
    CHECK(native_run_spec_digest(stated) == kNeutralSpecDigest);
    // Presence is the opt-in: a block with no limit in it at all still moves
    // the fold, and each field moves it further.
    auto declared = s;
    declared.risk = NativeRiskLimits{};
    CHECK(native_run_spec_digest(declared) != kNeutralSpecDigest);
    auto with_drawdown = declared;
    with_drawdown.risk->max_drawdown = loss(1000.0);
    CHECK(native_run_spec_digest(with_drawdown) != native_run_spec_digest(declared));
    auto percent_form = with_drawdown;
    percent_form.risk->max_drawdown = loss(1000.0, true);
    CHECK(native_run_spec_digest(percent_form) != native_run_spec_digest(with_drawdown));
    auto by_day = declared;
    by_day.risk->day_basis = NativeRiskDay::CalendarDayInTimezone;
    CHECK(native_run_spec_digest(by_day) != native_run_spec_digest(declared));
    auto by_action = declared;
    by_action.risk->action = NativeRiskAction::FlattenAndBlock;
    CHECK(native_run_spec_digest(by_action) != native_run_spec_digest(declared));

    // The run-level half, compared between two runs in this process rather
    // than against a constant: the same spec with `risk` spelled out as absent
    // books the same trades and reaches the same continuation.
    RiskHost restated;
    restated.rule = round_trip_rule;
    drive(restated, stated, hourly({100.0, 101.0, 102.0, 103.0, 104.0, 105.0}));
    CHECK(restated.trade_count() == 1);
    CHECK(restated.native_continuation_hash() == host.native_continuation_hash());
    CHECK(risk_events(host).empty());

    // Nothing of the ledger exists for a run that declared no block.
    const auto state = host.native_risk_state();
    CHECK(!state.blocked);
    CHECK(!state.reason.has_value());
    CHECK(!state.has_day);
    CHECK(state.fills_today == 0);
    CHECK(state.consecutive_loss_days == 0);
    near(state.peak_equity, 0.0);
    near(state.day_open_equity, 0.0);
}

// ------------------------------------------------------------------- 2
// Long 100 @ 100 booked at bar 0's close. Every later bar's open is a risk
// evaluation point, and the peak equity is the 100000 of bar 0's own open:
//   bar 1 @ 92 : equity 99200, drawdown  800  (limit 1000: no breach), so the
//                bar's own 1-unit probe is admitted and fills at 92
//   bar 2 @ 88 : equity 98796 = 100000 - 100*12 - 1*4, drawdown 1204 >= 1000
// The percent form is the same threshold spelled as 1 % of the 100000 peak.
void drawdown_absolute_and_percent() {
    for (const bool percent : {false, true}) {
        auto s = risk_spec(percent ? "l9-dd-pct" : "l9-dd-abs");
        NativeRiskLimits limits;
        limits.max_drawdown = percent ? loss(1.0, true) : loss(1000.0);
        s.risk = limits;
        RiskHost host;
        host.rule = [](RiskHost& self, int index) {
            if (index == 0) (void)put(self, tx(100.0, "enter"));
            if (index >= 1) (void)put(self, tx(1.0, "probe"));
            if (index == 3) (void)put(self, reduce(10.0, "trim"));
        };
        drive(host, s, hourly({100.0, 92.0, 88.0, 88.0}));

        const auto fired = risk_events(host);
        REQUIRE(fired.size() == 1);
        CHECK(fired[0].kind == no::RiskLimitKind::MaxDrawdown);
        near(fired[0].limit, 1000.0);
        near(fired[0].observed, 1204.0);
        CHECK(fired[0].cursor.point.interval_index == 2);
        // The probe of bar 1 is admitted (the breach is bar 2's), the probes
        // of bars 2 and 3 are refused with RiskLimit.
        CHECK(filled_bars(host, "probe") == std::vector<int>{1});
        CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
              == (std::vector<int>{2, 3}));
        // A reduce is not an opening: it still passes under the block.
        CHECK(filled_bars(host, "trim") == std::vector<int>{3});
        near(host.physical_position().signed_units, 91.0);
        // No flatten: BlockOpenings leaves the book exactly as it was.
        CHECK(filled_bars(host, kRiskLabel).empty());

        const auto state = host.native_risk_state();
        CHECK(state.blocked);
        REQUIRE(state.reason.has_value());
        CHECK(*state.reason == no::RiskLimitKind::MaxDrawdown);
        near(state.peak_equity, 100000.0);
        // The ledger the host read at bar 1 was still open, at bar 2 blocked.
        REQUIRE(host.ledger.size() == 4);
        CHECK(!host.ledger[1].blocked);
        CHECK(host.ledger[2].blocked);
    }
}

// ------------------------------------------------------------------- 3
// The same tape against the day's opening equity instead of the peak. The day
// opens at bar 0 with a flat book, so day_open_equity is 100000 and both the
// absolute 1000 and the 1 % form breach at bar 2, at the same 1204.
void intraday_loss_absolute_and_percent() {
    for (const bool percent : {false, true}) {
        auto s = risk_spec(percent ? "l9-intra-pct" : "l9-intra-abs");
        NativeRiskLimits limits;
        limits.max_intraday_loss = percent ? loss(1.0, true) : loss(1000.0);
        s.risk = limits;
        RiskHost host;
        host.rule = [](RiskHost& self, int index) {
            if (index == 0) (void)put(self, tx(100.0, "enter"));
            if (index >= 1) (void)put(self, tx(1.0, "probe"));
        };
        drive(host, s, hourly({100.0, 92.0, 88.0}));

        const auto fired = risk_events(host);
        REQUIRE(fired.size() == 1);
        CHECK(fired[0].kind == no::RiskLimitKind::MaxIntradayLoss);
        near(fired[0].limit, 1000.0);
        near(fired[0].observed, 1204.0);
        CHECK(fired[0].cursor.point.interval_index == 2);
        CHECK(filled_bars(host, "probe") == std::vector<int>{1});
        CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit) == std::vector<int>{2});

        const auto state = host.native_risk_state();
        CHECK(state.blocked);
        REQUIRE(state.reason.has_value());
        CHECK(*state.reason == no::RiskLimitKind::MaxIntradayLoss);
        CHECK(state.has_day);
        near(state.day_open_equity, 100000.0);
    }
}

// ------------------------------------------------------------------- 4
// FlattenAndBlock: the breach closes the book with exactly one
// kernel-originated Flatten at the point it was measured at, and then blocks.
void flatten_and_block_issues_one_kernel_flatten() {
    auto s = risk_spec("l9-flatten");
    NativeRiskLimits limits;
    limits.max_drawdown = loss(1000.0);
    limits.action = NativeRiskAction::FlattenAndBlock;
    s.risk = limits;
    RiskHost host;
    host.rule = [](RiskHost& self, int index) {
        if (index == 0) (void)put(self, tx(100.0, "enter"));
        if (index >= 1) (void)put(self, tx(1.0, "probe"));
    };
    drive(host, s, hourly({100.0, 92.0, 88.0, 88.0}));

    const auto fired = risk_events(host);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].kind == no::RiskLimitKind::MaxDrawdown);
    near(fired[0].limit, 1000.0);
    near(fired[0].observed, 1204.0);

    // Exactly one kernel request, and it is the kernel's own authorship.
    std::size_t kernel_accepts = 0;
    for (const auto& row : events<no::AcceptedEvent>(host)) {
        if (row.request().label != kRiskLabel) continue;
        ++kernel_accepts;
        REQUIRE(row.definition != nullptr);
        CHECK(row.definition->origin == no::RequestOrigin::KernelRisk);
    }
    CHECK(kernel_accepts == 1);
    const auto flattens = filled_bars(host, kRiskLabel);
    REQUIRE(flattens.size() == 1);
    CHECK(flattens[0] == 2);
    for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
        if (row.request().label != kRiskLabel) continue;
        // Booked at the bar-open price the breach was measured at, for the
        // whole book: the 100 of bar 0 plus bar 1's admitted 1-unit probe.
        near(row.resolved_price, 88.0);
        near(row.closed_units, 101.0);
    }
    near(host.physical_position().signed_units, 0.0);
    // The host saw the flatten as an ordinary applied fill.
    CHECK(std::count(host.applied_labels.begin(), host.applied_labels.end(),
                     std::string(kRiskLabel)) == 1);
    // Realized -1204 and blocked from bar 2 on; bar 3's probe is refused too,
    // and the block does not issue a second flatten on a flat book.
    near(host.net(), -1204.0);
    CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
          == (std::vector<int>{2, 3}));
}

// ------------------------------------------------------------------- 5
// The intraday block lasts to the end of ITS day, and the two day bases
// disagree about where that is. The session is 1800-1700, so the session day
// runs from 18:00 to 17:00 the next civil date while the calendar day in the
// spec timezone turns at midnight.
//
// Hourly tape from 18:00 (T is 2025-01-06 00:00 UTC):
//   index 0 = 18:00 day 1   long 100 @ 100 at its close
//   index 2 = 20:00 day 1   88 -> intraday loss 1200 >= 1000 : breach
//   index 6 = 00:00 day 2   the calendar day turns here
//   index 7 = 01:00 day 2
//   index 8 = 18:00 day 2   the session day turns here (17:00 is out of
//                           session and is not in the tape)
std::vector<Bar> midnight_tape() {
    const std::int64_t open = T + 18 * kHour;
    std::vector<Bar> bars;
    const double prices[] = {100.0, 100.0, 88.0, 88.0, 88.0, 88.0, 88.0, 88.0};
    for (int i = 0; i < 8; ++i) {
        bars.push_back(flat_bar(open + static_cast<std::int64_t>(i) * kHour, prices[i]));
    }
    // 18:00 of the next civil day: a new session day, skipping the 17:00 gap.
    bars.push_back(flat_bar(open + kDay, 88.0));
    return bars;
}

void intraday_block_lifts_by_day_basis() {
    struct Case {
        const char* key;
        NativeRiskDay basis;
        std::vector<int> refused;
        int first_open_again;
    };
    const Case cases[] = {
        // Session day: 18:00 day 1 through 01:00 day 2 are one day, so the
        // block holds until the 18:00 bar of the next session day.
        {"l9-session-day", NativeRiskDay::SessionDay, {2, 3, 4, 5, 6, 7}, 8},
        // Calendar day in UTC: midnight lifts it.
        {"l9-calendar-day", NativeRiskDay::CalendarDayInTimezone, {2, 3, 4, 5}, 6},
    };
    for (const auto& row : cases) {
        auto s = risk_spec(row.key, "60", "1800-1700");
        NativeRiskLimits limits;
        limits.max_intraday_loss = loss(1000.0);
        limits.day_basis = row.basis;
        s.risk = limits;
        RiskHost host;
        host.rule = [](RiskHost& self, int index) {
            if (index == 0) (void)put(self, tx(100.0, "enter"));
            if (index >= 1) (void)put(self, tx(1.0, "probe"));
        };
        drive(host, s, midnight_tape());

        CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit) == row.refused);
        const auto opened = filled_bars(host, "probe");
        REQUIRE(opened.size() >= 2);
        CHECK(opened[0] == 1);
        CHECK(opened[1] == row.first_open_again);
        // Exactly one breach: the new day re-arms rather than re-firing.
        CHECK(risk_events(host).size() == 1);
        CHECK(!host.native_risk_state().blocked);
    }
}

// ------------------------------------------------------------------- 6
// Consecutive loss days count the days that CLOSE with a realized loss, and a
// day that closes with a realized profit restarts the count. Daily bars, so
// one bar is one day; the streak is settled when the next day opens.
//   day 0  open 10 @ 100
//   day 1  flatten at 99  (-10 realized) and re-open 10
//   day 2  flatten at 98  (-10 realized) and re-open 10  -> streak 1 at open
//   day 3  the day-3 open settles day 2: streak 2 -> breach with a limit of 2
void consecutive_loss_days_counts_realized_losses() {
    struct Case {
        const char* key;
        double day_two_price;   // 98 = a second losing day, 101 = a winning one
        bool breaches;
    };
    const Case cases[] = {
        {"l9-loss-days", 98.0, true},
        {"l9-loss-days-reset", 101.0, false},
    };
    for (const auto& row : cases) {
        auto s = risk_spec(row.key, "D");
        NativeRiskLimits limits;
        limits.max_consecutive_loss_days = 2;
        s.risk = limits;
        RiskHost host;
        host.rule = [](RiskHost& self, int index) {
            if (index > 0) (void)put(self, flat("exit"));
            (void)put(self, tx(10.0, "enter"));
        };
        std::vector<Bar> bars;
        const double prices[] = {100.0, 99.0, row.day_two_price, 100.0};
        for (int i = 0; i < 4; ++i) {
            bars.push_back(flat_bar(T + static_cast<std::int64_t>(i) * kDay, prices[i]));
        }
        drive(host, s, bars);

        const auto fired = risk_events(host);
        if (row.breaches) {
            REQUIRE(fired.size() == 1);
            CHECK(fired[0].kind == no::RiskLimitKind::MaxConsecutiveLossDays);
            near(fired[0].limit, 2.0);
            near(fired[0].observed, 2.0);
            CHECK(fired[0].cursor.point.interval_index == 3);
            CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
                  == std::vector<int>{3});
            // The exit of day 3 is a reduce and still passes.
            CHECK(filled_bars(host, "exit") == (std::vector<int>{1, 2, 3}));
            CHECK(host.native_risk_state().consecutive_loss_days == 2);
        } else {
            CHECK(fired.empty());
            CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit).empty());
            // Day 2 closed +20 (short of nothing: the 10 units bought at 99
            // were flattened at 101), which restarted the count.
            CHECK(host.native_risk_state().consecutive_loss_days == 0);
        }
    }
}

// ------------------------------------------------------------------- 7
// Applied fills per day. Two fills at bar 0's close reach a limit of 2, so
// every later opening OF THAT DAY is refused; the next day restarts the count.
void fills_per_day_counts_and_resets() {
    auto s = risk_spec("l9-fills");
    NativeRiskLimits limits;
    limits.max_fills_per_day = 2;
    s.risk = limits;
    RiskHost host;
    host.rule = [](RiskHost& self, int index) {
        if (index == 0) {
            (void)put(self, tx(1.0, "enter"));
            (void)put(self, tx(1.0, "enter"));
        } else {
            (void)put(self, tx(1.0, "probe"));
        }
    };
    // Two hours of day 1, then the first hour of the next civil day.
    std::vector<Bar> bars = {
        flat_bar(T, 100.0),
        flat_bar(T + kHour, 100.0),
        flat_bar(T + kDay, 100.0),
    };
    drive(host, s, bars);

    const auto fired = risk_events(host);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].kind == no::RiskLimitKind::MaxFillsPerDay);
    near(fired[0].limit, 2.0);
    near(fired[0].observed, 2.0);
    CHECK(fired[0].cursor.point.interval_index == 0);
    CHECK(filled_bars(host, "enter").size() == 2);
    CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit) == std::vector<int>{1});
    CHECK(filled_bars(host, "probe") == std::vector<int>{2});
    // The ledger the host read at each bar: two fills on day 1 and blocked,
    // one fill on day 2 and open again.
    REQUIRE(host.ledger.size() == 3);
    CHECK(host.ledger[1].fills_today == 2);
    CHECK(host.ledger[1].blocked);
    CHECK(host.ledger[2].fills_today == 0);
    CHECK(!host.ledger[2].blocked);
    CHECK(host.ledger[2].day_ordinal == host.ledger[1].day_ordinal + 1);
    CHECK(!host.native_risk_state().blocked);
}

// ------------------------------------------------------------------ 7b
// The close calculation is the third evaluation point. Bar 1 OPENS at the
// running peak and closes 12 lower, so the drawdown does not exist at its
// open and does exist at its close:
//   bar 1 open  @ 100 : equity 100000, drawdown    0  (limit 1000: nothing)
//   bar 1 close @  88 : equity  98800, drawdown 1200  (>= 1000: breach)
// The probe the host submits in that same calculation is matched after it,
// so it is the very next opening — and it is refused. Nothing is refused
// earlier: the ledger the host itself reads at that calculation, before the
// close evaluation, is still open.
void drawdown_at_close_blocks_the_same_point() {
    auto s = risk_spec("l9-close-point");
    NativeRiskLimits limits;
    limits.max_drawdown = loss(1000.0);
    s.risk = limits;
    RiskHost host;
    host.rule = [](RiskHost& self, int index) {
        if (index == 0) (void)put(self, tx(100.0, "enter"));
        if (index >= 1) (void)put(self, tx(1.0, "probe"));
    };
    const std::vector<Bar> bars = {
        flat_bar(T, 100.0),
        {100.0, 100.0, 88.0, 88.0, 1.0, T + kHour},   // opens at the peak, closes 12 down
        flat_bar(T + 2 * kHour, 88.0),
    };
    drive(host, s, bars);

    const auto fired = risk_events(host);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].kind == no::RiskLimitKind::MaxDrawdown);
    near(fired[0].limit, 1000.0);
    near(fired[0].observed, 1200.0);
    // Measured at bar 1, not bar 2: the close of the bar the drawdown
    // appeared in.
    CHECK(fired[0].cursor.point.interval_index == 1);
    // The entry of bar 0 was admitted; the probe of bar 1, matched after that
    // same calculation, is the first opening the block refuses.
    CHECK(filled_bars(host, "enter") == std::vector<int>{0});
    CHECK(filled_bars(host, "probe").empty());
    CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
          == (std::vector<int>{1, 2}));
    near(host.physical_position().signed_units, 100.0);
    // And no earlier: at bar 1's own calculation — before the close
    // evaluation — the ledger the host reads is still open.
    REQUIRE(host.ledger.size() == 3);
    CHECK(!host.ledger[0].blocked);
    CHECK(!host.ledger[1].blocked);
    CHECK(host.ledger[2].blocked);
    const auto state = host.native_risk_state();
    CHECK(state.blocked);
    REQUIRE(state.reason.has_value());
    CHECK(*state.reason == no::RiskLimitKind::MaxDrawdown);
}

// ------------------------------------------------------------------- 8
// Configuration: every limit is validated, and a zero count is named.
void configuration_is_validated() {
    struct Case {
        const char* key;
        std::function<void(NativeRiskLimits&)> spoil;
        NativeRunSpecError error;
        NativeRunSpecField field;
    };
    const Case cases[] = {
        {"l9-bad-dd", [](NativeRiskLimits& r) { r.max_drawdown = loss(0.0); },
         NativeRunSpecError::NotFinitePositive, NativeRunSpecField::RiskDrawdown},
        {"l9-bad-intra", [](NativeRiskLimits& r) { r.max_intraday_loss = loss(-1.0); },
         NativeRunSpecError::NotFinitePositive, NativeRunSpecField::RiskIntradayLoss},
        {"l9-bad-days", [](NativeRiskLimits& r) { r.max_consecutive_loss_days = 0; },
         NativeRunSpecError::ZeroRiskLimit, NativeRunSpecField::RiskLossDays},
        {"l9-bad-fills", [](NativeRiskLimits& r) { r.max_fills_per_day = 0; },
         NativeRunSpecError::ZeroRiskLimit, NativeRunSpecField::RiskFillsPerDay},
        {"l9-bad-basis", [](NativeRiskLimits& r) {
             r.day_basis = static_cast<NativeRiskDay>(7);
         },
         NativeRunSpecError::UnknownRiskDay, NativeRunSpecField::RiskDayBasis},
        {"l9-bad-action", [](NativeRiskLimits& r) {
             r.action = static_cast<NativeRiskAction>(9);
         },
         NativeRunSpecError::UnknownRiskAction, NativeRunSpecField::RiskAction},
    };
    for (const auto& row : cases) {
        auto s = risk_spec(row.key);
        NativeRiskLimits limits;
        row.spoil(limits);
        s.risk = limits;
        RiskHost host;
        const auto setup = host.configure_native(s);
        CHECK(setup.status == NativeSetupStatus::Failed);
        CHECK(setup.validation.error == row.error);
        CHECK(setup.validation.field == row.field);
    }
    // A declared block with no limit at all is legal and inert.
    auto empty = risk_spec("l9-empty");
    empty.risk = NativeRiskLimits{};
    RiskHost host;
    REQUIRE(host.configure_native(empty).status == NativeSetupStatus::Applied);
    host.rule = [](RiskHost& self, int index) {
        if (index == 0) (void)put(self, tx(10.0, "enter"));
    };
    const auto bars = hourly({100.0, 90.0, 80.0});
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(risk_events(host).empty());
    CHECK(!host.native_risk_state().blocked);
    // The ledger still runs: a declared block keeps its day and its peak.
    CHECK(host.native_risk_state().has_day);
    near(host.native_risk_state().peak_equity, 100000.0);
}

// ------------------------------------------------------------------ TWIN
// tests/test_engine_risk.cpp drives the ADAPTER's strategy.risk.* rules
// through its protected surface (RiskProbe). Two of its scenarios are
// reproduced here on a bare native host with the generic limits.
//
// (a) test_intraday_loss_absolute_halt: capital 100000, max_intraday_loss
//     1000 absolute, a long 100 @ 100 carried into a day that opened at
//     100000; the tick at 92 does not fire (-800), the tick at 88 does
//     (-1200): the position is closed at that tick, orders are blocked for
//     the day, nothing latches, and the next chart-day lifts the block.
//
//     The native run books the SAME sequence bar for bar: no breach at 92,
//     the breach at 88 with the position flattened at 88, openings refused
//     for the rest of that day and admitted again on the next day. The
//     measured loss is 1204 rather than TV's 1200 because this run also
//     admits a 1-unit probe at the 92 bar, which marks its own -4 at 88.
//
//     Itemized differences, each a named TradingView quirk:
//       * MG14 (day key): TV keys the day on chart_day_key, the composite
//         mday*100+month of the chart timezone (pine_adapter.cpp:11281-11305).
//         The kernel keys on the calendar day ordinal of the spec timezone.
//         Same boundary here, different key.
//       * MG12 (cancel-pending): TV's forced close also latches
//         intraday_cancel_pending and withdraws the working orders
//         (pine_adapter.cpp:12400-12409). The kernel blocks openings and
//         leaves every working order where it is.
//       * MG12 (fill pricing and comment): TV submits its Flatten with
//         NearestTick pricing and the legacy comment "Close Position (Max
//         intraday Loss)"; the kernel books AsPresented at the evaluation
//         point under its own engine-owned label.
//       * MG12 (threshold epsilon): TV compares loss + 1e-9*max(1,|limit|)
//         >= limit (pine_adapter.cpp:11427-11429); the kernel compares
//         exactly. Both fire at -1200 against 1000.
//       * MG12 (unbooked closing fill): TV excludes the closing fill's own
//         realized P&L at its own tick; the kernel marks equity as it stands.
//
// (b) test_max_drawdown_absolute_halt: max_drawdown 5000 absolute latches at
//     exactly 5000 of observed drawdown and refuses entries in BOTH
//     directions, without closing anything.
//
//     Itemized difference: MG10 (sampling). TV measures against the engine's
//     own max_drawdown_ tracker, which samples every processed bar and tick;
//     the kernel measures at its two evaluation points (script-bar open,
//     applied drain). The two agree on this tape because each bar's open is
//     its whole path.
void twin_of_adapter_risk_halts() {
    // (a) the intraday-loss half.
    {
        auto s = risk_spec("l9-twin-intraday");
        NativeRiskLimits limits;
        limits.max_intraday_loss = loss(1000.0);
        limits.day_basis = NativeRiskDay::CalendarDayInTimezone;
        limits.action = NativeRiskAction::FlattenAndBlock;
        s.risk = limits;
        RiskHost host;
        host.rule = [](RiskHost& self, int index) {
            if (index == 0) (void)put(self, tx(100.0, "enter"));
            if (index >= 1) (void)put(self, tx(1.0, "probe"));
        };
        std::vector<Bar> bars = {
            flat_bar(T, 100.0),           // day 1 opens flat: E_day = 100000
            flat_bar(T + kHour, 92.0),    // -800: no fire, exactly as TV
            flat_bar(T + 2 * kHour, 88.0),// -1200: fire
            flat_bar(T + 3 * kHour, 88.0),// still blocked, same day
            flat_bar(T + kDay, 88.0),     // next chart-day: open again
        };
        drive(host, s, bars);

        const auto fired = risk_events(host);
        REQUIRE(fired.size() == 1);
        CHECK(fired[0].kind == no::RiskLimitKind::MaxIntradayLoss);
        CHECK(fired[0].cursor.point.interval_index == 2);
        // 1204, not TV's 1200: bar 1's admitted probe marks its own -4 here.
        near(fired[0].observed, 1204.0);
        // Closed at the breach point, like TV's close at the firing tick.
        const auto flattens = filled_bars(host, kRiskLabel);
        REQUIRE(flattens.size() == 1);
        CHECK(flattens[0] == 2);
        near(host.physical_position().signed_units, 1.0);  // bar 4's probe
        // Blocked for the rest of the day, open again on the next one.
        CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
              == (std::vector<int>{2, 3}));
        CHECK(filled_bars(host, "probe") == (std::vector<int>{1, 4}));
        // Nothing latched: the run is open again on the next day.
        CHECK(!host.native_risk_state().blocked);
    }
    // (b) the drawdown half. Two runs on the same tape, one probing the long
    // side and one the short, because a probe that merely reduces the live
    // book is not an opening and never reaches the gate at all.
    {
        struct Direction {
            const char* key;
            double probe;          // +1 adds to the long book, -200 reverses it
            bool probe_before;     // whether the same probe runs before the breach
            double observed;
        };
        const Direction directions[] = {
            // The long probe of bar 1 is admitted and marks its own -0.01 at
            // bar 2, so the observed drawdown is 5000.01.
            {"l9-twin-dd-long", 1.0, true, 5000.01},
            // Nothing is added before the breach, so the drawdown is TV's
            // exact 5000 against a 5000 limit: the threshold is reached, not
            // passed.
            {"l9-twin-dd-short", -200.0, false, 5000.0},
        };
        for (const auto& row : directions) {
            auto s = risk_spec(row.key);
            NativeRiskLimits limits;
            limits.max_drawdown = loss(5000.0);
            s.risk = limits;
            RiskHost host;
            const double probe = row.probe;
            const bool before = row.probe_before;
            host.rule = [probe, before](RiskHost& self, int index) {
                if (index == 0) (void)put(self, tx(100.0, "enter"));
                if (index == 1 && before) (void)put(self, tx(probe, "probe"));
                if (index == 2) (void)put(self, tx(probe, "probe"));
            };
            std::vector<Bar> bars = {
                flat_bar(T, 100.0),
                flat_bar(T + kHour, 50.01),      // drawdown 4999: no block
                flat_bar(T + 2 * kHour, 50.0),   // drawdown at/over 5000: block
            };
            drive(host, s, bars);

            const auto fired = risk_events(host);
            REQUIRE(fired.size() == 1);
            CHECK(fired[0].kind == no::RiskLimitKind::MaxDrawdown);
            CHECK(fired[0].cursor.point.interval_index == 2);
            near(fired[0].limit, 5000.0);
            near(fired[0].observed, row.observed);
            // Refused after the breach, admitted before it: TV's latch refuses
            // check_risk_allow_entry for long and short alike.
            CHECK(refused_bars(host, no::MatchRejectReason::RiskLimit)
                  == std::vector<int>{2});
            CHECK(filled_bars(host, "probe")
                  == (before ? std::vector<int>{1} : std::vector<int>{}));
            // Nothing was closed: the block refuses openings, it does not
            // close the book.
            CHECK(filled_bars(host, kRiskLabel).empty());
            CHECK(host.native_risk_state().blocked);
            near(host.physical_position().signed_units, before ? 101.0 : 100.0);
        }
    }
}

}  // namespace

int main() {
    test("neutral-without-risk", neutral_without_risk);
    test("drawdown", drawdown_absolute_and_percent);
    test("intraday-loss", intraday_loss_absolute_and_percent);
    test("flatten-and-block", flatten_and_block_issues_one_kernel_flatten);
    test("day-basis", intraday_block_lifts_by_day_basis);
    test("consecutive-loss-days", consecutive_loss_days_counts_realized_losses);
    test("fills-per-day", fills_per_day_counts_and_resets);
    test("close-point", drawdown_at_close_blocks_the_same_point);
    test("configuration", configuration_is_validated);
    test("twin-adapter-risk-halts", twin_of_adapter_risk_halts);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
