// R5 lane R4: the Pine adapter's strategy.exit brackets and trailing stops,
// re-lowered onto the kernel's L7 order ergonomics.
//
// What moved: a source trail offset is a TICK COUNT, and the kernel now owns
// that spelling (native_order::TrailTicks). The adapter no longer multiplies
// a tick count by the symbol tick; it hands the kernel the count and the
// acceptance path resolves it against NativeRunSpec::price_tick, which is the
// same syminfo mintick the adapter projects. The TradingView quirks that have
// no kernel analogue stay adapter policy and are exercised here so that they
// keep behaving exactly as they did: the explicit-zero trail rides the
// TICK-QUANTIZED best (half-a-tick sentinel, now spelled in ticks), the
// half-tick arm threshold (source_trigger_threshold), and the exit-leg
// lifecycle rules of src/compat/pine.
//
// This is a NEUTRALITY differential, not a feature witness: every pinned
// number below was harvested from the UNCHANGED adapter on engine main
// 04330d4 by compiling this same translation unit against that tree's
// libpineforge.a with -DPINEFORGE_R4_HARVEST (which prints the observed rows
// as the initializers below instead of checking them). Rebuild them the same
// way; never edit one by hand to make a run pass. A re-lowering that moves any
// TradingView outcome fails here.
//
// Scenarios (the bracket/trail shapes the corpus probes carry, reduced to
// deterministic binary-exact feeds):
//   from-entry-bracket  — strategy.exit with BOTH a limit and a stop bound to
//                         a named entry: the limit takes the profit and the
//                         OCA sibling leaves with the group.
//   from-entry-stopout  — the same bracket where the stop leg settles first.
//   trail-reissue       — trail_points activation with a trail_offset that is
//                         RE-ISSUED at a different distance while the
//                         activation is unchanged: the adapter keeps the live
//                         request (so the kernel still fires on the ORIGINAL
//                         distance) and re-prices the settlement from the
//                         retained extreme and the NEW distance. The pinned
//                         exit price of that row therefore sits outside the
//                         bar that fired it; that is the measured behaviour of
//                         engine main and this witness holds it still (see
//                         the lane report: expressing it as one kernel
//                         replace with ReplaceOptions{retain_trigger_state}
//                         would move it, so it stays adapter policy).
//   trail-zero-offset   — an explicit trail_offset of 0 issued once the
//                         activation is already reached, so the leg is a live
//                         generic Trail riding the TICK-QUANTIZED best (the
//                         half-a-tick sentinel) instead of a one-shot touch.
//   parent-rejection    — a declined in-position MARKET reversal kills the
//                         live position's standing priced bracket, and a
//                         same-(id, from_entry) re-issue revives it at the
//                         fresh prices (src/compat/pine/exit_lifecycle.cpp).
//   cycle-revival       — a bracket re-issued every bar across a completed
//                         position cycle arms the NEW cycle's lot instead of
//                         inheriting the finished cycle's touched legs.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s] %s:%d  %s\n", scenario, __FILE__, line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kT = 1700000000000LL;

Bar mk(int index, double o, double h, double l, double c) {
    Bar bar;
    bar.open = o; bar.high = h; bar.low = l; bar.close = c;
    bar.volume = 1.0;
    bar.timestamp = kT + static_cast<std::int64_t>(index) * 60000;
    return bar;
}

// ── the probe ───────────────────────────────────────────────────────────
enum class Shape {
    FromEntryBracket,
    FromEntryStopout,
    TrailReissue,
    TrailZeroOffset,
    ParentRejection,
    CycleRevival,
};

// A generated-strategy-shaped source host: the body issues Pine commands and
// nothing else. Every price below sits on the 0.01 grid and every level is a
// binary-exact multiple of it, so the run is reproducible to the bit.
class BracketProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit BracketProbe(Shape shape) : shape_(shape) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = 1;
        if (shape == Shape::ParentRejection) {
            // The canonical decline fixture needs the whole equity in the
            // position so the frozen opposite quantity cannot be afforded.
            config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            config.default_qty_value = 100.0;
        } else {
            config.default_qty_type = static_cast<int>(QtyType::FIXED);
            config.default_qty_value = 2.0;
        }
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        margin_call_enabled_ = false;
    }

    // The signed physical position left at the end of the run.
    double signed_units() const noexcept {
        return position_qty_
            * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int bar = bar_index_;
        switch (shape_) {
        case Shape::FromEntryBracket:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) {
                // Both legs of one strategy.exit: an OCA pair bound to "L".
                strategy_exit("X", "L", 102.5, 97.5, kNaN, kNaN, kNaN, 100.0, "tp");
            }
            break;
        case Shape::FromEntryStopout:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) {
                strategy_exit("X", "L", 108.0, 98.5, kNaN, kNaN, kNaN, 100.0, "sl");
            }
            break;
        case Shape::TrailReissue:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) {
                // Same activation (300 ticks above the fill) on every bar; the
                // trailing DISTANCE widens once the extreme is already
                // running, which is the re-issue that must not restart it.
                strategy_exit("T", "L", kNaN, kNaN, /*trail_points=*/300.0,
                              /*trail_offset=*/bar < 5 ? 100.0 : 200.0,
                              kNaN, 100.0, "trail");
            }
            break;
        case Shape::TrailZeroOffset:
            if (bar == 0) strategy_entry("L", true);
            // Issued only once the activation is ALREADY reached at the
            // placement point, which is the shape that keeps a generic Trail
            // riding the best instead of collapsing to a one-shot touch.
            if (bar >= 5) {
                strategy_exit("T", "L", kNaN, kNaN, /*trail_points=*/100.0,
                              /*trail_offset=*/0.0, kNaN, 100.0, "ride");
            }
            break;
        case Shape::ParentRejection:
            if (bar == 0) strategy_entry("L", true);
            if (bar == 2) {
                strategy_exit("X", "L", kNaN, 105.0, kNaN, kNaN, kNaN, 100.0, "");
                strategy_entry("S", false);      // declined at the +1 gap open
            }
            if (bar == 5) {
                // REVIVE-A: the same (id, from_entry) re-issue replaces the
                // dormant bracket wholesale and arms the NEW prices.
                strategy_exit("X", "L", kNaN, 108.0, kNaN, kNaN, kNaN, 100.0, "");
            }
            break;
        case Shape::CycleRevival:
            // One standing bracket definition re-issued on every bar across a
            // completed cycle: the second lot must arm fresh legs.
            if (bar == 0 || bar == 11) strategy_entry("L", true);
            if (bar >= 1) {
                strategy_exit("X", "L", 103.0, 99.0, kNaN, kNaN, kNaN, 100.0, "");
            }
            break;
        }
    }

private:
    Shape shape_;
};

// ── observed / pinned shape ─────────────────────────────────────────────
struct Row {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

struct Observed {
    std::vector<Row> rows;
    double final_equity = 0.0;
    double max_drawdown = 0.0;
    double max_runup = 0.0;
    double position_units = 0.0;
};

// Every scenario runs on the same binary-exact staircase: a rise past the
// bracket levels and the trail activation, a retrace deep enough to take a
// trailing stop, and a flat tail.
std::vector<Bar> feed(Shape shape) {
    std::vector<Bar> bars;
    auto push = [&](double o, double h, double l, double c) {
        bars.push_back(mk(static_cast<int>(bars.size()), o, h, l, c));
    };
    if (shape == Shape::ParentRejection) {
        // The KI-54 decline fixture: LONG the whole equity at 100, the signal
        // bar closes at 110, and the fill bar gaps +1 so the frozen opposite
        // quantity is unaffordable and the reversal is declined.
        push(100.0, 100.0, 100.0, 100.0);
        push(100.0, 100.0, 100.0, 100.0);   // L fills @100
        push(100.0, 112.0,  99.0, 110.0);   // X + S queued
        push(111.0, 112.0, 104.0, 111.0);   // S declined; 105 touched
        push(111.0, 111.0, 111.0, 111.0);
        push(111.0, 111.0, 104.0, 111.0);   // re-issue arms 108
        push(109.0, 109.0, 100.0, 101.0);   // the fresh 108 stop settles
        push(101.0, 101.0, 101.0, 101.0);
        return bars;
    }
    push(100.0, 100.0, 100.0, 100.0);       // 0: entry placed
    push(100.0, 100.5,  99.5, 100.25);      // 1: L fills @100
    push(100.25, 101.0, 100.0, 100.75);     // 2
    push(100.75, 102.0, 100.5, 101.75);     // 3
    push(101.75, 103.0, 101.5, 102.75);     // 4: past 102.5 / activation
    push(102.75, 104.0, 102.5, 103.75);     // 5
    push(103.75, 104.5, 103.0, 103.25);     // 6: extreme, then the retrace
    push(103.25, 103.5, 101.0, 101.25);     // 7: deep retrace
    push(101.25, 101.5,  98.0,  98.25);     // 8: through the 98.5 stop
    push(98.25,  99.0,  97.0,  97.5);       // 9: through the 97.5 limit-side
    push(97.5,   98.0,  97.0,  97.5);       // 10
    push(97.5,   99.5,  97.5,  99.25);      // 11: second-cycle entry ground
    push(99.25, 100.5,  99.0, 100.25);      // 12
    push(100.25, 103.5,  100.0, 103.0);     // 13: second cycle target
    push(103.0, 103.5, 102.5, 103.0);       // 14
    return bars;
}

Observed observe(Shape shape) {
    Observed out;
    BracketProbe probe(shape);
    const auto bars = feed(shape);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());

    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.is_long, t.open_at_end});
    }
    out.final_equity = report.equity_curve_len > 0
        ? report.equity_curve[report.equity_curve_len - 1].equity : 0.0;
    out.max_drawdown = report.metrics.equity.max_equity_drawdown;
    out.max_runup = report.metrics.equity.max_equity_runup;
    BacktestEngine::free_report(&report);
    out.position_units = probe.signed_units();
    return out;
}

#ifdef PINEFORGE_R4_HARVEST

void emit(const char* symbol, Shape shape) {
    const Observed got = observe(shape);
    std::printf("constexpr Row k%s_rows[] = {\n", symbol);
    for (const auto& r : got.rows) {
        std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %.17g, %d, %d},\n",
                    static_cast<long long>(r.entry_time),
                    static_cast<long long>(r.exit_time),
                    r.entry_price, r.exit_price, r.qty, r.pnl,
                    r.is_long, r.open_at_end);
    }
    if (got.rows.empty()) std::printf("    // (no rows)\n");
    std::printf("};\nconstexpr double k%s_final_equity = %.17g;\n", symbol, got.final_equity);
    std::printf("constexpr double k%s_max_drawdown = %.17g;\n", symbol, got.max_drawdown);
    std::printf("constexpr double k%s_max_runup = %.17g;\n", symbol, got.max_runup);
    std::printf("constexpr double k%s_position_units = %.17g;\n\n", symbol, got.position_units);
}

#else

// R4_PINNED_DATA_BEGIN — harvested on engine main 04330d4, unchanged adapter.
constexpr Row kFromEntryBracket_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr double kFromEntryBracket_final_equity = 10005;
constexpr double kFromEntryBracket_max_drawdown = 0;
constexpr double kFromEntryBracket_max_runup = 0;
constexpr double kFromEntryBracket_position_units = 0;

constexpr Row kFromEntryStopout_rows[] = {
    {1700000060000LL, 1700000480000LL, 100, 98.5, 2, -3, 1, 0},
};
constexpr double kFromEntryStopout_final_equity = 9997;
constexpr double kFromEntryStopout_max_drawdown = 10.5;
constexpr double kFromEntryStopout_max_runup = 0;
constexpr double kFromEntryStopout_position_units = 0;

constexpr Row kTrailReissue_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 102, 2, 4, 1, 0},
};
constexpr double kTrailReissue_final_equity = 10004;
constexpr double kTrailReissue_max_drawdown = 3.5;
constexpr double kTrailReissue_max_runup = 0;
constexpr double kTrailReissue_position_units = 0;

constexpr Row kTrailZeroOffset_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 103.75, 2, 7.5, 1, 0},
};
constexpr double kTrailZeroOffset_final_equity = 10007.5;
constexpr double kTrailZeroOffset_max_drawdown = 0;
constexpr double kTrailZeroOffset_max_runup = 0;
constexpr double kTrailZeroOffset_position_units = 0;

constexpr Row kParentRejection_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 108, 100, 800, 1, 0},
};
constexpr double kParentRejection_final_equity = 10800;
constexpr double kParentRejection_max_drawdown = 300;
constexpr double kParentRejection_max_runup = 0;
constexpr double kParentRejection_position_units = 0;

constexpr Row kCycleRevival_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 103, 2, 6, 1, 0},
    {1700000720000LL, 1700000720000LL, 99.25, 99, 2, -0.5, 1, 0},
};
constexpr double kCycleRevival_final_equity = 10005.5;
constexpr double kCycleRevival_max_drawdown = 0.5;
constexpr double kCycleRevival_max_runup = 0;
constexpr double kCycleRevival_position_units = 0;
// R4_PINNED_DATA_END

struct Expected {
    const char* name;
    Shape shape;
    const Row* rows;
    std::size_t rows_len;
    double final_equity;
    double max_drawdown;
    double max_runup;
    double position_units;
    // The shape assertion: a scenario that degenerates into "no bracket ran"
    // would otherwise pin an empty answer and witness nothing.
    std::size_t min_rows;
};

constexpr Expected kScenarios[] = {
    {"from-entry-bracket", Shape::FromEntryBracket, kFromEntryBracket_rows,
     std::size(kFromEntryBracket_rows), kFromEntryBracket_final_equity,
     kFromEntryBracket_max_drawdown, kFromEntryBracket_max_runup,
     kFromEntryBracket_position_units, 1},
    {"from-entry-stopout", Shape::FromEntryStopout, kFromEntryStopout_rows,
     std::size(kFromEntryStopout_rows), kFromEntryStopout_final_equity,
     kFromEntryStopout_max_drawdown, kFromEntryStopout_max_runup,
     kFromEntryStopout_position_units, 1},
    {"trail-reissue", Shape::TrailReissue, kTrailReissue_rows,
     std::size(kTrailReissue_rows), kTrailReissue_final_equity,
     kTrailReissue_max_drawdown, kTrailReissue_max_runup,
     kTrailReissue_position_units, 1},
    {"trail-zero-offset", Shape::TrailZeroOffset, kTrailZeroOffset_rows,
     std::size(kTrailZeroOffset_rows), kTrailZeroOffset_final_equity,
     kTrailZeroOffset_max_drawdown, kTrailZeroOffset_max_runup,
     kTrailZeroOffset_position_units, 1},
    {"parent-rejection", Shape::ParentRejection, kParentRejection_rows,
     std::size(kParentRejection_rows), kParentRejection_final_equity,
     kParentRejection_max_drawdown, kParentRejection_max_runup,
     kParentRejection_position_units, 1},
    {"cycle-revival", Shape::CycleRevival, kCycleRevival_rows,
     std::size(kCycleRevival_rows), kCycleRevival_final_equity,
     kCycleRevival_max_drawdown, kCycleRevival_max_runup,
     kCycleRevival_position_units, 2},
};

void compare(const Expected& pinned) {
    scenario = pinned.name;
    const Observed got = observe(pinned.shape);
    CHECK(got.rows.size() >= pinned.min_rows);
    CHECK(got.rows.size() == pinned.rows_len);
    const std::size_t n = got.rows.size() < pinned.rows_len ? got.rows.size()
                                                            : pinned.rows_len;
    for (std::size_t i = 0; i < n; ++i) {
        const Row& left = got.rows[i];
        const Row& right = pinned.rows[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(left.entry_price == right.entry_price);
        CHECK(left.exit_price == right.exit_price);
        CHECK(left.qty == right.qty);
        CHECK(left.pnl == right.pnl);
        CHECK(left.is_long == right.is_long);
        CHECK(left.open_at_end == right.open_at_end);
    }
    CHECK(got.final_equity == pinned.final_equity);
    CHECK(got.max_drawdown == pinned.max_drawdown);
    CHECK(got.max_runup == pinned.max_runup);
    CHECK(got.position_units == pinned.position_units);
}

// The kernel owns the tick spelling now, so the run spec the adapter projects
// must carry the very tick the acceptance path resolves a TrailTicks against:
// if these two ever drift apart the adapter's trailing legs would be rejected
// instead of silently re-priced.
void adapter_projects_the_tick_the_kernel_resolves_against() {
    scenario = "price-grid";
    BracketProbe probe(Shape::TrailReissue);
    const auto bars = feed(Shape::TrailReissue);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    const auto state = probe.native_state();
    CHECK(state.spec != nullptr);
    if (state.spec) CHECK(state.spec->price_tick == 0.01);
}

#endif  // PINEFORGE_R4_HARVEST

// ═══════════════════════════════════════════════════════════════════════
// R5 lane R4d: the RELATIVE from_entry legs on the kernel's anchored legs.
//
// A strategy.exit whose profit / loss / trail_points operand cannot resolve
// yet (its parent entry has not filled) used to wait in the adapter's own
// queue until the parent's fill re-ran the whole exit() pipeline. It is now
// also submitted as the parent's anchored bracket child
// (native_order::FromOwnerFill in ticks, Directional rounding,
// WaitForApplied{parent, PendingUntilArmed, AfterArmPrint, Book}, a
// host-sized close); the kernel arms it at the fill, the adapter restates
// TradingView's projection of the kernel's ladder point in
// resolve_anchored_level, and the fill-point re-run ADOPTS the armed request
// when it is the very request that re-run would have submitted.
//
// Every scenario below queues at least one relative operand against a parent
// that is still pending. The pinned facts are the closed rows, the equity
// extremes, the final position AND a per-bar digest of the source pending
// book (observe_pending_copy_v1: ids, levels, tick operands, quantities,
// creation bar / sequence, reservation, position side) folded together with
// the position the script sees, so "the same book as before at every point"
// is checked, not only the trades. Request incarnations are deliberately not
// folded: an anchored child is accepted when its parent is, not when it
// fills.
//
// R4D_PINNED_DATA was harvested from the UNCHANGED adapter on the wave-5
// integration tip 577315a: this translation unit compiled with
// -DPINEFORGE_R4D_HARVEST against that tree's headers and libpineforge.a.
// Rebuild it the same way; never edit a value by hand.
namespace r4d {

enum class Shape {
    RelBracketTp,
    RelBracketSl,
    RelBracketEveryBar,
    RelExitBeforeEntry,
    RelTrailOneShot,
    RelTrailOffset,
    RelTrailZero,
    RelShort,
    RelThreeWay,
    RelLimitParent,
    RelParentCancel,
    RelPyramid,
    RelDeclined,
    RelOcaName,
    RelSlippage,
    RelPartial,
    RelReversal,
    RelPyramidOtherId,
    RelOrderAdd,
    RelPartialClose,
    RelDeclinedFlat,
    RelCancelAll,
    RelCancelExitId,
    RelStopParent,
    RelNegativeShort,
    RelTwoExits,
    RelGapFill,
    RelShortTrail,
    RelProfitOnly,
    RelQtyExplicit,
    RelReissueChanged,
    RelSharedOca,
    RelBreakoutPair,
    RelBreakoutPairSetOnce,
    RelPyramidSetOnce,
};

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    }
    void u(std::uint64_t v) { bytes(&v, sizeof v); }
    void i(std::int64_t v) { bytes(&v, sizeof v); }
    void d(double v) {
        // One NaN spelling: the payload of a quiet NaN is not a source fact.
        if (std::isnan(v)) { u(0x7ff8000000000000ULL); return; }
        bytes(&v, sizeof v);
    }
    void s(const char* v) { bytes(v, std::strlen(v)); u(0xffULL); }
};

// How the scenario is scheduled. Plain is the ordinary one-calculation-per-bar
// run; Magnifier replays every chart bar as two lower-timeframe bars, so a
// parent fills and its legs arm on a sub-bar path; the two order-processing
// switches are the modes this lane leaves on the adapter's own queue.
enum class Mode { Plain, Magnifier, CalcOnOrderFills, ProcessOnClose };

class Probe final : public pineforge::source::PineStrategyHost {
public:
    Probe(Shape shape, Mode mode) : shape_(shape) {
        pineforge::source::PineStrategyConfig config;
        config.calc_on_order_fills = mode == Mode::CalcOnOrderFills;
        config.process_orders_on_close = mode == Mode::ProcessOnClose;
        config.initial_capital = 10000.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.slippage = shape == Shape::RelSlippage ? 2 : 0;
        config.pyramiding = shape == Shape::RelPyramid || shape == Shape::RelPyramidOtherId
                || shape == Shape::RelPyramidSetOnce
            ? 2 : 1;
        if (shape == Shape::RelDeclined || shape == Shape::RelDeclinedFlat) {
            config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            config.default_qty_value = 100.0;
        } else {
            config.default_qty_type = static_cast<int>(QtyType::FIXED);
            config.default_qty_value = 2.0;
        }
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        margin_call_enabled_ = false;
    }

    double signed_units() const noexcept {
        return position_qty_ * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }
#ifndef PINEFORGE_R4D_HARVEST
    // The lane's own witness (absent from the 577315a headers, which is this
    // unit's fail-before diagnostic): how many relative legs ran on the
    // kernel's anchored bracket legs.
    pineforge::source::PineExecutionAdapter::AnchoredRelativeStats anchored() const noexcept {
        return adapter_.anchored_relative_stats();
    }
#endif
    std::uint64_t book_digest() const noexcept { return book_.h; }
    const std::vector<std::string>& book_lines() const noexcept { return lines_; }

    void on_source_bar(const Bar&) override {
        observe_book();
        const int bar = bar_index_;
        switch (shape_) {
        case Shape::RelBracketTp:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "tp", kNaN, "",
                              /*profit=*/250.0, /*loss=*/250.0);
            }
            break;
        case Shape::RelBracketSl:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "sl", kNaN, "",
                              800.0, 150.0);
            }
            break;
        case Shape::RelBracketEveryBar:
            if (bar == 0) strategy_entry("L", true);
            strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          250.0, 250.0);
            break;
        case Shape::RelExitBeforeEntry:
            // The exit is declared on every bar, long before its entry exists.
            strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          300.0, 100.0);
            if (bar == 2 || bar == 10) strategy_entry("L", true);
            break;
        case Shape::RelTrailOneShot:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("T", "L", kNaN, kNaN, /*trail_points=*/250.0, kNaN, kNaN, 100.0,
                              "one-shot");
            }
            break;
        case Shape::RelTrailOffset:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("T", "L", kNaN, kNaN, 300.0, /*trail_offset=*/100.0, kNaN, 100.0,
                              "trail");
            }
            break;
        case Shape::RelTrailZero:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("T", "L", kNaN, kNaN, 100.0, 0.0, kNaN, 100.0, "ride");
            }
            break;
        case Shape::RelShort:
            if (bar == 0) {
                strategy_entry("S", false);
                strategy_exit("X", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              250.0, 150.0);
            }
            break;
        case Shape::RelThreeWay:
            // The corpus's pending-parent shape (bracket-exit-three-way-set-
            // once-entry-01 and its three siblings): absolute stop + limit
            // placed with the entry, the relative trail activation queued.
            if (bar == 0 || bar == 10) {
                strategy_entry("L", true);
                strategy_exit("X", "L", bar == 0 ? 104.25 : 103.25, bar == 0 ? 98.5 : 96.0,
                              /*trail_points=*/300.0, kNaN, kNaN, 100.0, "3-way");
            }
            break;
        case Shape::RelLimitParent:
            // A resting limit parent re-issued with its relative bracket on
            // every bar; it fills intrabar, so the legs arm on the path.
            if (signed_units() == 0.0 && bar < 8) strategy_entry("L", true, 99.75);
            strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          200.0, 100.0);
            break;
        case Shape::RelParentCancel:
            if (bar == 0) {
                strategy_entry("L", true, 90.0);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              250.0, 150.0);
            }
            if (bar == 2) strategy_cancel("L");
            if (bar == 3) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              100.0, 150.0);
            }
            break;
        case Shape::RelPyramid:
            if (bar == 0 || bar == 2) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 400.0);
            }
            break;
        case Shape::RelDeclined:
            if (bar == 0) strategy_entry("L", true);
            if (bar == 2) {
                // The reversal is declined at the +1 gap open (KI-54), so its
                // relative bracket never gets a fill to resolve against.
                strategy_entry("S", false);
                strategy_exit("SX", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              300.0, 300.0);
            }
            if (bar == 4) strategy_close("L");
            break;
        case Shape::RelOcaName:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "grp",
                              250.0, 250.0);
            }
            break;
        case Shape::RelSlippage:
            // Two ticks of slippage against a one-tick stop: the level is
            // already behind the fill print when the leg is born.
            if (bar == 0 || bar == 9) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              300.0, 1.0);
            }
            break;
        case Shape::RelPartial:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X1", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 50.0, "", kNaN, "",
                              150.0, 300.0);
                strategy_exit("X2", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              350.0, 300.0);
            }
            break;
        case Shape::RelReversal:
            if (bar == 0) strategy_entry("L", true);
            if (bar == 5) {
                strategy_entry("S", false);
                strategy_exit("SX", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 100.0);
            }
            break;
        case Shape::RelPyramidOtherId:
            // The bracket is set once for "L"; a second id adds to the book
            // afterwards, so the bracket's lot is no longer the whole position.
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 400.0);
            }
            if (bar == 2) strategy_entry("L2", true);
            break;
        case Shape::RelOrderAdd:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 400.0);
            }
            if (bar == 2) strategy_order("A", true, 1.0);
            break;
        case Shape::RelPartialClose:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 400.0);
            }
            if (bar == 2) strategy_close("L", "half", kNaN, 50.0);
            break;
        case Shape::RelDeclinedFlat:
            // The whole equity at the signal close cannot be afforded at the
            // +1 gap open: the parent is declined and its bracket never arms.
            if (bar == 2) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              300.0, 300.0);
            }
            if (bar == 4) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              200.0, 700.0);
            }
            break;
        case Shape::RelCancelAll:
            if (bar == 0) {
                strategy_entry("L", true, 90.0);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              250.0, 150.0);
            }
            if (bar == 2) strategy_cancel_all();
            if (bar == 3) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              100.0, 150.0);
            }
            break;
        case Shape::RelCancelExitId:
            if (bar == 0) {
                strategy_entry("L", true, 99.75);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              250.0, 150.0);
                strategy_cancel("X");
            }
            if (bar == 6) strategy_close("L");
            break;
        case Shape::RelStopParent:
            // A buy-stop parent crossed intrabar: the legs arm mid-path.
            if (bar == 0) {
                strategy_entry("L", true, kNaN, 100.75);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              150.0, 50.0);
            }
            break;
        case Shape::RelNegativeShort:
            // A short's profit target far below zero is no limit leg at all.
            if (bar == 0) {
                strategy_entry("S", false);
                strategy_exit("X", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              20000.0, 150.0);
            }
            break;
        case Shape::RelTwoExits:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X1", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              150.0, 300.0);
                strategy_exit("X2", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              350.0, 300.0);
            }
            break;
        case Shape::RelGapFill:
            // The target sits inside the next bar's opening gap.
            if (bar == 6) {
                strategy_entry("S", false);
                strategy_exit("X", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              20.0, 20.0);
            }
            break;
        case Shape::RelShortTrail:
            if (bar == 6) {
                strategy_entry("S", false);
                strategy_exit("T", "S", kNaN, kNaN, 300.0, 100.0, kNaN, 100.0, "trail");
            }
            break;
        case Shape::RelProfitOnly:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              250.0, kNaN);
            }
            break;
        case Shape::RelQtyExplicit:
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", /*qty=*/1.0, "",
                              250.0, 250.0);
            }
            break;
        case Shape::RelSharedOca:
            // Two parents whose brackets share one OCA name: the first
            // bracket's fill cancels the group while the second parent still
            // rests, so its anchored children leave with it and are anchored
            // again on the next evaluation.
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("XL", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "g",
                              150.0, 300.0);
                strategy_entry("S", false, kNaN, /*stop=*/97.25);
                strategy_exit("XS", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "g",
                              100.0, 100.0);
            }
            break;
        case Shape::RelBreakoutPair:
            // The breakout pair: a buy stop above and a sell stop below, each
            // with its own relative bracket, all re-issued on every flat bar.
            if (signed_units() == 0.0) {
                strategy_entry("L", true, kNaN, /*stop=*/101.25);
                strategy_entry("S", false, kNaN, /*stop=*/99.25);
            }
            strategy_exit("XL", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          150.0, 100.0);
            strategy_exit("XS", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          150.0, 100.0);
            break;
        case Shape::RelBreakoutPairSetOnce:
            if (bar == 0) {
                strategy_entry("L", true, kNaN, /*stop=*/101.25);
                strategy_entry("S", false, kNaN, /*stop=*/99.25);
                strategy_exit("XL", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              150.0, 100.0);
                strategy_exit("XS", "S", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              150.0, 100.0);
            }
            break;
        case Shape::RelPyramidSetOnce:
            // R5 gap lane N13. The bracket is set ONCE for "L" and a second
            // "L" entry adds a lot afterwards without re-issuing it: the
            // source leg closes the book, so both lots leave at the target.
            // A child bound to the first parent's own lot leaves the add
            // open (the R4d spelling did; see RelPyramidSetOnce's pin note).
            if (bar == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                              400.0, 400.0);
            }
            if (bar == 2) strategy_entry("L", true);
            break;
        case Shape::RelReissueChanged:
            // The queued definition changes while its limit parent still rests.
            if (signed_units() == 0.0 && bar < 8) strategy_entry("L", true, 97.25);
            strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "", kNaN, "",
                          100.0 + 10.0 * bar, 100.0);
            break;
        }
    }

private:
    // The pending book and the position exactly as the script body finds them.
    void observe_book() {
        const int count = observe_pending_count_v1();
        const double position = signed_units();
        book_.i(bar_index_); book_.i(count); book_.d(position);
        char line[512];
        std::snprintf(line, sizeof line, "bar=%d pos=%.17g pending=%d", bar_index_, position,
                      count);
        lines_.emplace_back(line);
        for (int index = 0; index < count; ++index) {
            pf_pending_order_v1_t row{};
            if (observe_pending_copy_v1(index, &row) != 0) { book_.i(-1); continue; }
            book_.s(row.id); book_.s(row.from_entry); book_.i(row.type); book_.i(row.is_long);
            book_.d(row.limit_price); book_.d(row.stop_price); book_.d(row.trail_points);
            book_.d(row.trail_price); book_.d(row.trail_offset); book_.d(row.profit_ticks);
            book_.d(row.loss_ticks); book_.d(row.qty); book_.d(row.qty_percent);
            book_.s(row.oca_name); book_.i(row.created_bar); book_.i(row.created_seq);
            book_.i(row.created_position_side); book_.i(row.created_position_cycle_seq);
            book_.d(row.sizing_price); book_.d(row.sizing_equity);
            book_.i(row.quantity_reservation_present); book_.d(row.quantity_reservation_units);
            book_.d(row.legs_definition_limit_price); book_.d(row.legs_definition_stop_price);
            book_.d(row.legs_definition_trail_price); book_.i(row.dormant_bracket);
            std::snprintf(line, sizeof line,
                          "  [%d] id=%s from=%s type=%d limit=%.17g stop=%.17g tp=%.17g "
                          "tprice=%.17g toff=%.17g profit=%.17g loss=%.17g qty=%.17g pct=%.17g "
                          "bar=%d seq=%lld side=%d resv=%.17g",
                          index, row.id, row.from_entry, row.type, row.limit_price,
                          row.stop_price, row.trail_points, row.trail_price, row.trail_offset,
                          row.profit_ticks, row.loss_ticks, row.qty, row.qty_percent,
                          row.created_bar, static_cast<long long>(row.created_seq),
                          row.created_position_side, row.quantity_reservation_units);
            lines_.emplace_back(line);
        }
    }

    Shape shape_;
    Fnv book_;
    std::vector<std::string> lines_;
};

std::vector<Bar> feed(Shape shape) {
    if (shape == Shape::RelDeclined || shape == Shape::RelDeclinedFlat)
        return ::feed(::Shape::ParentRejection);
    return ::feed(::Shape::FromEntryBracket);
}

struct Observed {
    std::vector<Row> rows;
    double final_equity = 0.0;
    double max_drawdown = 0.0;
    double max_runup = 0.0;
    double position_units = 0.0;
    std::uint64_t book = 0;
    std::vector<std::string> lines;
    std::uint64_t anchored = 0;
    std::uint64_t adopted = 0;
    std::uint64_t withdrawn = 0;
};

// Two lower bars per chart bar, every price taken from the chart bar itself so
// the run stays binary-exact: a rising bar dips first, a falling bar peaks
// first.
std::vector<Bar> lower_feed(const std::vector<Bar>& chart) {
    std::vector<Bar> lower;
    for (std::size_t i = 0; i < chart.size(); ++i) {
        const Bar& bar = chart[i];
        const bool rising = bar.close >= bar.open;
        Bar first = bar;
        Bar second = bar;
        if (rising) {
            first.high = bar.open; first.low = bar.low; first.close = bar.low;
            second.open = bar.low; second.low = bar.low;
        } else {
            first.low = bar.open; first.high = bar.high; first.close = bar.high;
            second.open = bar.high; second.high = bar.high;
        }
        first.timestamp = static_cast<std::int64_t>(i) * 120000;
        second.timestamp = first.timestamp + 60000;
        lower.push_back(first);
        lower.push_back(second);
    }
    return lower;
}

Observed observe(Shape shape, Mode mode) {
    Observed out;
    Probe probe(shape, mode);
    const auto bars = feed(shape);
    if (mode == Mode::Magnifier) {
        const auto lower = lower_feed(bars);
        probe.run(lower.data(), static_cast<int>(lower.size()), "1", "2",
                  /*bar_magnifier=*/true, /*magnifier_samples=*/4,
                  MagnifierDistribution::ENDPOINTS);
    } else {
        probe.run(bars.data(), static_cast<int>(bars.size()));
    }
#ifndef PINEFORGE_R4D_HARVEST
    out.anchored = probe.anchored().anchored;
    out.adopted = probe.anchored().adopted;
    out.withdrawn = probe.anchored().withdrawn;
#endif
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) std::printf("  run error: %s\n", probe.last_error().c_str());
    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.is_long, t.open_at_end});
    }
    out.final_equity = report.equity_curve_len > 0
        ? report.equity_curve[report.equity_curve_len - 1].equity : 0.0;
    out.max_drawdown = report.metrics.equity.max_equity_drawdown;
    out.max_runup = report.metrics.equity.max_equity_runup;
    BacktestEngine::free_report(&report);
    out.position_units = probe.signed_units();
    out.book = probe.book_digest();
    out.lines = probe.book_lines();
    return out;
}

// The lane's own facts per scenario: how many relative legs were placed as
// anchored kernel children, how many of them the parent's fill point adopted
// as its request, and how many left instead. They are NOT harvested from the
// base (it has no anchored legs); they pin which scenarios really run on the
// kernel primitive and which the adapter keeps on its own queue, so a hook
// that stops reproducing the level, or a predicate that stops holding, fails
// here even though the fallback keeps every trade identical.
struct Legs {
    std::uint64_t anchored;
    std::uint64_t adopted;
    std::uint64_t withdrawn;
};

struct Named {
    const char* symbol;
    const char* name;
    Shape shape;
    std::size_t min_rows;
    Legs legs;
    Mode mode = Mode::Plain;
};

// R5 gap lane N13 re-pinned the {anchored, adopted, withdrawn} counts below.
// The anchored child is now the request exit() submits at the parent's fill
// (a host-sized close, NativeArmScope::Book, NativeArmFirstMatch::
// AfterArmPrint), so it is that request for every quantity and every book
// shape, and the hook restates the kernel's own ladder point. No closed row,
// equity figure or book digest moved; only these counts did:
//   rel-declined        expectation corrected: {0,0,0} -> {2,0,2}, because a
//                       reversal parent is anchorable (the arm binds the book
//                       the reversal leaves) and the kernel itself retires
//                       the two children when the parent is declined.
//   rel-reversal        expectation corrected: {0,0,0} -> {2,2,0}, because of
//                       the same reversal-owner measurement: no kernel fact
//                       was missing, the flat-only predicate was.
//   rel-partial         expectation corrected: {0,0,0} -> {4,4,0}, because a
//                       Book child is host-sized: the 50 % leg and its
//                       remainder sibling are sized by the same terms code
//                       that sizes the fill-point request.
//   rel-slippage, mag-rel-slippage
//                       expectation corrected: {4,0,4} -> {4,4,0}, because
//                       AfterArmPrint gives the armed child the birth rule of
//                       a callback-born leg, so a level the fill print already
//                       satisfies no longer forces the fallback.
//   coof-rel-bracket-tp, coof-rel-trail-offset, pooc-rel-bracket-tp,
//   pooc-rel-short      expectation corrected: {0,0,0} -> {2,2,0} / {1,1,0},
//                       because the mode predicate was unmeasured: the hook
//                       shares exit()'s projection (a raw on-grid limit under
//                       calc_on_order_fills) and adoption is bit-exact.
//   rel-breakout-pair, mag-rel-breakout-pair
//                       expectation corrected: {40,14,26} -> {40,14,24},
//   rel-breakout-pair-set-once
//                       expectation corrected: {6,4,2} -> {4,4,0}, because the
//                       resting opposite parent keeps its children while the
//                       other side is in position (no flat-only withdrawal).
// Retained off the kernel path, each with its measurement: rel-qty-explicit
// (an explicit quantity is staged per origin by exit() and submitted at a
// later flush, so the fill point has no request for a child to be) and
// rel-cancel-exit-id (strategy.cancel removes the definition inside the same
// evaluation: no leg exists to place). The withdrawals that remain are the
// kernel cancelling a replaced, cancelled or declined parent's children
// (rel-limit-parent, rel-reissue-changed, the breakout pair, ...), a zero-
// capacity sibling (rel-two-exits) and an unrepresentable level
// (rel-negative-short).
constexpr Named kShapes[] = {
    {"RelBracketTp", "rel-bracket-tp", Shape::RelBracketTp, 1, {2, 2, 0}},
    {"RelBracketSl", "rel-bracket-sl", Shape::RelBracketSl, 1, {2, 2, 0}},
    {"RelBracketEveryBar", "rel-bracket-every-bar", Shape::RelBracketEveryBar, 1, {2, 2, 0}},
    {"RelExitBeforeEntry", "rel-exit-before-entry", Shape::RelExitBeforeEntry, 1, {4, 4, 0}},
    {"RelTrailOneShot", "rel-trail-one-shot", Shape::RelTrailOneShot, 1, {1, 1, 0}},
    {"RelTrailOffset", "rel-trail-offset", Shape::RelTrailOffset, 1, {1, 1, 0}},
    {"RelTrailZero", "rel-trail-zero", Shape::RelTrailZero, 1, {1, 1, 0}},
    {"RelShort", "rel-short", Shape::RelShort, 1, {2, 2, 0}},
    {"RelThreeWay", "rel-three-way", Shape::RelThreeWay, 1, {2, 2, 0}},
    {"RelLimitParent", "rel-limit-parent", Shape::RelLimitParent, 1, {12, 4, 8}},
    {"RelParentCancel", "rel-parent-cancel", Shape::RelParentCancel, 1, {4, 2, 2}},
    {"RelPyramid", "rel-pyramid", Shape::RelPyramid, 1, {2, 2, 0}},
    {"RelDeclined", "rel-declined", Shape::RelDeclined, 1, {2, 0, 2}},
    {"RelOcaName", "rel-oca-name", Shape::RelOcaName, 1, {2, 2, 0}},
    {"RelSlippage", "rel-slippage", Shape::RelSlippage, 1, {4, 4, 0}},
    {"RelPartial", "rel-partial", Shape::RelPartial, 1, {4, 4, 0}},
    {"RelReversal", "rel-reversal", Shape::RelReversal, 1, {2, 2, 0}},
    {"RelPyramidOtherId", "rel-pyramid-other-id", Shape::RelPyramidOtherId, 1, {2, 2, 0}},
    {"RelOrderAdd", "rel-order-add", Shape::RelOrderAdd, 1, {2, 2, 0}},
    {"RelPartialClose", "rel-partial-close", Shape::RelPartialClose, 1, {2, 2, 0}},
    {"RelDeclinedFlat", "rel-declined-flat", Shape::RelDeclinedFlat, 1, {4, 2, 2}},
    {"RelCancelAll", "rel-cancel-all", Shape::RelCancelAll, 1, {4, 2, 2}},
    {"RelCancelExitId", "rel-cancel-exit-id", Shape::RelCancelExitId, 1, {0, 0, 0}},
    {"RelStopParent", "rel-stop-parent", Shape::RelStopParent, 1, {2, 2, 0}},
    {"RelNegativeShort", "rel-negative-short", Shape::RelNegativeShort, 1, {2, 1, 1}},
    {"RelTwoExits", "rel-two-exits", Shape::RelTwoExits, 1, {4, 2, 2}},
    {"RelGapFill", "rel-gap-fill", Shape::RelGapFill, 1, {2, 2, 0}},
    {"RelShortTrail", "rel-short-trail", Shape::RelShortTrail, 1, {1, 1, 0}},
    {"RelProfitOnly", "rel-profit-only", Shape::RelProfitOnly, 1, {1, 1, 0}},
    {"RelQtyExplicit", "rel-qty-explicit", Shape::RelQtyExplicit, 1, {0, 0, 0}},
    {"RelReissueChanged", "rel-reissue-changed", Shape::RelReissueChanged, 1, {18, 2, 16}},
    {"RelSharedOca", "rel-shared-oca", Shape::RelSharedOca, 1, {6, 4, 2}},
    {"RelBreakoutPair", "rel-breakout-pair", Shape::RelBreakoutPair, 1, {40, 14, 24}},
    {"RelBreakoutPairSetOnce", "rel-breakout-pair-set-once", Shape::RelBreakoutPairSetOnce, 1,
     {4, 4, 0}},
    {"MagRelBreakoutPair", "mag-rel-breakout-pair", Shape::RelBreakoutPair, 1, {40, 14, 24},
     Mode::Magnifier},
    {"MagRelBracketTp", "mag-rel-bracket-tp", Shape::RelBracketTp, 1, {2, 2, 0}, Mode::Magnifier},
    {"MagRelBracketSl", "mag-rel-bracket-sl", Shape::RelBracketSl, 1, {2, 2, 0}, Mode::Magnifier},
    {"MagRelTrailOffset", "mag-rel-trail-offset", Shape::RelTrailOffset, 1, {1, 1, 0}, Mode::Magnifier},
    {"MagRelTrailOneShot", "mag-rel-trail-one-shot", Shape::RelTrailOneShot, 1, {1, 1, 0}, Mode::Magnifier},
    {"MagRelLimitParent", "mag-rel-limit-parent", Shape::RelLimitParent, 1, {12, 4, 8}, Mode::Magnifier},
    {"MagRelStopParent", "mag-rel-stop-parent", Shape::RelStopParent, 1, {2, 2, 0}, Mode::Magnifier},
    {"MagRelShort", "mag-rel-short", Shape::RelShort, 1, {2, 2, 0}, Mode::Magnifier},
    {"MagRelSlippage", "mag-rel-slippage", Shape::RelSlippage, 1, {4, 4, 0}, Mode::Magnifier},
    {"CoofRelBracketTp", "coof-rel-bracket-tp", Shape::RelBracketTp, 1, {2, 2, 0}, Mode::CalcOnOrderFills},
    {"CoofRelTrailOffset", "coof-rel-trail-offset", Shape::RelTrailOffset, 1, {1, 1, 0}, Mode::CalcOnOrderFills},
    {"PoocRelBracketTp", "pooc-rel-bracket-tp", Shape::RelBracketTp, 1, {2, 2, 0}, Mode::ProcessOnClose},
    {"PoocRelShort", "pooc-rel-short", Shape::RelShort, 1, {2, 2, 0}, Mode::ProcessOnClose},
    {"RelPyramidSetOnce", "rel-pyramid-set-once", Shape::RelPyramidSetOnce, 2, {2, 2, 0}},
    {"MagRelPyramidSetOnce", "mag-rel-pyramid-set-once", Shape::RelPyramidSetOnce, 2, {2, 2, 0},
     Mode::Magnifier},
};

#ifdef PINEFORGE_R4D_HARVEST

void harvest() {
    std::printf("// harvested: paste between R4D_PINNED_DATA_BEGIN/END\n");
    for (const Named& named : kShapes) {
        scenario = named.name;
        const Observed got = observe(named.shape, named.mode);
        std::printf("constexpr Row k%s_rows[] = {\n", named.symbol);
        for (const auto& r : got.rows) {
            std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %.17g, %d, %d},\n",
                        static_cast<long long>(r.entry_time),
                        static_cast<long long>(r.exit_time),
                        r.entry_price, r.exit_price, r.qty, r.pnl, r.is_long, r.open_at_end);
        }
        std::printf("};\nconstexpr Pinned k%s = {k%s_rows, %zu, %.17g, %.17g, %.17g, %.17g, "
                    "0x%016llxULL};\n\n",
                    named.symbol, named.symbol, got.rows.size(), got.final_equity,
                    got.max_drawdown, got.max_runup, got.position_units,
                    static_cast<unsigned long long>(got.book));
    }
}

#else

struct Pinned {
    const Row* rows;
    std::size_t rows_len;
    double final_equity;
    double max_drawdown;
    double max_runup;
    double position_units;
    std::uint64_t book;
};

// R4D_PINNED_DATA_BEGIN — harvested on 577315a, unchanged adapter.
constexpr Row kRelBracketTp_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kRelBracketTp = {kRelBracketTp_rows, 1, 10005, 0, 0, 0, 0xbbefba563f16c3aeULL};

constexpr Row kRelBracketSl_rows[] = {
    {1700000060000LL, 1700000480000LL, 100, 98.5, 2, -3, 1, 0},
};
constexpr Pinned kRelBracketSl = {kRelBracketSl_rows, 1, 9997, 10.5, 0, 0, 0xceca690bb7e934a6ULL};

constexpr Row kRelBracketEveryBar_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kRelBracketEveryBar = {kRelBracketEveryBar_rows, 1, 10005, 0, 0, 0, 0xb2ecb9b0e0661bdbULL};

constexpr Row kRelExitBeforeEntry_rows[] = {
    {1700000180000LL, 1700000300000LL, 100.75, 103.75, 2, 6, 1, 0},
    {1700000660000LL, 1700000720000LL, 97.5, 100.5, 2, 6, 1, 0},
};
constexpr Pinned kRelExitBeforeEntry = {kRelExitBeforeEntry_rows, 2, 10012, 0, 0, 0, 0x38323ef3655f7398ULL};

constexpr Row kRelTrailOneShot_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kRelTrailOneShot = {kRelTrailOneShot_rows, 1, 10005, 0, 0, 0, 0x5bdd541ccbd85cd8ULL};

constexpr Row kRelTrailOffset_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 103, 2, 6, 1, 0},
};
constexpr Pinned kRelTrailOffset = {kRelTrailOffset_rows, 1, 10006, 1.5, 0, 0, 0x12d56ad14b30e01dULL};

constexpr Row kRelTrailZero_rows[] = {
    {1700000060000LL, 1700000120000LL, 100, 101, 2, 2, 1, 0},
};
constexpr Pinned kRelTrailZero = {kRelTrailZero_rows, 1, 10002, 0, 0, 0, 0x52337b68d4ef03e3ULL};

constexpr Row kRelShort_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 101.5, 2, -3, 0, 0},
};
constexpr Pinned kRelShort = {kRelShort_rows, 1, 9997, 3, 0, 0, 0x8c12b97ed08064d4ULL};

constexpr Row kRelThreeWay_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 103, 2, 6, 1, 0},
    {1700000660000LL, 1700000720000LL, 97.5, 100.5, 2, 6, 1, 0},
};
constexpr Pinned kRelThreeWay = {kRelThreeWay_rows, 2, 10012, 0, 0, 0, 0x210c6f877fde3c48ULL};

constexpr Row kRelLimitParent_rows[] = {
    {1700000060000LL, 1700000180000LL, 99.75, 101.75, 2, 4, 1, 0},
    {1700000480000LL, 1700000480000LL, 99.75, 98.75, 2, -2, 1, 0},
};
constexpr Pinned kRelLimitParent = {kRelLimitParent_rows, 2, 10002, 2, 0, 0, 0x9703ae6d927006dfULL};

constexpr Row kRelParentCancel_rows[] = {
    {1700000240000LL, 1700000240000LL, 101.75, 102.75, 2, 2, 1, 0},
};
constexpr Pinned kRelParentCancel = {kRelParentCancel_rows, 1, 10002, 0, 0, 0, 0xe109b35e14c45190ULL};

constexpr Row kRelPyramid_rows[] = {
    {1700000060000LL, 1700000300000LL, 100, 104, 2, 8, 1, 0},
    {1700000180000LL, 1700000300000LL, 100.75, 104, 2, 6.5, 1, 0},
};
constexpr Pinned kRelPyramid = {kRelPyramid_rows, 2, 10014.5, 0, 0, 0, 0x7a228a3c3666f510ULL};

constexpr Row kRelDeclined_rows[] = {
    {1700000060000LL, 1700000300000LL, 100, 111, 100, 1100, 1, 0},
};
constexpr Pinned kRelDeclined = {kRelDeclined_rows, 1, 11100, 0, 0, 0, 0x088ba12fb3c43463ULL};

constexpr Row kRelOcaName_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kRelOcaName = {kRelOcaName_rows, 1, 10005, 0, 0, 0, 0x6b6305edcca0d5b5ULL};

constexpr Row kRelSlippage_rows[] = {
    {1700000060000LL, 1700000060000LL, 100.02, 99.980000000000004, 2, -0.079999999999984084, 1, 0},
    {1700000600000LL, 1700000600000LL, 97.519999999999996, 97.480000000000004, 2, -0.079999999999984084, 1, 0},
};
constexpr Pinned kRelSlippage = {kRelSlippage_rows, 2, 9999.8400000000001, 0.15999999999985448, 0, 0, 0x0514b30438e22e6cULL};

constexpr Row kRelPartial_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 101.5, 1, 1.5, 1, 0},
    {1700000060000LL, 1700000300000LL, 100, 103.5, 1, 3.5, 1, 0},
};
constexpr Pinned kRelPartial = {kRelPartial_rows, 2, 10005, 0, 0, 0, 0x303863dbc705f224ULL};

constexpr Row kRelReversal_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 103.75, 2, 7.5, 1, 0},
    {1700000360000LL, 1700000480000LL, 103.75, 99.75, 2, 8, 0, 0},
};
constexpr Pinned kRelReversal = {kRelReversal_rows, 2, 10015.5, 0, 0, 0, 0x64ddf650a058a5b4ULL};

constexpr Row kRelPyramidOtherId_rows[] = {
    {1700000060000LL, 1700000300000LL, 100, 104, 2, 8, 1, 0},
    {1700000180000LL, 1700000840000LL, 100.75, 103, 2, 4.5, 1, 1},
};
constexpr Pinned kRelPyramidOtherId = {kRelPyramidOtherId_rows, 2, 10012.5, 12.5, 11, 2, 0x52a8ad6e286b0628ULL};

constexpr Row kRelOrderAdd_rows[] = {
    {1700000060000LL, 1700000300000LL, 100, 104, 2, 8, 1, 0},
    {1700000180000LL, 1700000840000LL, 100.75, 103, 1, 2.25, 1, 1},
};
constexpr Pinned kRelOrderAdd = {kRelOrderAdd_rows, 2, 10010.25, 6.25, 5.5, 1, 0x47a66cd880d50594ULL};

constexpr Row kRelPartialClose_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 100.75, 1, 0.75, 1, 0},
    {1700000060000LL, 1700000300000LL, 100, 104, 1, 4, 1, 0},
};
constexpr Pinned kRelPartialClose = {kRelPartialClose_rows, 2, 10004.75, 0, 0, 0, 0x14c100fceb1d86b8ULL};

constexpr Row kRelDeclinedFlat_rows[] = {
    {1700000300000LL, 1700000300000LL, 111, 104, 90.090090090090087, -630.63063063063055, 1, 0},
};
constexpr Pinned kRelDeclinedFlat = {kRelDeclinedFlat_rows, 1, 9369.3693693693695, 630.63063063063055, 0, 0, 0x361936b7d7c3e483ULL};

constexpr Row kRelCancelAll_rows[] = {
    {1700000240000LL, 1700000240000LL, 101.75, 102.75, 2, 2, 1, 0},
};
constexpr Pinned kRelCancelAll = {kRelCancelAll_rows, 1, 10002, 0, 0, 0, 0xe109b35e14c45190ULL};

constexpr Row kRelCancelExitId_rows[] = {
    {1700000060000LL, 1700000420000LL, 99.75, 103.25, 2, 7, 1, 0},
};
constexpr Pinned kRelCancelExitId = {kRelCancelExitId_rows, 1, 10007, 1, 0, 0, 0x810ffa74ed9bfcecULL};

constexpr Row kRelStopParent_rows[] = {
    {1700000120000LL, 1700000240000LL, 100.75, 102.25, 2, 3, 1, 0},
};
constexpr Pinned kRelStopParent = {kRelStopParent_rows, 1, 10003, 0, 0, 0, 0xd1a3df50aac3145fULL};

constexpr Row kRelNegativeShort_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 101.5, 2, -3, 0, 0},
};
constexpr Pinned kRelNegativeShort = {kRelNegativeShort_rows, 1, 9997, 3, 0, 0, 0xde6a5338ca677394ULL};

constexpr Row kRelTwoExits_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 101.5, 2, 3, 1, 0},
};
constexpr Pinned kRelTwoExits = {kRelTwoExits_rows, 1, 10003, 0, 0, 0, 0x413e42490dd2a9d4ULL};

constexpr Row kRelGapFill_rows[] = {
    {1700000420000LL, 1700000420000LL, 103.25, 103.45, 2, -0.40000000000000568, 0, 0},
};
constexpr Pinned kRelGapFill = {kRelGapFill_rows, 1, 9999.6000000000004, 0.3999999999996362, 0, 0, 0x0514b30438e22e6cULL};

constexpr Row kRelShortTrail_rows[] = {
    {1700000420000LL, 1700000540000LL, 103.25, 99, 2, 8.5, 0, 0},
};
constexpr Pinned kRelShortTrail = {kRelShortTrail_rows, 1, 10008.5, 1.5, 0, 0, 0x3784a5265850bf4cULL};

constexpr Row kRelProfitOnly_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kRelProfitOnly = {kRelProfitOnly_rows, 1, 10005, 0, 0, 0, 0x854e44f39ba34322ULL};

constexpr Row kRelQtyExplicit_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 1, 2.5, 1, 0},
    {1700000060000LL, 1700000840000LL, 100, 103, 1, 3, 1, 1},
};
constexpr Pinned kRelQtyExplicit = {kRelQtyExplicit_rows, 2, 10005.5, 6.25, 5.5, 1, 0xdf9a7ea6263e0d46ULL};

constexpr Row kRelReissueChanged_rows[] = {
    {1700000540000LL, 1700000660000LL, 97.25, 99.25, 2, 4, 1, 0},
};
constexpr Pinned kRelReissueChanged = {kRelReissueChanged_rows, 1, 10004, 0, 0, 0, 0x5bd020efbd5159f8ULL};

constexpr Row kRelSharedOca_rows[] = {
    {1700000060000LL, 1700000180000LL, 100, 101.5, 2, 3, 1, 0},
    {1700000540000LL, 1700000660000LL, 97.25, 98.25, 2, -2, 0, 0},
};
constexpr Pinned kRelSharedOca = {kRelSharedOca_rows, 2, 10001, 2, 0, 0, 0x6bb1d517dd594ebeULL};

constexpr Row kRelBreakoutPair_rows[] = {
    {1700000180000LL, 1700000240000LL, 101.25, 102.75, 2, 3, 1, 0},
    {1700000300000LL, 1700000360000LL, 102.75, 104.25, 2, 3, 1, 0},
    {1700000420000LL, 1700000420000LL, 103.25, 102.25, 2, -2, 1, 0},
    {1700000480000LL, 1700000480000LL, 101.25, 100.25, 2, -2, 1, 0},
    {1700000540000LL, 1700000660000LL, 98.25, 99.25, 2, -2, 0, 0},
    {1700000720000LL, 1700000720000LL, 99.25, 100.25, 2, -2, 0, 0},
    {1700000780000LL, 1700000780000LL, 101.25, 102.75, 2, 3, 1, 0},
    {1700000840000LL, 1700000840000LL, 103, 103, 2, 0, 1, 1},
};
constexpr Pinned kRelBreakoutPair = {kRelBreakoutPair_rows, 8, 10001, 8, 3, 2, 0xdee6f9810220c7a0ULL};

constexpr Row kRelBreakoutPairSetOnce_rows[] = {
    {1700000180000LL, 1700000240000LL, 101.25, 102.75, 2, 3, 1, 0},
    {1700000480000LL, 1700000540000LL, 99.25, 97.75, 2, 3, 0, 0},
};
constexpr Pinned kRelBreakoutPairSetOnce = {kRelBreakoutPairSetOnce_rows, 2, 10006, 0, 0, 0, 0x157cc0001ede7376ULL};

constexpr Row kMagRelBreakoutPair_rows[] = {
    {420000LL, 540000LL, 101.25, 102.75, 2, 3, 1, 0},
    {600000LL, 720000LL, 102.75, 104.25, 2, 3, 1, 0},
    {840000LL, 900000LL, 103.25, 102.25, 2, -2, 1, 0},
    {960000LL, 1020000LL, 101.25, 100.25, 2, -2, 1, 0},
    {1080000LL, 1380000LL, 98.25, 99.25, 2, -2, 0, 0},
    {1440000LL, 1500000LL, 99.25, 100.25, 2, -2, 0, 0},
    {1620000LL, 1620000LL, 101.25, 102.75, 2, 3, 1, 0},
    {1680000LL, 1680000LL, 103, 103, 2, 0, 1, 1},
};
constexpr Pinned kMagRelBreakoutPair = {kMagRelBreakoutPair_rows, 8, 10001, 8, 3, 2, 0x03474c49dca8bc04ULL};

constexpr Row kMagRelBracketTp_rows[] = {
    {120000LL, 540000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kMagRelBracketTp = {kMagRelBracketTp_rows, 1, 10005, 0, 0, 0, 0x577159c6dba2ac05ULL};

constexpr Row kMagRelBracketSl_rows[] = {
    {120000LL, 1020000LL, 100, 98.5, 2, -3, 1, 0},
};
constexpr Pinned kMagRelBracketSl = {kMagRelBracketSl_rows, 1, 9997, 10.5, 0, 0, 0xa65938a0103f1ccdULL};

constexpr Row kMagRelTrailOffset_rows[] = {
    {120000LL, 780000LL, 100, 103.5, 2, 7, 1, 0},
};
constexpr Pinned kMagRelTrailOffset = {kMagRelTrailOffset_rows, 1, 10007, 0.5, 0, 0, 0x61c23d6a28f0f142ULL};

constexpr Row kMagRelTrailOneShot_rows[] = {
    {120000LL, 540000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kMagRelTrailOneShot = {kMagRelTrailOneShot_rows, 1, 10005, 0, 0, 0, 0x83150528a91d732fULL};

constexpr Row kMagRelLimitParent_rows[] = {
    {120000LL, 420000LL, 99.75, 101.75, 2, 4, 1, 0},
    {1020000LL, 1020000LL, 99.75, 98.75, 2, -2, 1, 0},
};
constexpr Pinned kMagRelLimitParent = {kMagRelLimitParent_rows, 2, 10002, 2, 0, 0, 0x24e46fd2675cf69fULL};

constexpr Row kMagRelStopParent_rows[] = {
    {300000LL, 540000LL, 100.75, 102.25, 2, 3, 1, 0},
};
constexpr Pinned kMagRelStopParent = {kMagRelStopParent_rows, 1, 10003, 0, 0, 0, 0x9288d52f3e3a8aabULL};

constexpr Row kMagRelShort_rows[] = {
    {120000LL, 420000LL, 100, 101.5, 2, -3, 0, 0},
};
constexpr Pinned kMagRelShort = {kMagRelShort_rows, 1, 9997, 3, 0, 0, 0x5ee3f00d56388cacULL};

constexpr Row kMagRelSlippage_rows[] = {
    {120000LL, 120000LL, 100.02, 99.990000000000009, 2, -0.059999999999973852, 1, 0},
    {1200000LL, 1200000LL, 97.519999999999996, 97.490000000000009, 2, -0.059999999999973852, 1, 0},
};
constexpr Pinned kMagRelSlippage = {kMagRelSlippage_rows, 2, 9999.8799999999992, 0.12000000000080036, 0, 0, 0x0514b30438e22e6cULL};

constexpr Row kCoofRelBracketTp_rows[] = {
    {1700000060000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kCoofRelBracketTp = {kCoofRelBracketTp_rows, 1, 10005, 0, 0, 0, 0x9b4c903337c72ec5ULL};

constexpr Row kCoofRelTrailOffset_rows[] = {
    {1700000060000LL, 1700000360000LL, 100, 103, 2, 6, 1, 0},
};
constexpr Pinned kCoofRelTrailOffset = {kCoofRelTrailOffset_rows, 1, 10006, 1.5, 0, 0, 0xdd3536b38dd5827fULL};

constexpr Row kPoocRelBracketTp_rows[] = {
    {1700000000000LL, 1700000240000LL, 100, 102.5, 2, 5, 1, 0},
};
constexpr Pinned kPoocRelBracketTp = {kPoocRelBracketTp_rows, 1, 10005, 0, 0, 0, 0xdde82a3e625617dfULL};

constexpr Row kPoocRelShort_rows[] = {
    {1700000000000LL, 1700000180000LL, 100, 101.5, 2, -3, 0, 0},
};
constexpr Pinned kPoocRelShort = {kPoocRelShort_rows, 1, 9997, 3, 0, 0, 0x47a1401276fc97fcULL};
constexpr Row kRelPyramidSetOnce_rows[] = {
    {1700000060000LL, 1700000300000LL, 100, 104, 2, 8, 1, 0},
    {1700000180000LL, 1700000300000LL, 100.75, 104, 2, 6.5, 1, 0},
};
constexpr Pinned kRelPyramidSetOnce = {kRelPyramidSetOnce_rows, 2, 10014.5, 0, 0, 0, 0x3ff244519fd858e8ULL};

constexpr Row kMagRelPyramidSetOnce_rows[] = {
    {120000LL, 660000LL, 100, 104, 2, 8, 1, 0},
    {360000LL, 660000LL, 100.75, 104, 2, 6.5, 1, 0},
};
constexpr Pinned kMagRelPyramidSetOnce = {kMagRelPyramidSetOnce_rows, 2, 10014.5, 0, 0, 0, 0xa608f3773ef1c340ULL};
// R4D_PINNED_DATA_END

constexpr const Pinned* kPinned[] = {
    &kRelBracketTp,
    &kRelBracketSl,
    &kRelBracketEveryBar,
    &kRelExitBeforeEntry,
    &kRelTrailOneShot,
    &kRelTrailOffset,
    &kRelTrailZero,
    &kRelShort,
    &kRelThreeWay,
    &kRelLimitParent,
    &kRelParentCancel,
    &kRelPyramid,
    &kRelDeclined,
    &kRelOcaName,
    &kRelSlippage,
    &kRelPartial,
    &kRelReversal,
    &kRelPyramidOtherId,
    &kRelOrderAdd,
    &kRelPartialClose,
    &kRelDeclinedFlat,
    &kRelCancelAll,
    &kRelCancelExitId,
    &kRelStopParent,
    &kRelNegativeShort,
    &kRelTwoExits,
    &kRelGapFill,
    &kRelShortTrail,
    &kRelProfitOnly,
    &kRelQtyExplicit,
    &kRelReissueChanged,
    &kRelSharedOca,
    &kRelBreakoutPair,
    &kRelBreakoutPairSetOnce,
    &kMagRelBreakoutPair,
    &kMagRelBracketTp,
    &kMagRelBracketSl,
    &kMagRelTrailOffset,
    &kMagRelTrailOneShot,
    &kMagRelLimitParent,
    &kMagRelStopParent,
    &kMagRelShort,
    &kMagRelSlippage,
    &kCoofRelBracketTp,
    &kCoofRelTrailOffset,
    &kPoocRelBracketTp,
    &kPoocRelShort,
    &kRelPyramidSetOnce,
    &kMagRelPyramidSetOnce,
};
static_assert(std::size(kPinned) == std::size(kShapes));

void compare_all() {
    for (std::size_t index = 0; index < std::size(kShapes); ++index) {
        const Named& named = kShapes[index];
        const Pinned& pinned = *kPinned[index];
        scenario = named.name;
        const Observed got = observe(named.shape, named.mode);
        CHECK(got.rows.size() >= named.min_rows);
        CHECK(got.rows.size() == pinned.rows_len);
        const std::size_t n = got.rows.size() < pinned.rows_len ? got.rows.size()
                                                                : pinned.rows_len;
        for (std::size_t i = 0; i < n; ++i) {
            const Row& left = got.rows[i];
            const Row& right = pinned.rows[i];
            CHECK(left.entry_time == right.entry_time);
            CHECK(left.exit_time == right.exit_time);
            CHECK(left.entry_price == right.entry_price);
            CHECK(left.exit_price == right.exit_price);
            CHECK(left.qty == right.qty);
            CHECK(left.pnl == right.pnl);
            CHECK(left.is_long == right.is_long);
            CHECK(left.open_at_end == right.open_at_end);
        }
        CHECK(got.final_equity == pinned.final_equity);
        CHECK(got.max_drawdown == pinned.max_drawdown);
        CHECK(got.max_runup == pinned.max_runup);
        CHECK(got.position_units == pinned.position_units);
        std::printf("  %-24s anchored=%llu adopted=%llu withdrawn=%llu\n", named.name,
                    static_cast<unsigned long long>(got.anchored),
                    static_cast<unsigned long long>(got.adopted),
                    static_cast<unsigned long long>(got.withdrawn));
        CHECK(got.anchored == named.legs.anchored);
        CHECK(got.adopted == named.legs.adopted);
        CHECK(got.withdrawn == named.legs.withdrawn);
        const bool same_book = got.book == pinned.book;
        CHECK(same_book);
        if (!same_book) {
            // The digest names no bar; the trace does.
            for (const auto& line : got.lines) std::printf("    %s\n", line.c_str());
        }
    }
}

#endif  // PINEFORGE_R4D_HARVEST

}  // namespace r4d

}  // namespace

int main() {
#if defined(PINEFORGE_R4D_HARVEST)
    r4d::harvest();
    return failures == 0 ? 0 : 1;
#elif defined(PINEFORGE_R4_HARVEST)
    std::printf("// harvested: paste between R4_PINNED_DATA_BEGIN/END\n");
    emit("FromEntryBracket", Shape::FromEntryBracket);
    emit("FromEntryStopout", Shape::FromEntryStopout);
    emit("TrailReissue", Shape::TrailReissue);
    emit("TrailZeroOffset", Shape::TrailZeroOffset);
    emit("ParentRejection", Shape::ParentRejection);
    emit("CycleRevival", Shape::CycleRevival);
    return 0;
#else
    for (const Expected& pinned : kScenarios) compare(pinned);
    adapter_projects_the_tick_the_kernel_resolves_against();
    r4d::compare_all();
    std::printf("adapter bracket/trail re-lowering: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
