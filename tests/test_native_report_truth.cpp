// R5 L2 report-truth witnesses. A bare NativeStrategyHost gets a truthful
// report only when its run spec asks for one: the HostRecorded default is
// unchanged to the byte, and NativeReportPolicy::KernelRecorded adds an equity
// curve, finite equity metrics and — with report_open_position_at_end — a
// mark-to-market row for a position the feed ended with. The last scenario is
// the §3.1b twin: one rule through the bare host and through the adapter.
#include "native_current_fixture.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace r4_test;

namespace {

// ── Pinned constants ────────────────────────────────────────────────
// The report digest is observed on a clean build of engine main (73817c1
// sources, this tree's parent commit) by a scratch probe carrying the scenario
// and digest below verbatim. It is the neutrality pin of §3.1a: a defaulted
// report block may not move one bit of an existing report.
constexpr std::uint64_t kMainHostRecordedReportDigest = 11554093071070742013ull;

// The portable spec-fold pin. A raw native_continuation_hash() constant is NOT
// portable: the consumer folds the resolved timezone identity — the zoneinfo
// root and the zone file paths of the machine that ran it — so the same run
// hashes differently on this tree, on CI's macOS runner and on CI's ubuntu
// runner. native_run_spec_digest() is exactly the consumer's spec fold and
// nothing else, so it is a machine-independent value a test may pin. This one
// is observed on THIS tree for report_spec("l2-report-truth"); it guards the
// spec fold's field list and order against a future change. The neutrality
// claim itself does not rest on the constant: it is the in-process equalities
// below (explicit defaults hash exactly like implicit ones).
constexpr std::uint64_t kSpecDigest = 9134795103255102476ull;

// Any fixed execution hash: it factors the continuation out of
// broker_state_hash_from_execution_hash so two runs can be compared on their
// broker state alone.
constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd0001ull;

// ── Feed and spec ───────────────────────────────────────────────────────
// A triangular wave in exact binary fractions: every price and every equity
// point is reproducible to the bit on any platform.
double price_at(int index) {
    const int phase = index % 20;
    const int triangle = phase < 10 ? phase : 20 - phase;
    return 100.0 + 0.5 * triangle + 0.25 * (index % 3);
}

std::vector<Bar> feed(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = price_at(i);
        bars.push_back({p, p, p, p, 1.0, T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

NativeRunSpec report_spec(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:R5L2";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 2.0;
    return spec;
}

no::Request market(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

// Enter long at bar 10, flatten at bar 30, enter short at bar 40, flatten at
// bar 55: two closed rows, flat at the end. The fixture host increments
// `calculations` before the hook runs, so index == calculations - 1.
void round_trip_rule(Host& host) {
    switch (host.calculations - 1) {
    case 10: host.submit(market(2.0, "enter-long")); break;
    case 30: host.submit(market(-2.0, "exit-long")); break;
    case 40: host.submit(market(-1.0, "enter-short")); break;
    case 55: host.submit(market(1.0, "exit-short")); break;
    default: break;
    }
}

// Enter long at bar 10 and never exit: the feed ends with an open position.
void ends_long_rule(Host& host) {
    if (host.calculations - 1 == 10) host.submit(market(2.0, "enter-long"));
}

void run_feed(Host& host, const NativeRunSpec& spec, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
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

// The metrics block is folded as raw bytes, which is exact only while it has
// no padding. These pin that: 4+2 int32 and 24 doubles per trade block, 15
// doubles of equity stats.
static_assert(sizeof(pf_trade_stats_t) == 216, "trade stats layout has padding");
static_assert(sizeof(pf_equity_stats_t) == 120, "equity stats layout has padding");
static_assert(sizeof(pf_metrics_t) == 768, "metrics layout has padding");

void fold_trades(Fnv1a& f, const ReportC& report) {
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
}

std::uint64_t trades_digest(const ReportC& report) {
    Fnv1a f;
    fold_trades(f, report);
    return f.h;
}

std::uint64_t report_digest(const ReportC& report) {
    Fnv1a f;
    fold_trades(f, report);
    f.i(report.input_bars_processed);
    f.i(report.script_bars_processed);
    f.i(report.magnifier_sub_bars_total);
    f.i(report.magnifier_sample_ticks_total);
    f.i(report.input_tf_seconds);
    f.i(report.script_tf_seconds);
    f.i(report.script_tf_ratio);
    f.i(report.needs_aggregation);
    f.i(report.bar_magnifier_enabled);
    f.bytes(&report.metrics, sizeof report.metrics);
    f.i(report.equity_curve_len);
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) {
        f.i(report.equity_curve[i].time_ms);
        f.d(report.equity_curve[i].equity);
        f.d(report.equity_curve[i].open_profit);
    }
    f.i(report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        f.u(report.broker_state_hash[i]);
    return f.h;
}

// Owning report view: fill_report allocates, free_report releases.
struct Report {
    ReportC c{};
    explicit Report(const BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

// The walk engine_metrics.cpp performs, written out independently here so the
// reported drawdown/run-up are checked against the recorded curve rather than
// against a literal.
struct Walk { double drawdown = 0.0; double runup = 0.0; };

Walk walk_curve(const ReportC& report) {
    Walk out;
    if (report.equity_curve_len <= 0) return out;
    double peak = report.equity_curve[0].equity;
    double trough = peak;
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) {
        const double equity = report.equity_curve[i].equity;
        if (equity > peak) { peak = equity; trough = equity; }
        if (equity < trough) trough = equity;
        if (peak - equity > out.drawdown) out.drawdown = peak - equity;
        if (equity - trough > out.runup) out.runup = equity - trough;
    }
    return out;
}

// Test-only reach for the generic broker-state projection. It takes the
// execution hash as an argument, so two runs whose continuations differ only
// by their run spec can still be compared on their broker state alone.
struct BrokerStateHost : Host {
    std::uint64_t broker_state_from(std::uint64_t execution_hash) const {
        return broker_state_hash_from_execution_hash(execution_hash);
    }
};

// ── Adapter twin ────────────────────────────────────────────────────────
// The same rule expressed in source commands. Nothing here configures a
// native report policy: the adapter keeps HostRecorded and records the curve
// itself, which is exactly what the twin has to show is equivalent.
class TwinAdapterProbe final : public pineforge::source::PineStrategyHost {
public:
    TwinAdapterProbe() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 1;
        config.slippage = 0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 2.0;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        const int index = seen_++;
        if (index == 10) strategy_entry("L", true, na<double>(), na<double>(), 2.0);
        else if (index == 30) strategy_close("L");
    }

private:
    int seen_ = 0;
};

void twin_native_rule(Host& host) {
    switch (host.calculations - 1) {
    case 10: host.submit(market(2.0, "twin-enter")); break;
    case 30: host.submit(market(-2.0, "twin-exit")); break;
    default: break;
    }
}

// ── Scenarios ───────────────────────────────────────────────────────────

// 1. The default did not move. Every field fill_report publishes — trades,
//    diagnostics, metrics, curve, per-bar hashes — folds to the digest a
//    clean main build produced for this same run, and the curve is still
//    empty because nobody recorded it.
void host_recorded_default_is_unchanged() {
    Host host;
    host.calculation = round_trip_rule;
    run_feed(host, report_spec("l2-report-truth"), feed(60));
    completed(host);

    Report report(host);
    CHECK(report.c.equity_curve_len == 0);
    CHECK(report.c.equity_curve == nullptr);
    CHECK(report.c.trades_len == 2);
    CHECK(report.c.script_bars_processed == 60);
    CHECK(report_digest(report.c) == kMainHostRecordedReportDigest);

    // The run's continuation identity cannot be pinned as a constant (it folds
    // this machine's timezone resources); the spec half of it is pinned in
    // spec_hash_is_neutral_by_default. Here the neutrality is proved in
    // process: spelling the report block out at its defaults drives the same
    // feed to the same report and the same continuation identity.
    auto stated = report_spec("l2-report-truth");
    stated.report_policy = NativeReportPolicy::HostRecorded;
    stated.report_open_position_at_end = false;
    Host restated;
    restated.calculation = round_trip_rule;
    run_feed(restated, stated, feed(60));
    completed(restated);
    Report restated_report(restated);
    CHECK(report_digest(restated_report.c) == kMainHostRecordedReportDigest);
    CHECK(restated.native_continuation_hash() == host.native_continuation_hash());
}

// 2. KernelRecorded: one point per script bar, all finite, and the reported
//    drawdown / run-up equal an independent walk of that same curve. The
//    closed rows are bit-identical to scenario 1: recording is reporting, it
//    moves no fill.
void kernel_recorded_curve_and_metrics() {
    auto spec = report_spec("l2-report-truth");
    spec.report_policy = NativeReportPolicy::KernelRecorded;

    Host host;
    host.calculation = round_trip_rule;
    run_feed(host, spec, feed(60));
    completed(host);

    Report report(host);
    REQUIRE(report.c.equity_curve_len == 60);
    CHECK(report.c.equity_curve_len == report.c.script_bars_processed);
    for (std::int64_t i = 0; i < report.c.equity_curve_len; ++i) {
        CHECK(std::isfinite(report.c.equity_curve[i].equity));
        CHECK(std::isfinite(report.c.equity_curve[i].open_profit));
        CHECK(report.c.equity_curve[i].time_ms == T + i * 60000);
    }

    const Walk walked = walk_curve(report.c);
    CHECK(walked.drawdown > 0.0);   // the pin is not vacuous
    CHECK(walked.runup > 0.0);
    CHECK(report.c.metrics.equity.max_equity_drawdown == walked.drawdown);
    CHECK(report.c.metrics.equity.max_equity_runup == walked.runup);
    CHECK(std::isfinite(report.c.metrics.equity.max_equity_drawdown_pct));
    CHECK(std::isfinite(report.c.metrics.equity.time_in_market_pct));
    CHECK(std::isfinite(report.c.metrics.equity.open_pl));

    Host plain;
    plain.calculation = round_trip_rule;
    run_feed(plain, report_spec("l2-report-truth"), feed(60));
    completed(plain);
    Report baseline(plain);
    CHECK(trades_digest(report.c) == trades_digest(baseline.c));
}

// 3. A position the feed ends with becomes exactly one reported row per lot,
//    marked at the last close, and nothing else moves: the same equity curve,
//    the same broker state.
void open_position_row_at_range_end() {
    auto quiet = report_spec("l2-report-truth-open");
    quiet.report_policy = NativeReportPolicy::KernelRecorded;
    auto reported = quiet;
    reported.report_open_position_at_end = true;

    BrokerStateHost without;
    without.calculation = ends_long_rule;
    run_feed(without, quiet, feed(60));
    completed(without);

    BrokerStateHost with;
    with.calculation = ends_long_rule;
    run_feed(with, reported, feed(60));
    completed(with);

    Report quiet_report(without);
    Report open_report(with);

    CHECK(quiet_report.c.trades_len == 0);
    REQUIRE(open_report.c.trades_len == 1);
    const TradeC& row = open_report.c.trades[0];
    CHECK(row.open_at_end == 1);
    CHECK(row.is_long == 1);
    near(row.qty, 2.0);
    near(row.exit_price, price_at(59));
    CHECK(row.exit_time == T + 59 * 60000);

    // The lot itself is untouched: both runs end holding the same position.
    CHECK(with.lots().size() == 1);
    CHECK(without.lots().size() == with.lots().size());
    CHECK(with.rows().empty());   // the row is report space, not the blotter

    // The curve is not re-marked; that is TradingView report shape and stays
    // in the adapter (§1.8 RP5).
    REQUIRE(quiet_report.c.equity_curve_len == open_report.c.equity_curve_len);
    for (std::int64_t i = 0; i < open_report.c.equity_curve_len; ++i) {
        CHECK(quiet_report.c.equity_curve[i].equity == open_report.c.equity_curve[i].equity);
        CHECK(quiet_report.c.equity_curve[i].open_profit
              == open_report.c.equity_curve[i].open_profit);
    }

    // Enabling the flag changes the run spec, and hash_spec folds the report
    // block once the policy is non-default, so the CONTINUATION identity moves
    // by construction. What may not move is the broker state itself: hashed
    // from one fixed execution hash, the two books are identical.
    CHECK(without.broker_state_from(kProbeExecutionHash)
          == with.broker_state_from(kProbeExecutionHash));
}

// 4. §3.1b twin: one rule, two hosts. The bare native host recording its own
//    report must produce the adapter's closed row and the adapter's curve.
void twin_native_and_adapter_agree() {
    const auto bars = feed(60);

    auto spec = report_spec("l2-report-truth-twin");
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    Host native;
    native.calculation = twin_native_rule;
    run_feed(native, spec, bars);
    completed(native);

    TwinAdapterProbe adapter;
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    Report native_report(native);
    Report adapter_report(adapter);

    REQUIRE(native_report.c.trades_len == 1);
    REQUIRE(adapter_report.c.trades_len == native_report.c.trades_len);
    for (int i = 0; i < native_report.c.trades_len; ++i) {
        const TradeC& left = native_report.c.trades[i];
        const TradeC& right = adapter_report.c.trades[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.commission == right.commission);
        CHECK(left.is_long == right.is_long);
    }

    REQUIRE(native_report.c.equity_curve_len == 60);
    REQUIRE(adapter_report.c.equity_curve_len == native_report.c.equity_curve_len);
    for (std::int64_t i = 0; i < native_report.c.equity_curve_len; ++i) {
        CHECK(native_report.c.equity_curve[i].time_ms
              == adapter_report.c.equity_curve[i].time_ms);
        CHECK(native_report.c.equity_curve[i].equity
              == adapter_report.c.equity_curve[i].equity);
        CHECK(native_report.c.equity_curve[i].open_profit
              == adapter_report.c.equity_curve[i].open_profit);
    }
    CHECK(native_report.c.metrics.equity.max_equity_drawdown
          == adapter_report.c.metrics.equity.max_equity_drawdown);
    CHECK(native_report.c.metrics.equity.max_equity_runup
          == adapter_report.c.metrics.equity.max_equity_runup);
}

// 5. Continuation neutrality, stated portably: the spec fold the continuation
//    identity applies to a run spec is pinned through native_run_spec_digest,
//    spelling the report block's defaults out changes nothing, and asking for
//    kernel recording is what moves it. The configured hosts witness the same
//    three facts at run level, compared against each other rather than against
//    a machine-dependent constant.
void spec_hash_is_neutral_by_default() {
    const auto implicit_digest = native_run_spec_digest(report_spec("l2-report-truth"));
    CHECK(implicit_digest == kSpecDigest);

    auto stated = report_spec("l2-report-truth");
    stated.report_policy = NativeReportPolicy::HostRecorded;
    stated.report_open_position_at_end = false;
    CHECK(native_run_spec_digest(stated) == kSpecDigest);

    auto kernel = report_spec("l2-report-truth");
    kernel.report_policy = NativeReportPolicy::KernelRecorded;
    CHECK(native_run_spec_digest(kernel) != kSpecDigest);

    // The range-end flag is folded with the policy, so on its own — with the
    // kernel left out of the report — it is not a behaviour and folds nothing.
    auto flag_only = report_spec("l2-report-truth");
    flag_only.report_open_position_at_end = true;
    CHECK(native_run_spec_digest(flag_only) == kSpecDigest);
    auto kernel_row = kernel;
    kernel_row.report_open_position_at_end = true;
    CHECK(native_run_spec_digest(kernel_row) != native_run_spec_digest(kernel));

    Host implicit;
    REQUIRE(implicit.configure_native(report_spec("l2-report-truth")).status
            == NativeSetupStatus::Applied);

    Host explicit_default;
    REQUIRE(explicit_default.configure_native(stated).status == NativeSetupStatus::Applied);
    CHECK(explicit_default.native_continuation_hash() == implicit.native_continuation_hash());

    Host recorded;
    REQUIRE(recorded.configure_native(kernel).status == NativeSetupStatus::Applied);
    CHECK(recorded.native_continuation_hash() != implicit.native_continuation_hash());

    // An unknown policy is rejected as a typed fact, like every other run-spec
    // policy enum, and never reaches a run.
    auto invalid = report_spec("l2-report-truth");
    invalid.report_policy = static_cast<NativeReportPolicy>(7);
    const auto validation = validate_native_run_spec(invalid);
    CHECK(validation.error == NativeRunSpecError::UnknownReportPolicy);
    CHECK(validation.field == NativeRunSpecField::ReportPolicy);
}

// ── Per-bar broker-state hash (§1.8 RP9, native half) ───────────────────
std::vector<std::uint64_t> hash_rows(const ReportC& report) {
    if (report.broker_state_hash_len <= 0) return {};
    return std::vector<std::uint64_t>(
        report.broker_state_hash, report.broker_state_hash + report.broker_state_hash_len);
}

void idle_rule(Host&) {}

// 6. KernelRecorded records the whole report, the per-bar broker-state hash
//    included: with the recording switch on, a bare host reports one row per
//    script bar, 1:1 with the curve. The rows are the broker state and nothing
//    but its past — equal while two books are equal, different once they
//    diverge, and the row after bar k is the row a run that ended at bar k
//    recorded last.
void kernel_recorded_broker_hash_per_script_bar() {
    auto spec = report_spec("l2-report-truth");
    spec.report_policy = NativeReportPolicy::KernelRecorded;

    Host host;
    host.set_broker_state_hash_recording(true);
    host.calculation = round_trip_rule;
    run_feed(host, spec, feed(60));
    completed(host);

    Report report(host);
    REQUIRE(report.c.script_bars_processed == 60);
    CHECK(report.c.broker_state_hash_len == report.c.script_bars_processed);
    CHECK(report.c.broker_state_hash_len == report.c.equity_curve_len);
    REQUIRE(report.c.broker_state_hash != nullptr);
    const auto rows = hash_rows(report.c);

    // Replay: the same run records the same rows.
    Host replay;
    replay.set_broker_state_hash_recording(true);
    replay.calculation = round_trip_rule;
    run_feed(replay, spec, feed(60));
    completed(replay);
    Report replay_report(replay);
    CHECK(hash_rows(replay_report.c) == rows);

    // The rows follow the book. An idle host and the trading host are the same
    // book until the first order is submitted inside calculation 10.
    Host idle;
    idle.set_broker_state_hash_recording(true);
    idle.calculation = idle_rule;
    run_feed(idle, spec, feed(60));
    completed(idle);
    Report idle_report(idle);
    const auto idle_rows = hash_rows(idle_report.c);
    REQUIRE(idle_rows.size() == rows.size());
    for (std::size_t i = 0; i < 10; ++i) CHECK(idle_rows[i] == rows[i]);
    for (std::size_t i = 10; i < rows.size(); ++i) CHECK(idle_rows[i] != rows[i]);

    // A row is a function of the past only: a run that ends at bar 29 recorded,
    // as its last row, the row this run recorded after bar 29.
    Host prefix;
    prefix.set_broker_state_hash_recording(true);
    prefix.calculation = round_trip_rule;
    run_feed(prefix, spec, feed(30));
    completed(prefix);
    Report prefix_report(prefix);
    const auto prefix_rows = hash_rows(prefix_report.c);
    REQUIRE(prefix_rows.size() == 30);
    for (std::size_t i = 0; i < prefix_rows.size(); ++i) CHECK(prefix_rows[i] == rows[i]);
}

// 7. The recording switch is the opt-in it always was, and recording is
//    reporting: switched off the array stays empty and the run is the same run
//    to the bit; under HostRecorded the report is the host's, so the kernel
//    appends nothing to it either.
void broker_hash_rows_are_opt_in_and_move_nothing() {
    auto spec = report_spec("l2-report-truth");
    spec.report_policy = NativeReportPolicy::KernelRecorded;

    Host recorded;
    recorded.set_broker_state_hash_recording(true);
    recorded.calculation = round_trip_rule;
    run_feed(recorded, spec, feed(60));
    completed(recorded);

    Host silent;
    silent.calculation = round_trip_rule;
    run_feed(silent, spec, feed(60));
    completed(silent);

    Report recorded_report(recorded);
    Report silent_report(silent);
    CHECK(recorded_report.c.broker_state_hash_len == 60);
    CHECK(silent_report.c.broker_state_hash_len == 0);
    CHECK(silent_report.c.broker_state_hash == nullptr);
    CHECK(trades_digest(recorded_report.c) == trades_digest(silent_report.c));
    REQUIRE(recorded_report.c.equity_curve_len == silent_report.c.equity_curve_len);
    for (std::int64_t i = 0; i < recorded_report.c.equity_curve_len; ++i) {
        CHECK(recorded_report.c.equity_curve[i].equity
              == silent_report.c.equity_curve[i].equity);
    }
    CHECK(recorded.native_continuation_hash() == silent.native_continuation_hash());
    CHECK(recorded.broker_state_hash() == silent.broker_state_hash());

    Host host_owned;
    host_owned.set_broker_state_hash_recording(true);
    host_owned.calculation = round_trip_rule;
    run_feed(host_owned, report_spec("l2-report-truth"), feed(60));
    completed(host_owned);
    Report host_owned_report(host_owned);
    CHECK(host_owned_report.c.script_bars_processed == 60);
    CHECK(host_owned_report.c.broker_state_hash_len == 0);
    CHECK(host_owned_report.c.equity_curve_len == 0);
}

// 8. The same 1:1 on a stream: the warmup leg and the realtime bars of one run
//    are one report, so the array spans both.
void kernel_recorded_broker_hash_spans_a_stream() {
    auto spec = report_spec("l2-report-truth-stream");
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    const auto bars = feed(60);

    Host host;
    host.set_broker_state_hash_recording(true);
    host.calculation = round_trip_rule;
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    REQUIRE(host.stream_begin(bars.data(), 30, "1", "1"));
    {
        Report warmup(host);
        CHECK(warmup.c.script_bars_processed == 30);
        CHECK(warmup.c.broker_state_hash_len == warmup.c.script_bars_processed);
    }
    for (std::size_t i = 30; i < bars.size(); ++i) REQUIRE(host.stream_push_bar(bars[i]));
    REQUIRE(host.stream_end(false));
    CHECK(host.last_error().empty());

    Report report(host);
    CHECK(report.c.script_bars_processed == 60);
    CHECK(report.c.broker_state_hash_len == report.c.script_bars_processed);
    CHECK(report.c.broker_state_hash_len == report.c.equity_curve_len);
    CHECK(report.c.trades_len == 2);
}

}  // namespace

int main() {
    test("host_recorded_default_is_unchanged", host_recorded_default_is_unchanged);
    test("kernel_recorded_curve_and_metrics", kernel_recorded_curve_and_metrics);
    test("open_position_row_at_range_end", open_position_row_at_range_end);
    test("twin_native_and_adapter_agree", twin_native_and_adapter_agree);
    test("spec_hash_is_neutral_by_default", spec_hash_is_neutral_by_default);
    test("kernel_recorded_broker_hash_per_script_bar",
         kernel_recorded_broker_hash_per_script_bar);
    test("broker_hash_rows_are_opt_in_and_move_nothing",
         broker_hash_rows_are_opt_in_and_move_nothing);
    test("kernel_recorded_broker_hash_spans_a_stream",
         kernel_recorded_broker_hash_spans_a_stream);
    std::printf("test_native_report_truth: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
