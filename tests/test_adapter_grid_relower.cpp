// R5 lane L8 / R7: the Pine adapter's trigger-threshold projection measured
// against the kernel price grid (NativePriceGrid::QuantizeFillsAndTriggers).
//
// Outcome of the lane: the adapter is NOT re-lowered onto the grid and keeps
// NativePriceGrid::None with its own source_trigger_threshold. The re-lowering
// was attempted and measured (adapter submits the raw level, project() sets
// QuantizeFillsAndTriggers / HalfUp): 6 of 515 ctest units move, this witness
// among them, and two of the mechanisms have no adapter-side or generic remedy:
//   B1 the grid's trigger mode is not usable on a sub-tick feed yet: the
//      matcher reports a cursor print that is inside the QUANTIZED region but
//      short of the RAW level, and the core re-validates every activation with
//      the raw compare (native_order.cpp stop_price_reached), so the run fails
//      closed with "native working-request preparation failed" — 9 pinned
//      NYSE:F / AAPL zero-offset-trail tapes and the POOC short-close panels
//      abort. Section 6 pins the generic reproducer; the remedy is a kernel
//      ruling on what an activation's reached price is under a grid.
//   B2 TradingView's tick quantization is a property of the ORDER KIND — stop
//      and limit legs and a trail's activation are tested on the quantized
//      path; a trail's stop (best - offset), the running best, stop-limit
//      entries and the calc_on_order_fills cursors on the raw path
//      (engine.hpp:1035-1063, ab9714be engine_path_resolve.cpp:392-424,
//      745-766, 939-947) — whereas NativeRunSpec::price_grid is a property of
//      the RUN and the matcher hands one GridThreshold to every trigger kind,
//      the trail stop included. Even on an on-grid feed the two differ:
//      best - offset lands one ULP under its ladder point on 14% of (best,
//      offset) pairs, the raw compare holds through an exact touch and the
//      grid's nanotick guard fires it (section 4 TrailUlp, section 5). A
//      per-kind grid mask would spell that inconsistency into the kernel,
//      which is not generic, so the lane stops there.
// Two further effects are adapter-side and were left alone with the rest:
// the grid pre-rounds facts.default_resolved_price, which TradingView's
// directional snap of a sub-tick open needs raw (AAPL 196.135 -> 196.13), and
// a raw level makes a re-issued leg bit-equal to its source level, so the
// adapter keeps a request it replaces today (5 of 312 corpus probes change
// their "Engine entry incarnation" column and nothing else).
//
// What the lane DID find and fix in the kernel, generically, are three grid
// exactness defects, pinned here with the cases that failed before:
//   G1 the HalfUp threshold was the product (index +/- 0.5) * tick, which is
//      not the binary64 boundary of grid_round_half_up: a tie print rounds
//      away from zero and lies OUTSIDE a `<=` region, and a decimal half tick
//      lands ULPs to either side of that product — 12 of the 26 verdicts on
//      13 real-world sub-tick prints (finding-446 / round-6 tapes) went
//      against the kernel's own rounding;
//   G2 grid_index_up/down applied the 1e-9 nanotick guard to a level that IS a
//      ladder price, which misindexes from k = 16,782,232 on (a 5-decimal
//      instrument above 167.82, an 8-decimal one above 0.2395);
//   G3 the limit cap re-spelled a ladder level as k * tick, one ULP past a
//      decimal literal, so under QuantizeFills the kernel booked a buy-limit
//      fill that its own limit-or-better check then refused: 41,570 of
//      300,001 two-decimal levels could never fill. The cap of a ladder level
//      is now the level itself; the half-up fill basis keeps its k * tick
//      spelling (SizePrice::Signal pins it) and the cap clamps it back.
//
// Sections:
//   1. kernel: HalfUp boundary == grid_round_half_up, sweeps + pinned prints
//   2. kernel: a ladder level maps to its own index, both spellings
//   3. kernel: a buy limit on a decimal literal fills at its level under the grid
//   4. adapter: TradingView's per-kind rule on the same boundary prints, as a
//      NEUTRALITY differential — every pinned number was harvested from the
//      UNCHANGED adapter on the lane base 86ef9ed by compiling this TU with
//      -DPINEFORGE_R7_HARVEST against that tree's libpineforge.a (Release,
//      the run_corpus.sh build); rebuild them the same way, never by hand.
//   5. the measured divergence: the kernel grid fires the trail stop of
//      section 4's TrailUlp scenario one tick early (B2).
//   6. the grid's trigger mode fails closed on a sub-tick cursor print (B1).
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_matching.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace nm = pineforge::native_matching;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "setup";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s] %s:%d  %s\n", scenario, __FILE__, line, expr);
}
#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kT = 1700000000000LL;
// A native host takes canonical slot labels only: a minute-aligned stamp.
constexpr std::int64_t kNativeT = 1736121600000LL;

bool same_bits(double actual, double expected) {
    if (nm::double_bits(actual) == nm::double_bits(expected)) return true;
    std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    return false;
}

double decimal_literal(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, value);
    return std::strtod(buf, nullptr);
}

// TradingView's census rule, engine.hpp:959-977: floor(p / tick + 0.5), the
// exact double-precision function every sub-tick TV fill was fitted to.
double census_index(double p, double tick) { return std::floor(p / tick + 0.5); }

// The kernel's verdict for "the quantized print p reaches the level": the
// matcher's region entry with include_current on a one-point segment.
bool kernel_reaches(double p, double level, double tick, bool le) {
    const nm::GridThreshold grid{tick, true};
    const nm::GeometricHit start{0.0, p, false};
    return nm::first_region_entry(p, p, start, level, le, /*include_current=*/true, grid)
        .has_value();
}

// --- 1. the HalfUp threshold is the binary64 boundary of the rounding ------
void kernel_half_up_boundary_matches_its_rounding() {
    scenario = "G1 half-up boundary";
    struct Sweep { double tick; double lo, hi; int decimals; };
    // decimals: the tick's decimal places; a binary tick (0.25, 2^-5) has an
    // exact half tick, so both spellings coincide and the tie itself is the
    // whole question.
    const Sweep sweeps[] = {
        {0.01, 5.0, 5000.0, 2},
        {0.25, 1000.0, 7000.0, 2},
        {0.03125, 50.0, 200.0, 5},
        {1e-5, 0.5, 2.0, 5},
        {1e-8, 1e-6, 1e-3, 8},
    };
    for (const auto& s : sweeps) {
        const long k_lo = static_cast<long>(std::floor(s.lo / s.tick));
        const long k_hi = static_cast<long>(std::floor(s.hi / s.tick));
        const long step = std::max(1L, (k_hi - k_lo) / 3000);
        int disagree = 0;
        for (long k = k_lo; k <= k_hi; k += step) {
            for (int spelling = 0; spelling < 2; ++spelling) {
                double p = (static_cast<double>(k) + 0.5) * s.tick;
                if (spelling == 1) p = decimal_literal(p, s.decimals + 1);
                const double lower = decimal_literal(k * s.tick, s.decimals);
                const double upper = decimal_literal((k + 1) * s.tick, s.decimals);
                // A sell stop on the lower point is reached iff the print
                // rounds down; a buy stop on the upper point iff it rounds up.
                // Judged on the ladder INDEX grid_round_half_up rounds to: the
                // two spellings of one ladder point differ by an ULP.
                const double rounded_index = std::round(p / s.tick);
                const bool rounds_down = rounded_index <= static_cast<double>(k);
                const bool rounds_up = rounded_index >= static_cast<double>(k + 1);
                if (kernel_reaches(p, lower, s.tick, true) != rounds_down) ++disagree;
                if (kernel_reaches(p, upper, s.tick, false) != rounds_up) ++disagree;
                // and the kernel's rounding is TradingView's census rule
                CHECK(std::round(p / s.tick) == census_index(p, s.tick));
            }
        }
        CHECK(disagree == 0);
        if (disagree) std::printf("  tick %g: %d verdicts disagree with grid_round_half_up\n", s.tick, disagree);
    }

    // The pinned real-world sub-tick prints (engine.hpp:963-972 finding-446,
    // :1040-1063 round-6, ab9714be engine_path_resolve.cpp:400-405 round-7),
    // with the direction TradingView was observed to round them.
    struct Pinned { double print; double rounds_to; };
    const Pinned pinned[] = {
        {228.765, 228.76}, {214.385, 214.39}, {11.805, 11.81}, {13.775, 13.78},
        {9.415, 9.41},     {12.075, 12.08},   {13.745, 13.75}, {14.035, 14.04},
        {10.175, 10.18},   {11.195, 11.20},   {196.135, 196.14}, {13.915, 13.91},
        {12.255, 12.26},
    };
    for (const auto& row : pinned) {
        CHECK(same_bits(census_index(row.print, 0.01) * 0.01,
                        std::round(row.rounds_to / 0.01) * 0.01));
        const double lower = decimal_literal(std::floor(row.print / 0.01) * 0.01, 2);
        const double upper = decimal_literal(lower + 0.01, 2);
        const bool down = row.rounds_to < row.print;
        CHECK(kernel_reaches(row.print, lower, 0.01, true) == down);
        CHECK(kernel_reaches(row.print, upper, 0.01, false) == !down);
    }

    // Threshold form, stated directly on the binary tick of the L8 witness: the
    // ge side keeps its exact half tick (401.5 rounds to 402, inside), the le
    // side ends one ULP below it (the tie rounds away, outside).
    const nm::GridThreshold half{0.25, true};
    CHECK(same_bits(nm::grid_region_threshold(100.50, false, half), 100.375));
    CHECK(same_bits(nm::grid_region_threshold(100.50, true, half),
                    std::nextafter(100.625, -kInf)));
    // An inactive grid is the raw level, bit for bit.
    CHECK(same_bits(nm::grid_region_threshold(214.38, true, {}), 214.38));
}

// --- 2. a ladder price maps to its own index -------------------------------
void kernel_index_is_exact_on_the_ladder() {
    scenario = "G2 exact ladder index";
    struct Ladder { double tick; int decimals; long first_failure_before; };
    // first_failure_before: the first k the nanotick guard misindexed on the
    // lane base, from the lane's measurement harness (both spellings, k to 2^26).
    const Ladder ladders[] = {
        {0.01, 2, 22653211L}, {0.25, 2, -1L}, {0.03125, 5, -1L},
        {1e-5, 5, 16782232L}, {1e-8, 8, 23955506L},
    };
    for (const auto& l : ladders) {
        const double inverse = std::floor(1.0 / l.tick + 0.5);
        int bad = 0;
        for (long k = 1; k <= (1L << 26); k += (k < 4096 ? 1 : k / 4096)) {
            const double product = static_cast<double>(k) * l.tick;
            const double quotient = static_cast<double>(k) / inverse;
            for (double level : {product, quotient}) {
                if (nm::grid_index_up(level, l.tick) != static_cast<double>(k)) ++bad;
                if (nm::grid_index_down(level, l.tick) != static_cast<double>(k)) ++bad;
                // and the directional rounding is a fixed point on the ladder
                if (nm::double_bits(nm::grid_round_directional(level, l.tick, true))
                    != nm::double_bits(level)) ++bad;
                if (nm::double_bits(nm::grid_round_directional(level, l.tick, false))
                    != nm::double_bits(level)) ++bad;
            }
        }
        CHECK(bad == 0);
        if (l.first_failure_before > 0) {
            const double level = static_cast<double>(l.first_failure_before) / inverse;
            CHECK(nm::grid_index_up(level, l.tick) == static_cast<double>(l.first_failure_before));
            CHECK(nm::grid_index_down(level, l.tick) == static_cast<double>(l.first_failure_before));
        }
    }
    // Off the ladder the nanotick guard still decides, unchanged.
    CHECK(nm::grid_index_up(100.003, 0.01) == 10001.0);
    CHECK(nm::grid_index_down(100.003, 0.01) == 10000.0);
    CHECK(nm::grid_index_up(100.00 + 1e-11, 0.01) == 10000.0);
    CHECK(same_bits(nm::grid_round_directional(2409.493, 0.01, false), 240949 * 0.01));
}

// --- 3. a decimal-literal limit fills at its level under the grid ---------
struct Host final : NativeStrategyHost {
    std::function<void(Host&)> begin;
    void on_native_run_begin() override { if (begin) begin(*this); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

NativeRunSpec grid_spec(const char* key, NativePriceGrid grid,
                        NativeGridRounding rounding = NativeGridRounding::HalfUp) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1"; s.script_tf = "1";
    s.tickerid = "TEST:GRID"; s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 100000; s.point_value = 1; s.account_fx = 1;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution; s.fee_value = 0;
    s.price_grid = grid;
    s.grid_rounding = rounding;
    return s;
}

struct Fill { double raw; double resolved; };
struct Run { std::vector<Fill> fills; std::string error; };

Run execute(const NativeRunSpec& s, const no::Request& request, const Bar& bar) {
    Host h;
    h.begin = [&](Host& x) { CHECK(x.submit(request).status == no::SubmitStatus::Accepted); };
    Run out;
    if (h.configure_native(s).status != NativeSetupStatus::Applied) {
        CHECK(false);
        return out;
    }
    h.run(&bar, 1);
    for (const auto& row : h.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command))
            out.fills.push_back({e->raw_price, e->resolved_price});
    }
    out.error = h.last_error();
    if (!out.error.empty()) std::printf("  run error: %s\n", out.error.c_str());
    return out;
}

void kernel_books_the_ladder_level_it_reached() {
    scenario = "G3 ladder level fills";
    // 2409.49 is a two-decimal literal whose k * 0.01 spelling sits one ULP
    // above it; every price on this bar is on the ladder.
    const double level = 2409.49;
    CHECK(240949 * 0.01 > level);
    const Bar bar{2409.60, 2409.70, 2409.40, 2409.50, 1.0, kNativeT};
    no::Request buy{no::Transact{1.0}, "buy", ""};
    buy.trigger = no::Limit{level, false};
    no::Request sell{no::Transact{-1.0}, "sell", ""};
    sell.trigger = no::Limit{2409.61, false};
    for (auto grid : {NativePriceGrid::QuantizeFills, NativePriceGrid::QuantizeFillsAndTriggers}) {
        for (auto rounding : {NativeGridRounding::HalfUp, NativeGridRounding::Directional}) {
            const auto got = execute(grid_spec("ladder-buy", grid, rounding), buy, bar);
            CHECK(got.error.empty());
            CHECK(got.fills.size() == 1);
            if (got.fills.size() == 1) {
                CHECK(same_bits(got.fills[0].raw, level));
                CHECK(same_bits(got.fills[0].resolved, level));
            }
            // A sell limit keeps limit-or-better the other way: it books its
            // own ladder point, never under the level.
            const auto sold = execute(grid_spec("ladder-sell", grid, rounding), sell, bar);
            CHECK(sold.error.empty());
            CHECK(sold.fills.size() == 1);
            if (sold.fills.size() == 1) {
                CHECK(sold.fills[0].resolved >= 2409.61);
                CHECK(std::round(sold.fills[0].resolved / 0.01) == 240961.0);
            }
        }
    }
    // The grid still rounds an off-ladder level onto the tick: a buy limit at
    // 2409.493 is reached (the quantized low 2409.40 is under it) and books
    // the tick on its own side, and the directional fill basis agrees.
    no::Request off{no::Transact{1.0}, "off", ""};
    off.trigger = no::Limit{2409.493, false};
    const auto rounded = execute(grid_spec("ladder-off", NativePriceGrid::QuantizeFills), off, bar);
    CHECK(rounded.error.empty());
    CHECK(rounded.fills.size() == 1);
    if (rounded.fills.size() == 1) CHECK(same_bits(rounded.fills[0].resolved, 240949 * 0.01));
    // None is untouched: the raw level is booked as presented.
    const auto raw = execute(grid_spec("ladder-none", NativePriceGrid::None), off, bar);
    CHECK(raw.error.empty());
    CHECK(raw.fills.size() == 1);
    if (raw.fills.size() == 1) CHECK(same_bits(raw.fills[0].resolved, 2409.493));
}

// --- 4. the adapter's TradingView rule, pinned as data ---------------------
Bar mk(int index, double o, double h, double l, double c) {
    Bar bar;
    bar.open = o; bar.high = h; bar.low = l; bar.close = c;
    bar.volume = 1.0;
    bar.timestamp = kT + static_cast<std::int64_t>(index) * 60000;
    return bar;
}

enum class Shape {
    SubTickSellStop,   // long, sell stop 13.74: low 13.745 -> 13.75 skips, 13.70 fills
    SubTickBuyStop,    // short, buy stop 14.04: high 14.0351 -> 14.04 fills
    SubTickSellLimit,  // long, sell limit 10.18: high 10.175 -> 10.18 fills
    TrailActivation,   // short, trail_points 8 offset 0: low 9.415 -> 9.41 arms and exits
    TrailUlp,          // long, offset trail: stop 1 ULP under 1499.92, the 1499.92 low holds
};

class GridProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit GridProbe(Shape shape) : shape_(shape) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = 1;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        margin_call_enabled_ = false;
    }

    // The signed physical position left at the end of the run.
    double signed_units() const noexcept {
        return position_qty_ * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int bar = bar_index_;
        switch (shape_) {
        case Shape::SubTickSellStop:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) strategy_exit("X", "L", kNaN, 13.74, kNaN, kNaN, kNaN, 100.0, "stop");
            break;
        case Shape::SubTickBuyStop:
            if (bar == 0) strategy_entry("S", false);
            if (bar >= 1) strategy_exit("X", "S", kNaN, 14.04, kNaN, kNaN, kNaN, 100.0, "stop");
            break;
        case Shape::SubTickSellLimit:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) strategy_exit("X", "L", 10.18, kNaN, kNaN, kNaN, kNaN, 100.0, "limit");
            break;
        case Shape::TrailActivation:
            if (bar == 0) strategy_entry("S", false);
            if (bar >= 1) strategy_exit("T", "S", kNaN, kNaN, /*trail_points=*/8.0,
                                        /*trail_offset=*/0.0, kNaN, 100.0, "trail");
            break;
        case Shape::TrailUlp:
            if (bar == 0) strategy_entry("L", true);
            if (bar >= 1) strategy_exit("T", "L", kNaN, kNaN, /*trail_points=*/5.0,
                                        /*trail_offset=*/15.0, kNaN, 100.0, "trail");
            break;
        }
    }

private:
    Shape shape_;
};

std::vector<Bar> feed(Shape shape) {
    std::vector<Bar> bars;
    auto push = [&](double o, double h, double l, double c) {
        bars.push_back(mk(static_cast<int>(bars.size()), o, h, l, c));
    };
    switch (shape) {
    case Shape::SubTickSellStop:
        // engine.hpp:1043-1046: sell-stops at 13.74x SKIP the bar whose low
        // is 13.745 (-> 13.75) and fill the next bar @13.74.
        push(13.80, 13.80, 13.80, 13.80);   // 0: entry placed
        push(13.80, 13.85, 13.78, 13.82);   // 1: L fills @13.80, stop 13.74 placed
        push(13.82, 13.84, 13.745, 13.80);  // 2: low 13.745 -> 13.75: no fill
        push(13.80, 13.81, 13.70, 13.72);   // 3: fills @13.74
        push(13.72, 13.73, 13.71, 13.72);   // 4
        break;
    case Shape::SubTickBuyStop:
        // engine.hpp:1047-1049: buy-stops at 14.03x/14.04 fill on the bar
        // whose high is 14.0351 (-> 14.04) @14.04; a raw compare would not.
        push(14.00, 14.00, 14.00, 14.00);   // 0: entry placed
        push(14.00, 14.02, 13.98, 14.00);   // 1: S fills @14.00, stop 14.04 placed
        push(14.00, 14.0351, 13.99, 14.01); // 2: high 14.0351 -> 14.04: fills @14.04
        push(14.01, 14.02, 14.00, 14.01);   // 3
        break;
    case Shape::SubTickSellLimit:
        // pine_adapter.cpp:7830-7834: NYSE:F high 10.175 -> 10.18 fills a
        // 10.18 sell limit.
        push(10.10, 10.10, 10.10, 10.10);   // 0: entry placed
        push(10.10, 10.12, 10.08, 10.10);   // 1: L fills @10.10, limit 10.18 placed
        push(10.10, 10.175, 10.09, 10.15);  // 2: high 10.175 -> 10.18: fills @10.18
        push(10.15, 10.16, 10.14, 10.15);   // 3
        break;
    case Shape::TrailActivation:
        // ab9714be engine_path_resolve.cpp:394-406 (round 7 family K): a short
        // @9.49 with trail_points 8 (activation 9.41) and offset 0 exits
        // one-shot @9.41 on the bar whose raw low is 9.415 (-> 9.41).
        push(9.49, 9.49, 9.49, 9.49);       // 0: entry placed
        push(9.49, 9.50, 9.48, 9.49);       // 1: S fills @9.49, trail placed
        push(9.49, 9.50, 9.415, 9.45);      // 2: low 9.415 -> 9.41 reaches 9.41: exit @9.41
        push(9.45, 9.46, 9.44, 9.45);       // 3
        break;
    case Shape::TrailUlp:
        // On-grid feed (the corpus shape). Activation 1500.05 (5 ticks),
        // offset 15 ticks: the best 1500.07 puts the stop at
        // 1500.07 - 15 * 0.01 = 1499.9199999999998, one ULP UNDER the ladder
        // point 1499.92, so a low printing exactly 1499.92 does not reach it
        // on the raw path (BASE, TradingView) and the exit waits for bar 4.
        push(1500.00, 1500.00, 1500.00, 1500.00);   // 0: entry placed
        push(1500.00, 1500.03, 1499.98, 1500.02);   // 1: L fills @1500.00, trail placed
        push(1500.02, 1500.07, 1500.01, 1500.05);   // 2: arms; best 1500.07
        push(1500.05, 1500.06, 1499.92, 1499.95);   // 3: low 1499.92 == the stop's ladder point
        push(1499.95, 1499.96, 1499.80, 1499.85);   // 4: through the stop
        push(1499.85, 1499.86, 1499.84, 1499.85);   // 5
        break;
    }
    return bars;
}

struct Row {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
};

struct Observed {
    std::vector<Row> rows;
    double position_units = 0.0;
};

Observed observe(Shape shape) {
    Observed out;
    GridProbe probe(shape);
    const auto bars = feed(shape);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                            t.qty, t.pnl, t.is_long});
    }
    BacktestEngine::free_report(&report);
    out.position_units = probe.signed_units();
    return out;
}

#ifdef PINEFORGE_R7_HARVEST

void emit(const char* symbol, Shape shape) {
    const Observed got = observe(shape);
    std::printf("constexpr Row k%s_rows[] = {\n", symbol);
    for (const auto& r : got.rows) {
        std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %.17g, %d},\n",
                    static_cast<long long>(r.entry_time), static_cast<long long>(r.exit_time),
                    r.entry_price, r.exit_price, r.qty, r.pnl, r.is_long);
    }
    if (got.rows.empty()) std::printf("    // (no rows)\n");
    std::printf("};\nconstexpr double k%s_position_units = %.17g;\n\n", symbol, got.position_units);
}

#else

// R7_PINNED_DATA_BEGIN — harvested on the lane base 86ef9ed, unchanged adapter.
constexpr Row kSubTickSellStop_rows[] = {
    {1700000060000LL, 1700000180000LL, 13.800000000000001, 13.74, 1, -0.060000000000000497, 1},
};
constexpr double kSubTickSellStop_position_units = 0;

constexpr Row kSubTickBuyStop_rows[] = {
    {1700000060000LL, 1700000120000LL, 14, 14.040000000000001, 1, -0.040000000000000924, 0},
};
constexpr double kSubTickBuyStop_position_units = 0;

constexpr Row kSubTickSellLimit_rows[] = {
    {1700000060000LL, 1700000120000LL, 10.1, 10.18, 1, 0.080000000000000071, 1},
};
constexpr double kSubTickSellLimit_position_units = 0;

constexpr Row kTrailActivation_rows[] = {
    {1700000060000LL, 1700000120000LL, 9.4900000000000002, 9.4100000000000001, 1, 0.080000000000000071, 0},
};
constexpr double kTrailActivation_position_units = 0;

constexpr Row kTrailUlp_rows[] = {
    {1700000060000LL, 1700000240000LL, 1500, 1499.9200000000001, 1, -0.07999999999992724, 1},
};
constexpr double kTrailUlp_position_units = 0;
// R7_PINNED_DATA_END

struct Expected {
    const char* name;
    Shape shape;
    const Row* rows;
    std::size_t rows_len;
    double position_units;
    // The discriminating fact of each scenario, restated so a harvest that
    // degenerated into "nothing happened" pins nothing.
    std::int64_t exit_time;
    double exit_price;
};

constexpr Expected kScenarios[] = {
    {"sub-tick-sell-stop", Shape::SubTickSellStop, kSubTickSellStop_rows,
     std::size(kSubTickSellStop_rows), kSubTickSellStop_position_units,
     kT + 3 * 60000, 13.74},
    {"sub-tick-buy-stop", Shape::SubTickBuyStop, kSubTickBuyStop_rows,
     std::size(kSubTickBuyStop_rows), kSubTickBuyStop_position_units,
     kT + 2 * 60000, 14.040000000000001},
    {"sub-tick-sell-limit", Shape::SubTickSellLimit, kSubTickSellLimit_rows,
     std::size(kSubTickSellLimit_rows), kSubTickSellLimit_position_units,
     kT + 2 * 60000, 10.18},
    {"trail-activation", Shape::TrailActivation, kTrailActivation_rows,
     std::size(kTrailActivation_rows), kTrailActivation_position_units,
     kT + 2 * 60000, 9.4100000000000001},
    {"trail-ulp", Shape::TrailUlp, kTrailUlp_rows,
     std::size(kTrailUlp_rows), kTrailUlp_position_units,
     kT + 4 * 60000, 1499.9200000000001},
};

void compare(const Expected& pinned) {
    scenario = pinned.name;
    const Observed got = observe(pinned.shape);
    CHECK(got.rows.size() == pinned.rows_len);
    CHECK(got.rows.size() == 1);
    const std::size_t n = std::min(got.rows.size(), pinned.rows_len);
    for (std::size_t i = 0; i < n; ++i) {
        const Row& left = got.rows[i];
        const Row& right = pinned.rows[i];
        CHECK(left.entry_time == right.entry_time);
        CHECK(left.exit_time == right.exit_time);
        CHECK(same_bits(left.entry_price, right.entry_price));
        CHECK(same_bits(left.exit_price, right.exit_price));
        CHECK(same_bits(left.qty, right.qty));
        CHECK(same_bits(left.pnl, right.pnl));
        CHECK(left.is_long == right.is_long);
        // the discriminating fact
        CHECK(left.exit_time == pinned.exit_time);
        CHECK(same_bits(left.exit_price, pinned.exit_price));
    }
    CHECK(same_bits(got.position_units, pinned.position_units));
}

// --- 5. the measured divergence -------------------------------------------
void grid_fires_the_trail_stop_one_tick_early() {
    scenario = "trail-ulp kernel twin";
    // TrailUlp, bar 3, the H -> L leg: best 1500.07, offset TrailTicks{15}
    // resolved as 15 * 0.01 (native_order.cpp:1387), a long's sell trail.
    const double best = 1500.07;
    const double offset = 15.0 * 0.01;
    double stop = 0.0;
    CHECK(nm::checked_trail_stop(best, offset, /*buy=*/false, &stop));
    CHECK(stop < 1499.92);
    CHECK(same_bits(std::nextafter(1499.92, -kInf), stop));
    const nm::GeometricHit start{0.0, 1500.06, false};
    // Raw path (the adapter's None): the 1499.92 low does not reach a stop one
    // ULP under it — the pinned BASE row above exits on bar 4.
    CHECK(!nm::trail_stop_hit(1500.06, 1499.92, start, best, offset, false).has_value());
    // The grid absorbs the ULP by its nanotick guard and fires on bar 3.
    const nm::GridThreshold half{0.01, true};
    const auto early = nm::trail_stop_hit(1500.06, 1499.92, start, best, offset, false, half);
    CHECK(early.has_value());
    if (early) CHECK(same_bits(early->price, stop));
    // The same grid AGREES with the adapter on every resting stop / limit of
    // section 4: those are the kinds TradingView tests on the quantized path.
    CHECK(!kernel_reaches(13.745, 13.74, 0.01, true));     // sub-tick-sell-stop skips
    CHECK(kernel_reaches(14.0351, 14.04, 0.01, false));    // sub-tick-buy-stop fills
    CHECK(kernel_reaches(10.175, 10.18, 0.01, false));     // sub-tick-sell-limit fills
    CHECK(kernel_reaches(9.415, 9.41, 0.01, true));        // trail activation reached
    // and disagrees with the raw compare TradingView keeps for its trail stop:
    // one grid, two TradingView rules — the per-kind mask is not generic.
}

// --- 6. the trigger mode fails closed on a sub-tick cursor print ----------
// KNOWN KERNEL DEFECT, open (B1): this pins that the run is REFUSED rather
// than mis-filled, and is the reproducer for the lane that rules on it. A buy
// stop at 100.50 on a 0.25 ladder; the bar OPENS at 100.40, whose nearest tick
// is 100.50. The grid matcher reports that cursor print as inside the region,
// the core's raw re-validation (100.40 < 100.50) refuses the activation, and
// the run latches. None is unaffected: the raw path never reaches the stop.
void grid_trigger_mode_fails_closed_on_a_subtick_cursor_print() {
    scenario = "B1 sub-tick cursor print";
    no::Request buy{no::Transact{1.0}, "buy-stop", ""};
    buy.trigger = no::Stop{100.50};
    const Bar bar{100.40, 100.45, 100.00, 100.10, 1.0, kNativeT};
    CHECK(same_bits(nm::grid_round_half_up(bar.open, 0.25), 100.50));
    auto quarter = [](const char* key, NativePriceGrid grid) {
        auto s = grid_spec(key, grid);
        s.price_tick = 0.25;
        return s;
    };
    {
        Host h;
        h.begin = [&](Host& x) { CHECK(x.submit(buy).status == no::SubmitStatus::Accepted); };
        CHECK(h.configure_native(quarter("b1-none", NativePriceGrid::None)).status
              == NativeSetupStatus::Applied);
        h.run(&bar, 1);
        CHECK(h.last_error().empty());
    }
    {
        Host h;
        h.begin = [&](Host& x) { CHECK(x.submit(buy).status == no::SubmitStatus::Accepted); };
        CHECK(h.configure_native(quarter("b1-grid", NativePriceGrid::QuantizeFillsAndTriggers)).status
              == NativeSetupStatus::Applied);
        h.run(&bar, 1);
        CHECK(h.last_error() == "native working-request preparation failed");
        int fills = 0;
        for (const auto& row : h.native_events(0)) {
            if (row.command && std::get_if<no::ExecutionAppliedEvent>(&*row.command)) ++fills;
        }
        CHECK(fills == 0);
    }
}

#endif  // PINEFORGE_R7_HARVEST

}  // namespace

int main() {
#ifdef PINEFORGE_R7_HARVEST
    std::printf("// harvested: paste between R7_PINNED_DATA_BEGIN/END\n");
    emit("SubTickSellStop", Shape::SubTickSellStop);
    emit("SubTickBuyStop", Shape::SubTickBuyStop);
    emit("SubTickSellLimit", Shape::SubTickSellLimit);
    emit("TrailActivation", Shape::TrailActivation);
    emit("TrailUlp", Shape::TrailUlp);
    return failures ? 1 : 0;
#else
    kernel_half_up_boundary_matches_its_rounding();
    kernel_index_is_exact_on_the_ladder();
    kernel_books_the_ladder_level_it_reached();
    for (const auto& pinned : kScenarios) compare(pinned);
    grid_fires_the_trail_stop_one_tick_early();
    grid_trigger_mode_fails_closed_on_a_subtick_cursor_print();
    std::printf("%s adapter grid re-lowering witness: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
#endif
}
