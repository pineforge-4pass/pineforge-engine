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
// expectation corrected (v19-E, kOrdinary_hashes, 24 of 24 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the curves, rows and marks did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12639766645888297560ull -> 2926214571128404135ull
//   17829204156816530938ull -> 2460311144695717203ull
//   8025178495203387901ull -> 8827033979564876247ull
//   5131852293017663393ull -> 12198137721016629270ull
//   1366831289981335516ull -> 8010034670511875064ull
//   6098945266742256491ull -> 14775446960658454710ull
//   12620064712001914325ull -> 12026164355176752283ull
//   9436544788383326163ull -> 17208159137580538219ull
//   16310238331223777337ull -> 5096664486441078049ull
//   16245567321610464409ull -> 16022104096870556683ull
//   18104293097181184875ull -> 4836076347901812102ull
//   1992390751756887078ull -> 8937674376376720246ull
//   9740657585781373307ull -> 2703551404259163957ull
//   9718527900716965688ull -> 3061368442138030587ull
//   8274551599963368185ull -> 10178081078865716434ull
//   8595352794705645044ull -> 8206897629920030398ull
//   14350650840874626930ull -> 6525045815118222249ull
//   5511030235869260280ull -> 15184055765275961048ull
//   17042293468659343500ull -> 5759341035588050840ull
//   17488769300453378241ull -> 3484237427845144905ull
//   11431951480972000938ull -> 6855851226167457247ull
//   16479222579093225873ull -> 17070412854470623372ull
//   7003778984678674920ull -> 5698741489304391014ull
//   13107512069304394357ull -> 8249112969542764486ull
// expectation corrected (V19-FIX, kOrdinary_hashes, 24 of 24 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the curves, rows and marks did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2926214571128404135ull -> 10420073189004151623ull
//   2460311144695717203ull -> 13145042526267837939ull
//   8827033979564876247ull -> 2622782777793463991ull
//   12198137721016629270ull -> 17996446729054892726ull
//   8010034670511875064ull -> 3969181056128787992ull
//   14775446960658454710ull -> 16947117433974038ull
//   12026164355176752283ull -> 423735939073054907ull
//   17208159137580538219ull -> 18056612604689961163ull
//   5096664486441078049ull -> 12445011709498043361ull
//   16022104096870556683ull -> 16382730376298748267ull
//   4836076347901812102ull -> 12718112296054108390ull
//   8937674376376720246ull -> 913929622865489206ull
//   2703551404259163957ull -> 13218761675008378229ull
//   3061368442138030587ull -> 10388057217060506235ull
//   10178081078865716434ull -> 4373273430774112306ull
//   8206897629920030398ull -> 11946915320520331422ull
//   6525045815118222249ull -> 2472677882334572745ull
//   15184055765275961048ull -> 4709222664471492344ull
//   5759341035588050840ull -> 5785035152764109816ull
//   3484237427845144905ull -> 15834701310167426825ull
//   6855851226167457247ull -> 18416054639860847807ull
//   17070412854470623372ull -> 9654818568811123148ull
//   5698741489304391014ull -> 7514399512975361670ull
//   8249112969542764486ull -> 375912627396258150ull
// expectation corrected (INT26 v19 hash re-pin, kOrdinary_hashes, 24 of 24 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the curves, rows and marks did not move:
//   10420073189004151623ull -> 243601838968427296ull [H-THIN]
//   13145042526267837939ull -> 14082045497352522076ull [H-THIN]
//   2622782777793463991ull -> 16788444728701273278ull [H-THIN]
//   17996446729054892726ull -> 4735244315032400709ull [H-THIN]
//   3969181056128787992ull -> 9274703563551556229ull [H-THIN]
//   16947117433974038ull -> 4437548837203718331ull [H-THIN]
//   423735939073054907ull -> 9447280110204500336ull [H-THIN]
//   18056612604689961163ull -> 644150700710845644ull [H-THIN]
//   12445011709498043361ull -> 12856503234686701952ull [H-THIN]
//   16382730376298748267ull -> 8228843622305023308ull [H-THIN]
//   12718112296054108390ull -> 11723584407685805665ull [H-THIN]
//   913929622865489206ull -> 12427688725613086017ull [H-THIN]
//   13218761675008378229ull -> 5717024028566739016ull [H-THIN]
//   10388057217060506235ull -> 9136288938210272546ull [H-THIN]
//   4373273430774112306ull -> 14265105010801500849ull [H-THIN]
//   11946915320520331422ull -> 5913118032193444201ull [H-THIN]
//   2472677882334572745ull -> 2601700310587933770ull [H-THIN]
//   4709222664471492344ull -> 16710877784426826175ull [H-THIN]
//   5785035152764109816ull -> 12222101465241554407ull [H-THIN]
//   15834701310167426825ull -> 1466897115669479138ull [H-THIN]
//   18416054639860847807ull -> 8818039625616895170ull [H-THIN]
//   9654818568811123148ull -> 17382764399684682567ull [H-THIN]
//   7514399512975361670ull -> 5569220102658162789ull [H-THIN]
//   375912627396258150ull -> 5858716210019128261ull [H-THIN]
constexpr std::uint64_t kOrdinary_hashes[] = {
    243601838968427296ull,
    14082045497352522076ull,
    16788444728701273278ull,
    4735244315032400709ull,
    9274703563551556229ull,
    4437548837203718331ull,
    9447280110204500336ull,
    644150700710845644ull,
    12856503234686701952ull,
    8228843622305023308ull,
    11723584407685805665ull,
    12427688725613086017ull,
    5717024028566739016ull,
    9136288938210272546ull,
    14265105010801500849ull,
    5913118032193444201ull,
    2601700310587933770ull,
    16710877784426826175ull,
    12222101465241554407ull,
    1466897115669479138ull,
    8818039625616895170ull,
    17382764399684682567ull,
    5569220102658162789ull,
    5858716210019128261ull,
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
// expectation corrected (v19-E, kCalcOnOrderFills_hashes, 24 of 24 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the curves, rows and marks did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   13161340968273075943ull -> 5145607761876851736ull
//   10929339750835648437ull -> 6152128856392094028ull
//   1543287778625610256ull -> 13682746943137549482ull
//   13074102385255616291ull -> 4503732430497534158ull
//   14278002143802035352ull -> 11859342531076363326ull
//   2884070919719744989ull -> 14194996582993046802ull
//   9580226274844099793ull -> 5247200169972194989ull
//   12985280254195429119ull -> 16195574192086600895ull
//   3259928963577250707ull -> 13971810610233433902ull
//   10227163091378049788ull -> 1413803196547873442ull
//   13545372270959858659ull -> 1952174628368416920ull
//   261032876629205419ull -> 16821279773722983085ull
//   10479370283066526297ull -> 3730293276333437051ull
//   1526355699564266223ull -> 5931646053601729348ull
//   272852557971009988ull -> 6622293103480280585ull
//   723137568841207239ull -> 10117757211028084286ull
//   6357384183934834411ull -> 61715527004884585ull
//   12112277347099672678ull -> 4117698574911413933ull
//   3955606011053380392ull -> 7257550778589677645ull
//   11711715509389115267ull -> 336153049891645020ull
//   7912915566230022805ull -> 10004434679436164789ull
//   15121847794654015719ull -> 12518209463246424427ull
//   15174618554357284082ull -> 2102874898517593241ull
//   11557602030571190083ull -> 7311762479881515305ull
// expectation corrected (V19-FIX, kCalcOnOrderFills_hashes, 24 of 24 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the curves, rows and marks did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   5145607761876851736ull -> 13492117694829868536ull
//   6152128856392094028ull -> 12939649384498536620ull
//   13682746943137549482ull -> 18283200234405460746ull
//   4503732430497534158ull -> 17715026773738590286ull
//   11859342531076363326ull -> 16652393141204816958ull
//   14194996582993046802ull -> 10681558687161056850ull
//   5247200169972194989ull -> 12828138156283959725ull
//   16195574192086600895ull -> 13337213902076129151ull
//   13971810610233433902ull -> 2897666538282385134ull
//   1413803196547873442ull -> 4846519892874967202ull
//   1952174628368416920ull -> 4182996206551988888ull
//   16821279773722983085ull -> 3828630408241748525ull
//   3730293276333437051ull -> 6013095397646776827ull
//   5931646053601729348ull -> 1201561623500117444ull
//   6622293103480280585ull -> 16339691134963647305ull
//   10117757211028084286ull -> 1360168785605943262ull
//   61715527004884585ull -> 10446097969456318121ull
//   4117698574911413933ull -> 2226082304527453549ull
//   7257550778589677645ull -> 5506525745160110349ull
//   336153049891645020ull -> 16314828352609565244ull
//   10004434679436164789ull -> 14800719633294876469ull
//   12518209463246424427ull -> 1572183010617309931ull
//   2102874898517593241ull -> 6980933373957006777ull
//   7311762479881515305ull -> 3071340150260882121ull
// expectation corrected (INT26 v19 hash re-pin, kCalcOnOrderFills_hashes, 24 of 24 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the curves, rows and marks did not move:
//   13492117694829868536ull -> 8076983691197291309ull [H-THIN]
//   12939649384498536620ull -> 2440556825337174781ull [H-THIN]
//   18283200234405460746ull -> 17660598049729719549ull [H-THIN]
//   17715026773738590286ull -> 584654546328644269ull [H-THIN]
//   16652393141204816958ull -> 10522330026569309375ull [H-THIN]
//   10681558687161056850ull -> 2930399316675077519ull [H-THIN]
//   12828138156283959725ull -> 17808710347154107934ull [H-THIN]
//   13337213902076129151ull -> 6715617711192628536ull [H-THIN]
//   2897666538282385134ull -> 6587502700890907205ull [H-THIN]
//   4846519892874967202ull -> 2192689119498288561ull [H-THIN]
//   4182996206551988888ull -> 8664008431780515081ull [H-THIN]
//   3828630408241748525ull -> 7930584285132978374ull [H-THIN]
//   6013095397646776827ull -> 17064940335416518660ull [H-THIN]
//   1201561623500117444ull -> 9016627554631535731ull [H-THIN]
//   16339691134963647305ull -> 16088314143365594964ull [H-THIN]
//   1360168785605943262ull -> 13783309705914047335ull [H-THIN]
//   10446097969456318121ull -> 9827199985728418388ull [H-THIN]
//   2226082304527453549ull -> 16177976828038662588ull [H-THIN]
//   5506525745160110349ull -> 12444021166412526756ull [H-THIN]
//   16314828352609565244ull -> 12728919766761920669ull [H-THIN]
//   14800719633294876469ull -> 17328862593452428978ull [H-THIN]
//   1572183010617309931ull -> 565050300121405896ull [H-THIN]
//   6980933373957006777ull -> 12671085486866481742ull [H-THIN]
//   3071340150260882121ull -> 9604569239120914630ull [H-THIN]
constexpr std::uint64_t kCalcOnOrderFills_hashes[] = {
    8076983691197291309ull,
    2440556825337174781ull,
    17660598049729719549ull,
    584654546328644269ull,
    10522330026569309375ull,
    2930399316675077519ull,
    17808710347154107934ull,
    6715617711192628536ull,
    6587502700890907205ull,
    2192689119498288561ull,
    8664008431780515081ull,
    7930584285132978374ull,
    17064940335416518660ull,
    9016627554631535731ull,
    16088314143365594964ull,
    13783309705914047335ull,
    9827199985728418388ull,
    16177976828038662588ull,
    12444021166412526756ull,
    12728919766761920669ull,
    17328862593452428978ull,
    565050300121405896ull,
    12671085486866481742ull,
    9604569239120914630ull,
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
// expectation corrected (v19-E, kSuppressedTail_hashes, 24 of 24 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the curves, rows and marks did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12639766645888297560ull -> 2926214571128404135ull
//   17829204156816530938ull -> 2460311144695717203ull
//   8025178495203387901ull -> 8827033979564876247ull
//   5131852293017663393ull -> 12198137721016629270ull
//   1366831289981335516ull -> 8010034670511875064ull
//   6098945266742256491ull -> 14775446960658454710ull
//   12620064712001914325ull -> 12026164355176752283ull
//   9436544788383326163ull -> 17208159137580538219ull
//   16310238331223777337ull -> 5096664486441078049ull
//   16245567321610464409ull -> 16022104096870556683ull
//   18104293097181184875ull -> 4836076347901812102ull
//   1992390751756887078ull -> 8937674376376720246ull
//   9740657585781373307ull -> 2703551404259163957ull
//   9718527900716965688ull -> 3061368442138030587ull
//   8274551599963368185ull -> 10178081078865716434ull
//   8595352794705645044ull -> 8206897629920030398ull
//   14350650840874626930ull -> 6525045815118222249ull
//   5511030235869260280ull -> 15184055765275961048ull
//   17042293468659343500ull -> 5759341035588050840ull
//   17488769300453378241ull -> 3484237427845144905ull
//   11431951480972000938ull -> 6855851226167457247ull
//   16479222579093225873ull -> 17070412854470623372ull
//   7003778984678674920ull -> 5698741489304391014ull
//   4761381105021282633ull -> 1169937375292956878ull
// expectation corrected (V19-FIX, kSuppressedTail_hashes, 24 of 24 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the curves, rows and marks did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2926214571128404135ull -> 10420073189004151623ull
//   2460311144695717203ull -> 13145042526267837939ull
//   8827033979564876247ull -> 2622782777793463991ull
//   12198137721016629270ull -> 17996446729054892726ull
//   8010034670511875064ull -> 3969181056128787992ull
//   14775446960658454710ull -> 16947117433974038ull
//   12026164355176752283ull -> 423735939073054907ull
//   17208159137580538219ull -> 18056612604689961163ull
//   5096664486441078049ull -> 12445011709498043361ull
//   16022104096870556683ull -> 16382730376298748267ull
//   4836076347901812102ull -> 12718112296054108390ull
//   8937674376376720246ull -> 913929622865489206ull
//   2703551404259163957ull -> 13218761675008378229ull
//   3061368442138030587ull -> 10388057217060506235ull
//   10178081078865716434ull -> 4373273430774112306ull
//   8206897629920030398ull -> 11946915320520331422ull
//   6525045815118222249ull -> 2472677882334572745ull
//   15184055765275961048ull -> 4709222664471492344ull
//   5759341035588050840ull -> 5785035152764109816ull
//   3484237427845144905ull -> 15834701310167426825ull
//   6855851226167457247ull -> 18416054639860847807ull
//   17070412854470623372ull -> 9654818568811123148ull
//   5698741489304391014ull -> 7514399512975361670ull
//   1169937375292956878ull -> 7096719386424781198ull
// expectation corrected (INT26 v19 hash re-pin, kSuppressedTail_hashes, 24 of 24 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the curves, rows and marks did not move:
//   10420073189004151623ull -> 243601838968427296ull [H-THIN]
//   13145042526267837939ull -> 14082045497352522076ull [H-THIN]
//   2622782777793463991ull -> 16788444728701273278ull [H-THIN]
//   17996446729054892726ull -> 4735244315032400709ull [H-THIN]
//   3969181056128787992ull -> 9274703563551556229ull [H-THIN]
//   16947117433974038ull -> 4437548837203718331ull [H-THIN]
//   423735939073054907ull -> 9447280110204500336ull [H-THIN]
//   18056612604689961163ull -> 644150700710845644ull [H-THIN]
//   12445011709498043361ull -> 12856503234686701952ull [H-THIN]
//   16382730376298748267ull -> 8228843622305023308ull [H-THIN]
//   12718112296054108390ull -> 11723584407685805665ull [H-THIN]
//   913929622865489206ull -> 12427688725613086017ull [H-THIN]
//   13218761675008378229ull -> 5717024028566739016ull [H-THIN]
//   10388057217060506235ull -> 9136288938210272546ull [H-THIN]
//   4373273430774112306ull -> 14265105010801500849ull [H-THIN]
//   11946915320520331422ull -> 5913118032193444201ull [H-THIN]
//   2472677882334572745ull -> 2601700310587933770ull [H-THIN]
//   4709222664471492344ull -> 16710877784426826175ull [H-THIN]
//   5785035152764109816ull -> 12222101465241554407ull [H-THIN]
//   15834701310167426825ull -> 1466897115669479138ull [H-THIN]
//   18416054639860847807ull -> 8818039625616895170ull [H-THIN]
//   9654818568811123148ull -> 17382764399684682567ull [H-THIN]
//   7514399512975361670ull -> 5569220102658162789ull [H-THIN]
//   7096719386424781198ull -> 8558103254181104877ull [H-THIN]
constexpr std::uint64_t kSuppressedTail_hashes[] = {
    243601838968427296ull,
    14082045497352522076ull,
    16788444728701273278ull,
    4735244315032400709ull,
    9274703563551556229ull,
    4437548837203718331ull,
    9447280110204500336ull,
    644150700710845644ull,
    12856503234686701952ull,
    8228843622305023308ull,
    11723584407685805665ull,
    12427688725613086017ull,
    5717024028566739016ull,
    9136288938210272546ull,
    14265105010801500849ull,
    5913118032193444201ull,
    2601700310587933770ull,
    16710877784426826175ull,
    12222101465241554407ull,
    1466897115669479138ull,
    8818039625616895170ull,
    17382764399684682567ull,
    5569220102658162789ull,
    8558103254181104877ull,
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
