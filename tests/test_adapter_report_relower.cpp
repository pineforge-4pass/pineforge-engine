// R5 lane R1: the Pine adapter's report, re-lowered onto the kernel.
//
// The adapter no longer records the equity curve itself. Its run spec selects
// NativeReportPolicy::KernelRecordedAtHostMarks and the source host only marks
// the script bar it has just published; the consumer performs the extremes
// fold and the curve append (native_execution_consumer.cpp
// mark_script_report_point). Nothing about the report may move, so these
// witnesses are data: every equity point, every reported row (the range-end
// rows included) and the per-bar broker-state sequence of three adapter runs,
// observed on engine main 06ef483 — where PineStrategyHost still called
// update_equity_extremes() / record_equity_point() itself — and pinned here.
//
// Provenance of kOrdinary / kCalcOnOrderFills / kSuppressedTail below: this
// same TU, compiled unchanged against the 06ef483 library in a scratch
// worktree with -DPINEFORGE_R1_HARVEST (which prints the observed values as
// the initializers below instead of checking them), against that tree's
// libpineforge.a. Rebuild them the same way; never edit one by hand to make a
// run pass.
//
// The three runs are chosen for the three cadences the kernel's own
// per-calculation cadence does not have:
//   * ordinary            — one mark per published source slot, and a
//                           position open at the end, so TradingView's
//                           range-end rows (which re-mark the curve's last
//                           point) are part of the pin;
//   * calc-on-order-fills — a resting stop fills intrabar, the fill re-enters
//                           the script and opens that bar's source slot, so
//                           the bar's point is marked at the fill and its
//                           ordinary close calculation marks nothing;
//   * suppressed-tail     — the probe's forming tail advances source history
//                           without invoking generated code at all, and still
//                           carries a point.
#include "native_current_fixture.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <vector>

using namespace r4_test;

namespace {

// A per-bar broker-state hash folds the consumer's continuation, and that
// folds the run's resolved timezone identity (native_execution_consumer.cpp
// hash_tz_identity) — the zone's own content since R5 lane E23, so a raw
// sequence is portable across hosts but still moves with the installed tzdata
// release, which is not a pin this row wants. The probe below overrides the projection to fold one fixed
// execution hash instead: what remains is the generic broker state and the
// source extension — position, lots, realized sums, the equity extremes the
// recording order moves, and the closed rows — all of which are portable.
constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd0001ull;

constexpr int kBars = 24;

// A triangular wave in exact binary fractions: every price, every equity
// point and every hashed double is reproducible to the bit on any platform.
double price_at(int index) {
    const int phase = index % 8;
    const int triangle = phase < 4 ? phase : 8 - phase;
    return 100.0 + 0.5 * triangle + 0.25 * (index % 3);
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(kBars));
    for (int i = 0; i < kBars; ++i) {
        const double p = price_at(i);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0,
                        T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

// A generated-strategy-shaped source host: the body issues Pine commands and
// nothing else. Pyramiding 2 leaves the feed ending on two open lots, so the
// range-end producer emits one row per lot.
class ReportProbe final : public pineforge::source::PineStrategyHost {
public:
    ReportProbe(bool calc_on_order_fills, bool suppress_tail) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.slippage = 0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 2.0;
        config.calc_on_order_fills = calc_on_order_fills;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
        if (suppress_tail) set_probe_suppress_tail_logic(true);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kProbeExecutionHash);
    }

    // How many times generated code ran. A suppressed tail advances source
    // history without a body; a calc_on_order_fills re-entry runs one extra.
    int source_bodies() const noexcept { return seen_; }

    void on_source_bar(const Bar& bar) override {
        const int index = seen_++;
        switch (index) {
        case 2:
            strategy_entry("A", true);          // market: fills at the next open
            break;
        case 8:
            strategy_close("A");                // one closed row
            break;
        case 12:
            // A stop entry rests for several bars and fills intrabar. Under
            // calc_on_order_fills that fill re-enters the script.
            strategy_entry("B", true, na<double>(), bar.high);
            break;
        case 18:
            strategy_entry("C", true);          // second lot, never closed
            break;
        default:
            break;
        }
    }

private:
    int seen_ = 0;
};

// ── Observed / pinned shape ─────────────────────────────────────────────
struct Point {
    std::int64_t time_ms;
    double equity;
    double open_profit;
};

struct Row {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    double commission;
    double max_runup;
    double max_drawdown;
    int is_long;
    int open_at_end;
};

struct Observed {
    int source_bodies = 0;
    std::vector<Point> curve;
    std::vector<std::uint64_t> hashes;
    std::vector<Row> rows;
    double max_drawdown = 0.0;
    double max_runup = 0.0;
    std::int64_t script_bars = 0;
};

struct Expected {
    const char* name;
    bool calc_on_order_fills;
    bool suppress_tail;
    const Point* curve;
    std::size_t curve_len;
    const std::uint64_t* hashes;
    std::size_t hashes_len;
    const Row* rows;
    std::size_t rows_len;
    double max_drawdown;
    double max_runup;
    std::int64_t script_bars;
    int source_bodies;
};

Observed observe(bool calc_on_order_fills, bool suppress_tail) {
    Observed out;
    ReportProbe probe(calc_on_order_fills, suppress_tail);
    const auto bars = feed();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());

    out.source_bodies = probe.source_bodies();
    ReportC c{};
    probe.fill_report(&c);
    for (std::int64_t i = 0; i < c.equity_curve_len; ++i)
        out.curve.push_back({c.equity_curve[i].time_ms, c.equity_curve[i].equity,
                             c.equity_curve[i].open_profit});
    for (std::int64_t i = 0; i < c.broker_state_hash_len; ++i)
        out.hashes.push_back(c.broker_state_hash[i]);
    for (int i = 0; i < c.trades_len; ++i) {
        const TradeC& t = c.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.commission, t.max_runup, t.max_drawdown,
                            t.is_long, t.open_at_end});
    }
    out.max_drawdown = c.metrics.equity.max_equity_drawdown;
    out.max_runup = c.metrics.equity.max_equity_runup;
    out.script_bars = c.script_bars_processed;
    BacktestEngine::free_report(&c);
    return out;
}

#ifdef PINEFORGE_R1_HARVEST
void emit(const char* symbol, const Observed& got) {
    std::printf("constexpr Point k%s_curve[] = {\n", symbol);
    for (const auto& p : got.curve)
        std::printf("    {%lldLL, %.17g, %.17g},\n",
                    static_cast<long long>(p.time_ms), p.equity, p.open_profit);
    std::printf("};\nconstexpr std::uint64_t k%s_hashes[] = {\n", symbol);
    for (const auto h : got.hashes)
        std::printf("    %lluull,\n", static_cast<unsigned long long>(h));
    std::printf("};\nconstexpr Row k%s_rows[] = {\n", symbol);
    for (const auto& r : got.rows)
        std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %.17g, %.17g, %.17g, %.17g, %d, %d},\n",
                    static_cast<long long>(r.entry_time), static_cast<long long>(r.exit_time),
                    r.entry_price, r.exit_price, r.qty, r.pnl, r.commission,
                    r.max_runup, r.max_drawdown, r.is_long, r.open_at_end);
    std::printf("};\nconstexpr double k%s_max_drawdown = %.17g;\n", symbol, got.max_drawdown);
    std::printf("constexpr double k%s_max_runup = %.17g;\n", symbol, got.max_runup);
    std::printf("constexpr std::int64_t k%s_script_bars = %lldLL;\n", symbol,
                static_cast<long long>(got.script_bars));
    std::printf("constexpr int k%s_source_bodies = %d;\n\n", symbol, got.source_bodies);
}

#else
// ── Pinned data (see the provenance note at the top of this file) ───────
// R1_PINNED_DATA_BEGIN
constexpr Point kOrdinary_curve[] = {
    {1736121600000LL, 10000, 0},
    {1736121660000LL, 10000, 0},
    {1736121720000LL, 10000, 0},
    {1736121780000LL, 10000.5, 0.5},
    {1736121840000LL, 10002, 2},
    {1736121900000LL, 10001.5, 1.5},
    {1736121960000LL, 9999.5, -0.5},
    {1736122020000LL, 9999, -1},
    {1736122080000LL, 9998.5, -1.5},
    {1736122140000LL, 9994, 0},
    {1736122200000LL, 9994, 0},
    {1736122260000LL, 9994, 0},
    {1736122320000LL, 9994, 0},
    {1736122380000LL, 9994, 0},
    {1736122440000LL, 9994, 0},
    {1736122500000LL, 9994, 0},
    {1736122560000LL, 9994, 0},
    {1736122620000LL, 9994, 0},
    {1736122680000LL, 9994, 0},
    {1736122740000LL, 9994.5, 0.5},
    {1736122800000LL, 9996.5, 2.5},
    {1736122860000LL, 9992.5, -1.5},
    {1736122920000LL, 9991.5, -2.5},
    {1736122980000LL, 9982.5, 0},
};
// expectation corrected (kOrdinary_hashes, 24 of 24 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the curves, rows and marks did not move:
//   14428523588319070169ull -> 12639766645888297560ull
//   4908364170615239347ull -> 17829204156816530938ull
//   17646203156940909272ull -> 8025178495203387901ull
//   2258553983727079610ull -> 5131852293017663393ull
//   16955472445713015603ull -> 1366831289981335516ull
//   14833304399415046940ull -> 6098945266742256491ull
//   12038547637609333378ull -> 12620064712001914325ull
//   12451371199643108104ull -> 9436544788383326163ull
//   7868177789545332922ull -> 16310238331223777337ull
//   13933672769334170286ull -> 16245567321610464409ull
//   10349727198831667288ull -> 18104293097181184875ull
//   13483841289458064577ull -> 1992390751756887078ull
//   11188055315288985692ull -> 9740657585781373307ull
//   17218777449023991603ull -> 9718527900716965688ull
//   13235898218565913262ull -> 8274551599963368185ull
//   17743664912186319639ull -> 8595352794705645044ull
//   13100530872466650237ull -> 14350650840874626930ull
//   8411210958166129527ull -> 5511030235869260280ull
//   13807168064211682463ull -> 17042293468659343500ull
//   8393008110655788396ull -> 17488769300453378241ull
//   15415869006913971177ull -> 11431951480972000938ull
//   14561487182517332750ull -> 16479222579093225873ull
//   18035903239164160095ull -> 7003778984678674920ull
//   535087629016479182ull -> 13107512069304394357ull
constexpr std::uint64_t kOrdinary_hashes[] = {
    12639766645888297560ull,
    17829204156816530938ull,
    8025178495203387901ull,
    5131852293017663393ull,
    1366831289981335516ull,
    6098945266742256491ull,
    12620064712001914325ull,
    9436544788383326163ull,
    16310238331223777337ull,
    16245567321610464409ull,
    18104293097181184875ull,
    1992390751756887078ull,
    9740657585781373307ull,
    9718527900716965688ull,
    8274551599963368185ull,
    8595352794705645044ull,
    14350650840874626930ull,
    5511030235869260280ull,
    17042293468659343500ull,
    17488769300453378241ull,
    11431951480972000938ull,
    16479222579093225873ull,
    7003778984678674920ull,
    13107512069304394357ull,
};
constexpr Row kOrdinary_rows[] = {
    {1736121780000LL, 1736122140000LL, 101.5, 100.5, 2, -6, 4, 0.5, 5, 1, 0},
    {1736122740000LL, 1736122980000LL, 101.75, 101.25, 2, -5, 4, 0.5, 4.5, 1, 1},
    {1736122800000LL, 1736122980000LL, 102.5, 101.25, 2, -6.5, 4, 0, 6, 1, 1},
};
constexpr double kOrdinary_max_drawdown = 19.5;
constexpr double kOrdinary_max_runup = 2.5;
constexpr std::int64_t kOrdinary_script_bars = 24LL;
constexpr int kOrdinary_source_bodies = 24;

constexpr Point kCalcOnOrderFills_curve[] = {
    {1736121600000LL, 10000, 0},
    {1736121660000LL, 10000, 0},
    {1736121720000LL, 10000, 0},
    {1736121780000LL, 10000.5, 0.5},
    {1736121840000LL, 10002, 2},
    {1736121900000LL, 10001.5, 1.5},
    {1736121960000LL, 9999.5, -0.5},
    {1736122020000LL, 9999, -1},
    {1736122080000LL, 9994, 0},
    {1736122140000LL, 9994, 0},
    {1736122200000LL, 9994, 0},
    {1736122260000LL, 9994.5, 0.5},
    {1736122320000LL, 9994.5, 0.5},
    {1736122380000LL, 9994, 0},
    {1736122440000LL, 9993.5, -0.5},
    {1736122500000LL, 9991.5, -2.5},
    {1736122560000LL, 9991.5, -2.5},
    {1736122620000LL, 9994.5, 0.5},
    {1736122680000LL, 9994.5, 0.5},
    {1736122740000LL, 9997.5, 3.5},
    {1736122800000LL, 10000.5, 6.5},
    {1736122860000LL, 9996.5, 2.5},
    {1736122920000LL, 9995.5, 1.5},
    {1736122980000LL, 9986.5, 0},
};
// expectation corrected (kCalcOnOrderFills_hashes, 24 of 24 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the curves, rows and marks did not move:
//   5884693225938450662ull -> 13161340968273075943ull
//   6241024304745702344ull -> 10929339750835648437ull
//   8905802012617633069ull -> 1543287778625610256ull
//   8486185426285827604ull -> 13074102385255616291ull
//   14310247159411806763ull -> 14278002143802035352ull
//   8829885583007511122ull -> 2884070919719744989ull
//   16241823467762851954ull -> 9580226274844099793ull
//   771335666206226600ull -> 12985280254195429119ull
//   4829414084647805226ull -> 3259928963577250707ull
//   17679082592490180129ull -> 10227163091378049788ull
//   18301943634580135446ull -> 13545372270959858659ull
//   4606375775964620708ull -> 261032876629205419ull
//   3300515511589250802ull -> 10479370283066526297ull
//   16307076671527510224ull -> 1526355699564266223ull
//   11876546265209465043ull -> 272852557971009988ull
//   1110051138071775592ull -> 723137568841207239ull
//   3887485794348997246ull -> 6357384183934834411ull
//   9460640120796558179ull -> 12112277347099672678ull
//   15871317000522201249ull -> 3955606011053380392ull
//   15781858999348969326ull -> 11711715509389115267ull
//   6422804272890272832ull -> 7912915566230022805ull
//   14803847465059048714ull -> 15121847794654015719ull
//   15811420619350223543ull -> 15174618554357284082ull
//   8650330379058968970ull -> 11557602030571190083ull
constexpr std::uint64_t kCalcOnOrderFills_hashes[] = {
    13161340968273075943ull,
    10929339750835648437ull,
    1543287778625610256ull,
    13074102385255616291ull,
    14278002143802035352ull,
    2884070919719744989ull,
    9580226274844099793ull,
    12985280254195429119ull,
    3259928963577250707ull,
    10227163091378049788ull,
    13545372270959858659ull,
    261032876629205419ull,
    10479370283066526297ull,
    1526355699564266223ull,
    272852557971009988ull,
    723137568841207239ull,
    6357384183934834411ull,
    12112277347099672678ull,
    3955606011053380392ull,
    11711715509389115267ull,
    7912915566230022805ull,
    15121847794654015719ull,
    15174618554357284082ull,
    11557602030571190083ull,
};
constexpr Row kCalcOnOrderFills_rows[] = {
    {1736121780000LL, 1736122080000LL, 101.5, 100.5, 2, -6, 4, 0.5, 4.5, 1, 0},
    {1736122260000LL, 1736122980000LL, 102, 101.25, 2, -5.5, 4, 0, 6.5, 1, 1},
    {1736122560000LL, 1736122980000LL, 100.25, 101.25, 2, -2, 4, 3.5, 3, 1, 1},
};
constexpr double kCalcOnOrderFills_max_drawdown = 15.5;
constexpr double kCalcOnOrderFills_max_runup = 9;
constexpr std::int64_t kCalcOnOrderFills_script_bars = 24LL;
constexpr int kCalcOnOrderFills_source_bodies = 28;

constexpr Point kSuppressedTail_curve[] = {
    {1736121600000LL, 10000, 0},
    {1736121660000LL, 10000, 0},
    {1736121720000LL, 10000, 0},
    {1736121780000LL, 10000.5, 0.5},
    {1736121840000LL, 10002, 2},
    {1736121900000LL, 10001.5, 1.5},
    {1736121960000LL, 9999.5, -0.5},
    {1736122020000LL, 9999, -1},
    {1736122080000LL, 9998.5, -1.5},
    {1736122140000LL, 9994, 0},
    {1736122200000LL, 9994, 0},
    {1736122260000LL, 9994, 0},
    {1736122320000LL, 9994, 0},
    {1736122380000LL, 9994, 0},
    {1736122440000LL, 9994, 0},
    {1736122500000LL, 9994, 0},
    {1736122560000LL, 9994, 0},
    {1736122620000LL, 9994, 0},
    {1736122680000LL, 9994, 0},
    {1736122740000LL, 9994.5, 0.5},
    {1736122800000LL, 9996.5, 2.5},
    {1736122860000LL, 9992.5, -1.5},
    {1736122920000LL, 9991.5, -2.5},
    {1736122980000LL, 9982.5, 0},
};
// expectation corrected (kSuppressedTail_hashes, 24 of 24 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the curves, rows and marks did not move:
//   14428523588319070169ull -> 12639766645888297560ull
//   4908364170615239347ull -> 17829204156816530938ull
//   17646203156940909272ull -> 8025178495203387901ull
//   2258553983727079610ull -> 5131852293017663393ull
//   16955472445713015603ull -> 1366831289981335516ull
//   14833304399415046940ull -> 6098945266742256491ull
//   12038547637609333378ull -> 12620064712001914325ull
//   12451371199643108104ull -> 9436544788383326163ull
//   7868177789545332922ull -> 16310238331223777337ull
//   13933672769334170286ull -> 16245567321610464409ull
//   10349727198831667288ull -> 18104293097181184875ull
//   13483841289458064577ull -> 1992390751756887078ull
//   11188055315288985692ull -> 9740657585781373307ull
//   17218777449023991603ull -> 9718527900716965688ull
//   13235898218565913262ull -> 8274551599963368185ull
//   17743664912186319639ull -> 8595352794705645044ull
//   13100530872466650237ull -> 14350650840874626930ull
//   8411210958166129527ull -> 5511030235869260280ull
//   13807168064211682463ull -> 17042293468659343500ull
//   8393008110655788396ull -> 17488769300453378241ull
//   15415869006913971177ull -> 11431951480972000938ull
//   14561487182517332750ull -> 16479222579093225873ull
//   18035903239164160095ull -> 7003778984678674920ull
//   5243032618906527138ull -> 4761381105021282633ull
constexpr std::uint64_t kSuppressedTail_hashes[] = {
    12639766645888297560ull,
    17829204156816530938ull,
    8025178495203387901ull,
    5131852293017663393ull,
    1366831289981335516ull,
    6098945266742256491ull,
    12620064712001914325ull,
    9436544788383326163ull,
    16310238331223777337ull,
    16245567321610464409ull,
    18104293097181184875ull,
    1992390751756887078ull,
    9740657585781373307ull,
    9718527900716965688ull,
    8274551599963368185ull,
    8595352794705645044ull,
    14350650840874626930ull,
    5511030235869260280ull,
    17042293468659343500ull,
    17488769300453378241ull,
    11431951480972000938ull,
    16479222579093225873ull,
    7003778984678674920ull,
    4761381105021282633ull,
};
constexpr Row kSuppressedTail_rows[] = {
    {1736121780000LL, 1736122140000LL, 101.5, 100.5, 2, -6, 4, 0.5, 5, 1, 0},
    {1736122740000LL, 1736122980000LL, 101.75, 101.25, 2, -5, 4, 0.5, 4.5, 1, 1},
    {1736122800000LL, 1736122980000LL, 102.5, 101.25, 2, -6.5, 4, 0, 6, 1, 1},
};
constexpr double kSuppressedTail_max_drawdown = 19.5;
constexpr double kSuppressedTail_max_runup = 2.5;
constexpr std::int64_t kSuppressedTail_script_bars = 24LL;
constexpr int kSuppressedTail_source_bodies = 23;

constexpr Expected kScenarios[] = {
    {"ordinary", false, false,
     kOrdinary_curve, std::size(kOrdinary_curve),
     kOrdinary_hashes, std::size(kOrdinary_hashes),
     kOrdinary_rows, std::size(kOrdinary_rows),
     kOrdinary_max_drawdown, kOrdinary_max_runup,
     kOrdinary_script_bars, kOrdinary_source_bodies},
    {"calc-on-order-fills", true, false,
     kCalcOnOrderFills_curve, std::size(kCalcOnOrderFills_curve),
     kCalcOnOrderFills_hashes, std::size(kCalcOnOrderFills_hashes),
     kCalcOnOrderFills_rows, std::size(kCalcOnOrderFills_rows),
     kCalcOnOrderFills_max_drawdown, kCalcOnOrderFills_max_runup,
     kCalcOnOrderFills_script_bars, kCalcOnOrderFills_source_bodies},
    {"suppressed-tail", false, true,
     kSuppressedTail_curve, std::size(kSuppressedTail_curve),
     kSuppressedTail_hashes, std::size(kSuppressedTail_hashes),
     kSuppressedTail_rows, std::size(kSuppressedTail_rows),
     kSuppressedTail_max_drawdown, kSuppressedTail_max_runup,
     kSuppressedTail_script_bars, kSuppressedTail_source_bodies},
};
// R1_PINNED_DATA_END

void compare(const Expected& pinned) {
    scenario = pinned.name;
    const Observed got = observe(pinned.calc_on_order_fills, pinned.suppress_tail);

    // One report point per script bar the source published — the suppressed
    // tail, which runs no generated code at all, included.
    REQUIRE(got.curve.size() == pinned.curve_len);
    CHECK(got.curve.size() == static_cast<std::size_t>(kBars));
    CHECK(got.script_bars == pinned.script_bars);
    // Generated code ran on its own cadence, not the curve's: one body fewer
    // than points for the suppressed tail, one more under calc_on_order_fills.
    CHECK(got.source_bodies == pinned.source_bodies);
    for (std::size_t i = 0; i < got.curve.size(); ++i) {
        CHECK(got.curve[i].time_ms == pinned.curve[i].time_ms);
        CHECK(got.curve[i].equity == pinned.curve[i].equity);
        CHECK(got.curve[i].open_profit == pinned.curve[i].open_profit);
    }

    // The metrics walk the recorded curve, so it moves with it.
    CHECK(got.max_drawdown == pinned.max_drawdown);
    CHECK(got.max_runup == pinned.max_runup);

    // Reported rows, the range-end rows among them: a range-end row is dated
    // on the curve's last point and re-marks it, so it is a direct witness of
    // where the last point was recorded.
    REQUIRE(got.rows.size() == pinned.rows_len);
    for (std::size_t i = 0; i < got.rows.size(); ++i) {
        const Row& left = got.rows[i];
        const Row& right = pinned.rows[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.commission == right.commission);
        CHECK(left.max_runup == right.max_runup);
        CHECK(left.max_drawdown == right.max_drawdown);
        CHECK(left.is_long == right.is_long);
        CHECK(left.open_at_end == right.open_at_end);
    }

    // One hash per script bar, each taken after that bar's own extremes fold
    // and its own point: the sequence pins the instant the mark happens, not
    // only its value.
    REQUIRE(got.hashes.size() == pinned.hashes_len);
    CHECK(got.hashes.size() == got.curve.size());
    for (std::size_t i = 0; i < got.hashes.size(); ++i)
        CHECK(got.hashes[i] == pinned.hashes[i]);
}

// The adapter states the policy and nothing else: the recording itself is the
// kernel's, so the run spec it projects must carry the kernel report policy.
void adapter_states_the_policy() {
    scenario = "policy";
    ReportProbe probe(false, false);
    const auto bars = feed();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    const auto state = probe.native_state();
    REQUIRE(state.spec != nullptr);
    CHECK(state.spec->report_policy == NativeReportPolicy::KernelRecordedAtHostMarks);
    // TradingView's range-end report is report shape, not a mark-to-market
    // row, so the kernel's own range-end producer stays off (§1.8 RP5).
    CHECK(state.spec->report_open_position_at_end == false);
}

#endif  // PINEFORGE_R1_HARVEST

}  // namespace

int main() {
#ifdef PINEFORGE_R1_HARVEST
    std::printf("// harvested: paste between R1_PINNED_DATA_BEGIN/END\n");
    emit("Ordinary", observe(false, false));
    emit("CalcOnOrderFills", observe(true, false));
    emit("SuppressedTail", observe(false, true));
    return 0;
#else
    try {
        for (const Expected& pinned : kScenarios) compare(pinned);
        adapter_states_the_policy();
    } catch (const Stop&) {
        // A REQUIRE already recorded the failure.
    }
    std::printf("adapter report re-lowering: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
