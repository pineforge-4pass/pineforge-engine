// R6 — the Pine adapter's calc_on_order_fills re-lowered onto the kernel's
// calculation-timing feature (R5 lane L5).
//
// DIFFERENTIAL TEST. Every `kExpect*` literal below was harvested from the
// PRE-lowering library — engine main b0cec54 ("Port the wave-2 lanes'
// hash-neutrality guards to native_run_spec_digest()"), whose adapter ran its
// own fill-triggered recalculation from inside on_native_applied and left
// NativeRunSpec::calculation at BarClose. The harvest ran this very file
// compiled with -DPF_COOF_HARVEST against that library; the run prints each
// scenario's trace, and those strings are pinned here verbatim. The test then
// re-runs the identical probes against the POST-lowering adapter, whose spec
// selects NativeCalculationTrigger::BarCloseAndFills and whose recalculation
// is delivered by the consumer as on_native_recalculate(..., OrderFill, cause).
// A trace is the whole observable cadence: one row per source callback with
// the bar index, the position the script sees and the bar it is handed, then
// every closed trade and every surviving lot.
//
// Corpus breadth is not in this file: the 64 corpus/validation probes whose
// strategy.pine declares calc_on_order_fills are covered by the whole-corpus
// byte-identity sweep that accompanies the lane.
#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "";

#define CHECK(condition) do {                                                    \
    ++checks;                                                                    \
    if (!(condition)) {                                                          \
        ++failures;                                                              \
        std::fprintf(stderr, "FAIL [%s] %s:%d  %s\n", scenario, __FILE__,        \
                     __LINE__, #condition);                                      \
    }                                                                            \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string num(double value) {
    if (std::isnan(value)) return "na";
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

std::string integer(std::int64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return buffer;
}

// One probe shape for every scenario: it records the cadence it is driven
// with, so a re-lowering that moved a callback, dropped one, added one or
// changed what a callback sees cannot pass.
class TraceProbe : public source::PineNativeHost {
public:
    explicit TraceProbe(const source::PineStrategyConfig& config) {
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) final {
        const auto position = physical_position();
        trace_ += "#" + integer(callbacks_++)
            + ":b" + integer(bar_index_)
            + ":u" + num(position.signed_units)
            + ":l" + integer(static_cast<std::int64_t>(position.lot_count))
            + ":o" + num(bar.open) + ":h" + num(bar.high)
            + ":l" + num(bar.low) + ":c" + num(bar.close)
            + ":f" + integer(is_first_tick() ? 1 : 0) + "\n";
        script(bar);
    }

    // The trace the differential pins: the cadence, then the book it left.
    std::string trace() const {
        std::string out = trace_;
        const int closed = static_cast<int>(closed_trade_count());
        out += "T" + integer(closed) + "\n";
        for (int i = 0; i < closed; ++i) {
            out += "  t" + integer(i) + " " + closed_trade_entry_id(i)
                + "@" + num(closed_trade_entry_price(i))
                + "@" + integer(closed_trade_entry_time(i))
                + " > " + closed_trade_exit_id(i)
                + "@" + num(closed_trade_exit_price(i))
                + "@" + integer(closed_trade_exit_time(i))
                + " qty " + num(closed_trade_size(i))
                + " pnl " + num(closed_trade_profit(i)) + "\n";
        }
        const auto position = physical_position();
        out += "L" + integer(static_cast<std::int64_t>(position.lot_count)) + "\n";
        for (std::size_t i = 0; i < position.lot_count; ++i) {
            const int index = static_cast<int>(i);
            out += "  l" + integer(index) + " " + open_trade_entry_id(index)
                + "@" + num(open_trade_entry_price(index))
                + "@" + integer(open_trade_entry_time(index)) + "\n";
        }
        out += "U" + num(position.signed_units) + " C" + integer(callbacks_) + "\n";
        return out;
    }

    std::int64_t callbacks() const { return callbacks_; }
    std::int64_t script_bars() const { return script_bars_processed(); }
    std::int64_t lot_time(int index) const { return open_trade_entry_time(index); }
    double lot_price(int index) const { return open_trade_entry_price(index); }
    std::string lot_id(int index) const { return open_trade_entry_id(index); }
    std::uint64_t recalculations() const { return native_recalculation_count(); }
    std::uint64_t recalculations_skipped() const { return native_recalculations_skipped(); }
    const NativeRunSpec* spec() const { return native_state().spec; }

protected:
    virtual void script(const Bar&) = 0;

private:
    std::string trace_;
    std::int64_t callbacks_ = 0;
};

source::PineStrategyConfig coof_config() {
    source::PineStrategyConfig config;
    config.calc_on_order_fills = true;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 10;
    config.commission_value = 0.0;
    return config;
}

// ---------------------------------------------------------------------------
// The probes
// ---------------------------------------------------------------------------

// (1) The L4c refill cascade over a retained lower-timeframe feed: each fill
// recalculates, the recalculation opens the next lot, and TV's waypoint-only
// refill rule (the adapter's PendingCoofRequest deferral) decides where that
// newborn is presented.
class RefillProbe final : public TraceProbe {
public:
    RefillProbe() : TraceProbe(coof_config()) {}
protected:
    void script(const Bar&) override {
        if (bar_index_ <= 1 && std::abs(physical_position().signed_units) < 6.0)
            strategy_entry("L" + std::to_string(physical_position().lot_count), true);
    }
};

// (2) Two resting stop entries reached in one script bar: the chronology of
// the fill recalculations decides the lot order.
class ChronologyProbe final : public TraceProbe {
public:
    ChronologyProbe() : TraceProbe(coof_config()) {}
protected:
    void script(const Bar&) override {
        if (submitted_) return;
        submitted_ = true;
        strategy_entry("Far", true, kNaN, 108.0);
        strategy_entry("Near", true, kNaN, 105.0);
    }
private:
    bool submitted_ = false;
};

// (3) Market + limit + stop entries in the same statement block: the
// first-open chain, the two-fills-at-open rule and the waypoint deferral all
// run over one bar's O/H/L/C path.
class StopLimitProbe final : public TraceProbe {
public:
    StopLimitProbe() : TraceProbe(coof_config()) {}
protected:
    void script(const Bar&) override {
        if (submitted_) return;
        submitted_ = true;
        strategy_entry("M0", true);
        strategy_entry("M1", true);
        strategy_entry("A", true, 95.0, 108.0);
        strategy_entry("B103", true, kNaN, 103.0);
        strategy_entry("B105", true, kNaN, 105.0);
    }
private:
    bool submitted_ = false;
};

// (4) process_orders_on_close: a fill at the already-consumed close is final
// for its script bar and schedules NO source callback. The kernel still
// offers a recalculation at that cursor; Pine refuses it.
source::PineStrategyConfig pooc_config() {
    auto config = coof_config();
    config.process_orders_on_close = true;
    return config;
}

class PoocProbe final : public TraceProbe {
public:
    PoocProbe() : TraceProbe(pooc_config()) {}
protected:
    void script(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("P0", true);
        else if (bar_index_ == 1) strategy_entry("P1", true);
        else if (bar_index_ == 2) strategy_close("P0");
    }
};

// (5) Two same-entry stop exits reached on the same Low leg with pyramiding
// off: the adapter refuses the second fill's recalculation
// (suppress_grouped_stop_recalc), which the kernel has already offered.
source::PineStrategyConfig grouped_stop_config() {
    source::PineStrategyConfig config;
    config.calc_on_order_fills = true;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 2.0;
    config.pyramiding = 0;
    config.commission_value = 0.0;
    return config;
}

class GroupedStopProbe final : public TraceProbe {
public:
    GroupedStopProbe() : TraceProbe(grouped_stop_config()) {}
protected:
    void script(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", true);
            return;
        }
        if (bar_index_ == 1 && !armed_) {
            armed_ = true;
            strategy_exit("X1", "E", kNaN, 99.0, kNaN, kNaN, kNaN, kNaN, "", 1.0);
            strategy_exit("X2", "E", kNaN, 98.0, kNaN, kNaN, kNaN, kNaN, "", 1.0);
        }
    }
private:
    bool armed_ = false;
};

// (6) A plain confirmed-bar cascade with no intrabar path: entry, bracket and
// re-entry all born in fill recalculations.
class CascadeProbe final : public TraceProbe {
public:
    CascadeProbe() : TraceProbe(coof_config()) {}
protected:
    void script(const Bar& bar) override {
        if (physical_position().lot_count == 0) {
            strategy_entry("C" + std::to_string(entries_++), true);
            return;
        }
        if (bar.close > bar.open) strategy_close_all();
    }
private:
    int entries_ = 0;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

std::vector<Bar> lower_bars() {
    std::vector<Bar> lower;
    for (int i = 0; i < 30; ++i) {
        const double open = i < 15 ? 100.0 : 100.0 + (i - 15) * 0.1;
        lower.push_back({open, open + 1.0, open - 1.0, open + 0.25, 500.0,
                         static_cast<std::int64_t>(i) * 60000});
    }
    return lower;
}

const Bar kChronologyBars[] = {{100, 101, 99, 100, 1, 900000},
                               {100, 110, 99, 100, 1, 1800000}};
const Bar kStopLimitBars[] = {{100, 101, 99, 100, 1, 900000},
                              {100, 110, 85, 100, 1, 1800000},
                              {100, 104, 90, 95, 1, 2700000}};
const Bar kPoocBars[] = {{100, 102, 99, 101, 1, 900000},
                         {101, 103, 100, 102, 1, 1800000},
                         {102, 104, 101, 103, 1, 2700000},
                         {103, 105, 102, 104, 1, 3600000}};
// Both stops are armed on bar 1, whose own low clears them; bar 2's Low leg
// then reaches past both on the same matching point.
const Bar kGroupedStopBars[] = {{100, 101, 99.5, 100, 1, 900000},
                                {100, 101, 99.5, 100, 1, 1800000},
                                {100, 101, 96, 97, 1, 2700000}};
const Bar kCascadeBars[] = {{100, 102, 98, 101, 1, 900000},
                            {101, 104, 99, 100, 1, 1800000},
                            {100, 103, 97, 102, 1, 2700000},
                            {102, 106, 101, 105, 1, 3600000},
                            {105, 107, 100, 101, 1, 4500000}};

// ---------------------------------------------------------------------------
// Harvested from the PRE-lowering library (engine main b0cec54). See the file
// header: these are the adapter's own cadence and book, produced when the
// adapter still drove the recalculation itself.
// ---------------------------------------------------------------------------

const char kExpectRefill[] =
    "#0:b0:u0:l0:o100:h101:l99:c100.25:f1\n"
    "#1:b1:u1:l1:o100:h102.4:l99:c101.65:f1\n"
    "#2:b1:u2:l2:o100:h102.4:l99:c101.65:f1\n"
    "#3:b1:u3:l3:o100:h102.4:l99:c101.65:f1\n"
    "#4:b1:u4:l4:o100:h102.4:l99:c101.65:f1\n"
    "#5:b1:u5:l5:o100:h102.4:l99:c101.65:f1\n"
    "#6:b1:u6:l6:o100:h102.4:l99:c101.65:f1\n"
    "#7:b1:u6:l6:o100:h102.4:l99:c101.65:f1\n"
    "T0\n"
    "L6\n"
    "  l0 L0@100@900000\n"
    "  l1 L1@100@900000\n"
    "  l2 L2@99@900000\n"
    "  l3 L3@101@900000\n"
    "  l4 L4@100.25@900000\n"
    "  l5 L5@100.1@960000\n"
    "U6 C8\n";
const char kExpectChronology[] =
    "#0:b0:u0:l0:o100:h101:l99:c100:f1\n"
    "#1:b1:u1:l1:o100:h110:l99:c100:f1\n"
    "#2:b1:u2:l2:o100:h110:l99:c100:f1\n"
    "#3:b1:u2:l2:o100:h110:l99:c100:f1\n"
    "T0\n"
    "L2\n"
    "  l0 Near@105@1800000\n"
    "  l1 Far@108@1800000\n"
    "U2 C4\n";
const char kExpectStopLimit[] =
    "#0:b0:u0:l0:o100:h101:l99:c100:f1\n"
    "#1:b1:u1:l1:o100:h110:l85:c100:f1\n"
    "#2:b1:u2:l2:o100:h110:l85:c100:f1\n"
    "#3:b1:u3:l3:o100:h110:l85:c100:f1\n"
    "#4:b1:u4:l4:o100:h110:l85:c100:f1\n"
    "#5:b1:u5:l5:o100:h110:l85:c100:f1\n"
    "#6:b1:u5:l5:o100:h110:l85:c100:f1\n"
    "#7:b2:u5:l5:o100:h104:l90:c95:f1\n"
    "T0\n"
    "L5\n"
    "  l0 M0@100@1800000\n"
    "  l1 M1@100@1800000\n"
    "  l2 B103@103@1800000\n"
    "  l3 B105@105@1800000\n"
    "  l4 A@95@1800000\n"
    "U5 C8\n";
const char kExpectPooc[] =
    "#0:b0:u0:l0:o100:h102:l99:c101:f1\n"
    "#1:b1:u1:l1:o101:h103:l100:c102:f1\n"
    "#2:b2:u2:l2:o102:h104:l101:c103:f1\n"
    "#3:b3:u1:l1:o103:h105:l102:c104:f1\n"
    "T1\n"
    "  t0 P0@101@900000 > __close__P0@103@2700000 qty 1 pnl 2\n"
    "L1\n"
    "  l0 P1@102@1800000\n"
    "U1 C4\n";
const char kExpectGroupedStop[] =
    "#0:b0:u0:l0:o100:h101:l99.5:c100:f1\n"
    "#1:b1:u2:l1:o100:h101:l99.5:c100:f1\n"
    "#2:b1:u2:l1:o100:h101:l99.5:c100:f1\n"
    "#3:b2:u0:l0:o100:h101:l96:c97:f1\n"
    "#4:b2:u0:l0:o100:h101:l96:c97:f1\n"
    "T2\n"
    "  t0 E@100@1800000 > X1@99@2700000 qty 1 pnl -1\n"
    "  t1 E@100@1800000 > X2@98@2700000 qty 1 pnl -2\n"
    "L0\n"
    "U0 C5\n";
const char kExpectCascade[] =
    "#0:b0:u0:l0:o100:h102:l98:c101:f1\n"
    "#1:b1:u1:l1:o101:h104:l99:c100:f1\n"
    "#2:b1:u1:l1:o101:h104:l99:c100:f1\n"
    "#3:b2:u1:l1:o100:h103:l97:c102:f1\n"
    "#4:b3:u0:l0:o102:h106:l101:c105:f1\n"
    "#5:b3:u1:l1:o102:h106:l101:c105:f1\n"
    "#6:b3:u1:l1:o102:h106:l101:c105:f1\n"
    "#7:b4:u0:l0:o105:h107:l100:c101:f1\n"
    "#8:b4:u1:l1:o105:h107:l100:c101:f1\n"
    "#9:b4:u1:l1:o105:h107:l100:c101:f1\n"
    "T2\n"
    "  t0 C0@101@1800000 > __close__@102@3600000 qty 1 pnl 1\n"
    "  t1 C1@102@3600000 > __close__@105@4500000 qty 1 pnl 3\n"
    "L1\n"
    "  l0 C2@105@4500000\n"
    "U1 C10\n";
const char kExpectStream[] =
    "T0\n"
    "L0\n"
    "U0 C0\n";

void compare(const char* name, const std::string& actual, const char* expected) {
    scenario = name;
#ifdef PF_COOF_HARVEST
    std::printf("=== %s ===\n%s", name, actual.c_str());
    (void)expected;
#else
    ++checks;
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] adapter cadence differs from main b0cec54\n"
                             "--- expected ---\n%s--- actual ---\n%s",
                     name, expected, actual.c_str());
    }
#endif
}

// ---------------------------------------------------------------------------
// 1. The differential: same cadence, same book, before and after.
// ---------------------------------------------------------------------------

void test_differential() {
    const auto lower = lower_bars();

    RefillProbe refill;
    refill.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
               MagnifierDistribution::ENDPOINTS);
    scenario = "refill";
    CHECK(refill.last_error().empty());
    compare("refill", refill.trace(), kExpectRefill);

    ChronologyProbe chronology;
    chronology.run(kChronologyBars, 2);
    scenario = "chronology";
    CHECK(chronology.last_error().empty());
    compare("chronology", chronology.trace(), kExpectChronology);

    StopLimitProbe stops;
    stops.run(kStopLimitBars, 3);
    scenario = "stop-limit";
    CHECK(stops.last_error().empty());
    compare("stop-limit", stops.trace(), kExpectStopLimit);

    PoocProbe pooc;
    pooc.run(kPoocBars, 4);
    scenario = "pooc";
    CHECK(pooc.last_error().empty());
    compare("pooc", pooc.trace(), kExpectPooc);

    GroupedStopProbe grouped;
    grouped.run(kGroupedStopBars, 3);
    scenario = "grouped-stop";
    CHECK(grouped.last_error().empty());
    compare("grouped-stop", grouped.trace(), kExpectGroupedStop);

    CascadeProbe cascade;
    cascade.run(kCascadeBars, 5);
    scenario = "cascade";
    CHECK(cascade.last_error().empty());
    compare("cascade", cascade.trace(), kExpectCascade);

    // Stream: calc_on_order_fills has no forward-execution cadence to move.
    // The Pine host refuses the pairing outright, before any spec is
    // projected, so BarCloseAndFills never reaches a streaming run.
    CascadeProbe stream;
    scenario = "stream refusal";
    CHECK(!stream.stream_begin(kCascadeBars, 2, "15", "15"));
    CHECK(stream.last_error()
          == "native stream requires close-only calculation; "
             "calc_on_order_fills is unsupported");
    CHECK(stream.spec() == nullptr);
    compare("stream", stream.trace(), kExpectStream);
}

#ifndef PF_COOF_HARVEST
// ---------------------------------------------------------------------------
// 2. The cadence is the KERNEL's now: the projected spec says so, and the
//    consumer's own counters show it driving the fill recalculations.
// ---------------------------------------------------------------------------

void test_the_kernel_drives_the_cadence() {
    scenario = "spec projection";
    const auto lower = lower_bars();

    RefillProbe refill;
    refill.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(refill.last_error().empty());
    const auto* spec = refill.spec();
    CHECK(spec != nullptr);
    if (spec) {
        CHECK(spec->calculation == NativeCalculationTrigger::BarCloseAndFills);
        // TradingView's guard literal, handed to the generic bound.
        CHECK(spec->max_recalculations_per_point == source::kCoofLoopGuard);
        CHECK(spec->max_recalculations_per_point == (1U << 20));
        // CT4: the COOF callback reads the whole script bar.
        CHECK(spec->open_bar_view == NativeOpenBarView::Complete);
    }
    // Every calculation of the run is the kernel's: one per script bar, plus
    // one per applied execution it recalculated on. This fixture has no Pine
    // refusal, so the source callbacks account for exactly those two, and the
    // TV bound never binds.
    CHECK(refill.recalculations() > 0);
    CHECK(refill.callbacks()
          == refill.script_bars() + static_cast<std::int64_t>(refill.recalculations()));
    CHECK(refill.recalculations_skipped() == 0);

    // A run without calc_on_order_fills keeps the pre-lane default surface, so
    // its continuation identity is untouched by this lane.
    scenario = "coof off keeps BarClose";
    class PlainProbe final : public TraceProbe {
    public:
        PlainProbe() : TraceProbe(plain_config()) {}
        static source::PineStrategyConfig plain_config() {
            auto config = coof_config();
            config.calc_on_order_fills = false;
            return config;
        }
    protected:
        void script(const Bar&) override {
            if (physical_position().lot_count == 0) strategy_entry("P", true);
        }
    };
    PlainProbe plain;
    plain.run(kCascadeBars, 5);
    CHECK(plain.last_error().empty());
    const auto* plain_spec = plain.spec();
    CHECK(plain_spec != nullptr);
    if (plain_spec) {
        CHECK(plain_spec->calculation == NativeCalculationTrigger::BarClose);
        CHECK(plain_spec->max_recalculations_per_point == 8U);
        CHECK(plain_spec->open_bar_view == NativeOpenBarView::Complete);
    }
    // BarClose drives no recalculation at all: one calculation per script bar
    // and nothing else, which is the pre-lane surface byte for byte.
    CHECK(plain.recalculations() == 0);
    CHECK(plain.callbacks() == plain.script_bars());
    CHECK(plain.recalculations_skipped() == 0);
}

// ---------------------------------------------------------------------------
// 3. Pine still refuses the recalculations TradingView never scheduled. The
//    kernel offers one per applied execution; the source layer publishes
//    strictly fewer whenever a POOC close fill or a grouped-stop sibling is in
//    the run, and the refused ones publish nothing.
// ---------------------------------------------------------------------------

void test_pine_refusals_compose_with_the_kernel_cadence() {
    scenario = "pooc refusal";
    PoocProbe pooc;
    pooc.run(kPoocBars, 4);
    CHECK(pooc.last_error().empty());
    // The kernel offered one recalculation per applied execution; Pine
    // published strictly fewer callbacks than script bars + recalculations,
    // which is exactly the terminal-close refusal.
    CHECK(pooc.recalculations() > 0);
    CHECK(pooc.callbacks()
          < pooc.script_bars() + static_cast<std::int64_t>(pooc.recalculations()));
    CHECK(pooc.callbacks() == pooc.script_bars());
    CHECK(pooc.recalculations_skipped() == 0);

    scenario = "grouped-stop refusal";
    GroupedStopProbe grouped;
    grouped.run(kGroupedStopBars, 3);
    CHECK(grouped.last_error().empty());
    CHECK(grouped.recalculations() > 0);
    CHECK(grouped.callbacks()
          < grouped.script_bars() + static_cast<std::int64_t>(grouped.recalculations()));
    CHECK(grouped.recalculations_skipped() == 0);
}

// ---------------------------------------------------------------------------
// 4. The waypoint deferral is still needed: the kernel's birth rule alone does
//    not reproduce TradingView's eligibility. Lane L5 measured this against a
//    bare native twin (tests/test_native_calc_timing.cpp, CT11); here it is
//    measured against the adapter itself — the six refilled lots sit on the
//    chart bar's own O/L/H/C and the next chart open, NOT on the successive
//    sub-bar opens the unqualified birth rule would produce.
// ---------------------------------------------------------------------------

void test_the_waypoint_deferral_is_still_load_bearing() {
    scenario = "waypoint deferral";
    const auto lower = lower_bars();
    RefillProbe refill;
    refill.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(refill.last_error().empty());
    const auto position = refill.physical_position();
    CHECK(position.lot_count == 6);
    const std::vector<std::int64_t> waypoint_times = {900000, 900000, 900000,
                                                      900000, 900000, 960000};
    const std::vector<std::int64_t> birth_rule_times = {900000, 960000, 1020000,
                                                        1080000, 1140000, 1200000};
    for (std::size_t i = 0; i < position.lot_count && i < waypoint_times.size(); ++i) {
        const int index = static_cast<int>(i);
        CHECK(refill.lot_time(index) == waypoint_times[i]);
        CHECK(refill.lot_time(index) != birth_rule_times[i]
              || waypoint_times[i] == birth_rule_times[i]);
    }
}
#endif  // PF_COOF_HARVEST

}  // namespace

int main() {
    test_differential();
#ifndef PF_COOF_HARVEST
    test_the_kernel_drives_the_cadence();
    test_pine_refusals_compose_with_the_kernel_cadence();
    test_the_waypoint_deferral_is_still_load_bearing();
    std::printf("adapter COOF re-lowering: %d checks, %d failures\n", checks, failures);
#else
    std::printf("harvest: %d checks, %d failures\n", checks, failures);
#endif
    return failures == 0 ? 0 : 1;
}
