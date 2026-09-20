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
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
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

}  // namespace

int main() {
#ifdef PINEFORGE_R4_HARVEST
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
    std::printf("adapter bracket/trail re-lowering: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
