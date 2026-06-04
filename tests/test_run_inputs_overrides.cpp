// test_run_inputs_overrides.cpp — coverage for the input getters and the
// full run() overload (inputs + SymInfo + StrategyOverrides) in
// src/engine_run.cpp.
//
// Three concern groups, each pinning Pine-correct behaviour:
//
//   1. get_input_double / int / int64 / bool / string (lines 557-599):
//      valid parse, fallback-on-garbage (the catch(...) arms on bad numeric
//      strings), and the "true"/"1"/"false"/"0" bool grammar. These back the
//      generated code's input.* lookups; an operator override string that
//      cannot be parsed must silently fall back to the Pine default rather
//      than throw across the engine.
//
//   2. The run-with-overrides overload (lines 624-669): apply a
//      StrategyOverrides struct (initial_capital, pyramiding, slippage,
//      commission_value/type, default_qty_value/type, process_orders_on_close,
//      close_entries_rule), run a strategy, and assert the report/equity
//      reflect each field — initial_capital flows to equity, pyramiding caps
//      the number of same-direction market legs, commission reduces realized
//      PnL, process_orders_on_close changes the market fill price.
//
//   3. The timeframe auto-detection branch (line 301): call the TF-aware
//      overload with an EMPTY script_tf (and empty input_tf) so
//      detect_timeframe runs over the bar timestamps; assert the report's
//      input_tf_seconds / script_tf_seconds match the detected median delta.
//
// All expected values were derived by reading src/engine_run.cpp +
// src/engine_orders.cpp and confirmed by running this test.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {

// ── Group 1: input-getter probe ──────────────────────────────────────────
// Thin passthrough to the protected get_input_* surface.
struct GetterProbe : public BacktestEngine {
    void on_bar(const Bar&) override {}
    double dbl(const std::string& k, double d) const { return get_input_double(k, d); }
    int    integer(const std::string& k, int d) const { return get_input_int(k, d); }
    int64_t i64(const std::string& k, int64_t d) const { return get_input_int64(k, d); }
    bool   boolean(const std::string& k, bool d) const { return get_input_bool(k, d); }
    std::string str(const std::string& k, const std::string& d) const {
        return get_input_string(k, d);
    }
};

void test_get_input_double() {
    std::printf("test_get_input_double\n");
    GetterProbe p;
    // Valid float parses; partial trailing junk is tolerated by std::stod.
    p.set_input("len", "14.5");
    CHECK(near(p.dbl("len", 0.0), 14.5));
    p.set_input("neg", "-2.25");
    CHECK(near(p.dbl("neg", 0.0), -2.25));
    // Missing key → default.
    CHECK(near(p.dbl("absent", 7.0), 7.0));
    // Malformed numeric → catch(...) → default (NOT a throw).
    p.set_input("garbage", "not-a-number");
    CHECK(near(p.dbl("garbage", 3.5), 3.5));
    p.set_input("empty", "");
    CHECK(near(p.dbl("empty", 99.0), 99.0));
}

void test_get_input_int() {
    std::printf("test_get_input_int\n");
    GetterProbe p;
    p.set_input("n", "21");
    CHECK(p.integer("n", 0) == 21);
    p.set_input("neg", "-5");
    CHECK(p.integer("neg", 0) == -5);
    CHECK(p.integer("absent", 42) == 42);
    // std::stoi throws on a non-numeric leading char → catch(...) → default.
    p.set_input("bad", "xyz");
    CHECK(p.integer("bad", 13) == 13);
    p.set_input("empty", "");
    CHECK(p.integer("empty", -1) == -1);
}

void test_get_input_int64() {
    std::printf("test_get_input_int64\n");
    GetterProbe p;
    // ms-epoch value well past int32 range.
    p.set_input("ts", "1700000000000");
    CHECK(p.i64("ts", 0) == 1700000000000LL);
    CHECK(p.i64("absent", -9) == -9);
    p.set_input("bad", "abc");
    CHECK(p.i64("bad", 8) == 8);
}

void test_get_input_bool() {
    std::printf("test_get_input_bool\n");
    GetterProbe p;
    // Pine bool grammar: "true"/"1" → true, "false"/"0" → false.
    p.set_input("a", "true");
    CHECK(p.boolean("a", false) == true);
    p.set_input("b", "1");
    CHECK(p.boolean("b", false) == true);
    p.set_input("c", "false");
    CHECK(p.boolean("c", true) == false);
    p.set_input("d", "0");
    CHECK(p.boolean("d", true) == false);
    // Missing key → default (both polarities).
    CHECK(p.boolean("absent", true) == true);
    CHECK(p.boolean("absent", false) == false);
    // Any other string is NOT recognized → default is returned unchanged.
    p.set_input("weird", "yes");
    CHECK(p.boolean("weird", true) == true);
    CHECK(p.boolean("weird", false) == false);
}

void test_get_input_string() {
    std::printf("test_get_input_string\n");
    GetterProbe p;
    p.set_input("mode", "SMA");
    CHECK(p.str("mode", "EMA") == "SMA");
    CHECK(p.str("absent", "EMA") == "EMA");
    // Empty string is a PRESENT value — returned verbatim, not the default.
    p.set_input("blank", "");
    CHECK(p.str("blank", "fallback") == "");
}

// ── Group 2: run-with-overrides overload ─────────────────────────────────
//
// Strategy: place one market entry per bar with a distinct id and never
// close. Market entries fill at the NEXT bar's open. pyramiding=N caps the
// number of same-direction legs at N, so only the first N placements ever
// open a leg. With default_qty_value=Q (FIXED), each leg adds qty Q; final
// position holds N*Q contracts (no closed trades → net_profit==0, equity
// stays at initial_capital).
class PyramidEntryStrat : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        // Distinct ids so each call is a fresh pyramid-add attempt rather
        // than a same-id replacement.
        strategy_entry("E" + std::to_string(bar_index_), /*is_long=*/true);
    }
    // Observers for the protected runtime state.
    double equity() const { return current_equity(); }
    double init_cap() const { return initial_capital_; }
    double signed_size() const { return signed_position_size(); }
    int pyramiding() const { return pyramiding_; }
    int slippage() const { return slippage_; }
    double commission_value() const { return commission_value_; }
    int commission_type() const { return static_cast<int>(commission_type_); }
    double default_qty_value() const { return default_qty_value_; }
    int default_qty_type() const { return static_cast<int>(default_qty_type_); }
    bool process_orders_on_close() const { return process_orders_on_close_; }
    bool close_entries_rule_any() const { return close_entries_rule_any_; }
};

// Build a flat-priced rising-open bar series so every leg fills at a known
// open. 6 bars, opens 100, 101, 102, ... (range ±1).
static void make_bars(Bar* bars, int n, int64_t step_ms = 60'000) {
    double open_price = 100.0;
    for (int i = 0; i < n; ++i) {
        bars[i].open = open_price;
        bars[i].high = open_price + 1.0;
        bars[i].low = open_price - 1.0;
        bars[i].close = open_price;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * step_ms;
        open_price += 1.0;
    }
}

void test_overrides_applied_to_config_and_equity() {
    std::printf("test_overrides_applied_to_config_and_equity\n");
    PyramidEntryStrat s;

    StrategyOverrides ov;
    ov.initial_capital = 250000.0;
    ov.pyramiding = 2;
    ov.slippage = 3;
    ov.commission_value = 0.5;
    ov.commission_type = static_cast<int>(CommissionType::PERCENT);  // 0
    ov.default_qty_value = 4.0;
    ov.default_qty_type = static_cast<int>(QtyType::FIXED);          // 0
    ov.process_orders_on_close = 0;                                   // false
    ov.close_entries_rule = 1;                                        // ANY

    constexpr int N = 6;
    Bar bars[N];
    make_bars(bars, N);

    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;  // defaults: mintick 0.01, pointvalue 1.0

    s.run(bars, N, "1", "1", inputs, sym, &ov);

    CHECK(s.last_error().empty());

    // Every scalar override landed on the matching config field.
    CHECK(near(s.init_cap(), 250000.0));
    CHECK(s.pyramiding() == 2);
    CHECK(s.slippage() == 3);
    CHECK(near(s.commission_value(), 0.5));
    CHECK(s.commission_type() == static_cast<int>(CommissionType::PERCENT));
    CHECK(near(s.default_qty_value(), 4.0));
    CHECK(s.default_qty_type() == static_cast<int>(QtyType::FIXED));
    CHECK(s.process_orders_on_close() == false);
    CHECK(s.close_entries_rule_any() == true);

    // pyramiding=2 caps same-direction legs at 2; default_qty_value=4 each.
    // No leg ever closes → final long position holds 2*4 = 8 contracts.
    CHECK(near(s.signed_size(), 8.0));

    // No closed trades → net_profit==0 → equity stays at the overridden
    // initial_capital. (open_profit is not part of current_equity().)
    ReportC rep{};
    s.fill_report(&rep);
    CHECK(rep.total_trades == 0);
    CHECK(near(rep.net_profit, 0.0));
    CHECK(near(s.equity(), 250000.0));
    BacktestEngine::free_report(&rep);
}

// Larger pyramiding cap lets every placement through, proving the override
// is what bounds the leg count (not some other gate). With pyramiding=10 on
// a 6-bar series, the first 5 placements (bars 0..4) all fill (bar i's
// market order fills at bar i+1's open; bar 5's order would fill at bar 6
// which doesn't exist), so 5 legs open at qty 1 each → 5 contracts.
void test_overrides_large_pyramiding_opens_all_legs() {
    std::printf("test_overrides_large_pyramiding_opens_all_legs\n");
    PyramidEntryStrat s;

    StrategyOverrides ov;
    ov.initial_capital = 1'000'000.0;
    ov.pyramiding = 10;
    ov.default_qty_value = 1.0;
    ov.default_qty_type = static_cast<int>(QtyType::FIXED);

    constexpr int N = 6;
    Bar bars[N];
    make_bars(bars, N);

    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;
    s.run(bars, N, "1", "1", inputs, sym, &ov);

    CHECK(s.last_error().empty());
    CHECK(s.pyramiding() == 10);
    // 5 legs fill (bars 0..4 fill at bars 1..5 open); bar 5's order can't
    // fill (no bar 6). Each leg qty 1 → 5 contracts long.
    CHECK(near(s.signed_size(), 5.0));
}

// nullptr overrides leaves the engine's compiled-in defaults intact. The
// PyramidEntryStrat ctor is the implicit default: initial_capital_ 1e6,
// pyramiding_ 1, default_qty_value_ 1. With pyramiding=1 only the first leg
// opens → 1 contract.
void test_overrides_null_keeps_defaults() {
    std::printf("test_overrides_null_keeps_defaults\n");
    PyramidEntryStrat s;

    constexpr int N = 6;
    Bar bars[N];
    make_bars(bars, N);

    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;
    s.run(bars, N, "1", "1", inputs, sym, /*overrides=*/nullptr);

    CHECK(s.last_error().empty());
    CHECK(near(s.init_cap(), 1'000'000.0));  // BacktestEngine default
    CHECK(s.pyramiding() == 1);              // BacktestEngine default
    // Only the first placement opens a leg; the rest are gated by the
    // default pyramiding=1. 1 contract long.
    CHECK(near(s.signed_size(), 1.0));
}

// process_orders_on_close override changes the market fill price: when ON,
// a market order placed in on_bar fills at THIS bar's close instead of the
// next bar's open. We verify by realizing a closed trade and comparing PnL.
class CloseThenExitStrat : public BacktestEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", /*is_long=*/true);
        if (bar_index_ == 1) strategy_close("L", "exit");
    }
    int trades() const { return (int)trades_.size(); }
    double trade_pnl(int i) const { return trades_[i].pnl; }
    double trade_entry(int i) const { return trades_[i].entry_price; }
    double trade_exit(int i) const { return trades_[i].exit_price; }
};

void test_override_process_orders_on_close_fills_at_close() {
    std::printf("test_override_process_orders_on_close_fills_at_close\n");
    // Bars: open != close so the close-fill vs next-open-fill prices differ.
    constexpr int N = 4;
    Bar bars[N];
    for (int i = 0; i < N; ++i) {
        bars[i].open = 100.0 + i * 10.0;   // 100, 110, 120, 130
        bars[i].close = bars[i].open + 5.0; // 105, 115, 125, 135
        bars[i].high = bars[i].close + 1.0;
        bars[i].low = bars[i].open - 1.0;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
    }

    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;  // mintick 0.01 → directional snap is a no-op on these prices

    // process_orders_on_close = ON: entry placed bar 0 fills at bar 0 close
    // (105), close placed bar 1 fills at bar 1 close (115). PnL = (115-105)*1.
    {
        CloseThenExitStrat s;
        StrategyOverrides ov;
        ov.process_orders_on_close = 1;  // ON
        ov.slippage = 0;
        ov.commission_value = 0.0;
        s.run(bars, N, "1", "1", inputs, sym, &ov);
        CHECK(s.last_error().empty());
        CHECK(s.trades() == 1);
        if (s.trades() == 1) {
            CHECK(near(s.trade_entry(0), 105.0));
            CHECK(near(s.trade_exit(0), 115.0));
            CHECK(near(s.trade_pnl(0), 10.0));
        }
    }

    // process_orders_on_close = OFF: entry placed bar 0 fills at bar 1 open
    // (110), close placed bar 1 fills at bar 2 open (120). PnL = (120-110)*1.
    {
        CloseThenExitStrat s;
        StrategyOverrides ov;
        ov.process_orders_on_close = 0;  // OFF
        ov.slippage = 0;
        ov.commission_value = 0.0;
        s.run(bars, N, "1", "1", inputs, sym, &ov);
        CHECK(s.last_error().empty());
        CHECK(s.trades() == 1);
        if (s.trades() == 1) {
            CHECK(near(s.trade_entry(0), 110.0));
            CHECK(near(s.trade_exit(0), 120.0));
            CHECK(near(s.trade_pnl(0), 10.0));
        }
    }
}

// Commission override (CASH_PER_ORDER) flows into realized PnL. With a flat
// market (entry open == exit open) the gross PnL is 0, so the net PnL equals
// -(entry_commission + exit_commission) = -2 * commission_value.
void test_override_commission_reduces_pnl() {
    std::printf("test_override_commission_reduces_pnl\n");
    constexpr int N = 4;
    Bar bars[N];
    for (int i = 0; i < N; ++i) {
        bars[i].open = 100.0;   // flat market → zero gross PnL
        bars[i].close = 100.0;
        bars[i].high = 101.0;
        bars[i].low = 99.0;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
    }

    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;

    CloseThenExitStrat s;
    StrategyOverrides ov;
    ov.process_orders_on_close = 0;
    ov.slippage = 0;
    ov.commission_value = 2.5;
    ov.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);  // 1
    ov.default_qty_value = 1.0;
    ov.default_qty_type = static_cast<int>(QtyType::FIXED);
    s.run(bars, N, "1", "1", inputs, sym, &ov);

    CHECK(s.last_error().empty());
    CHECK(s.trades() == 1);
    if (s.trades() == 1) {
        // Gross PnL 0; two CASH_PER_ORDER commissions of 2.5 each → -5.0.
        CHECK(near(s.trade_pnl(0), -5.0));
    }
}

// ── Group 3: timeframe auto-detection (empty tf strings) ─────────────────
//
// With an EMPTY input_tf AND empty script_tf, the TF-aware overload runs
// detect_timeframe over the bar timestamps. detect_timeframe takes the
// MEDIAN inter-bar delta and snaps to the nearest standard TF label, then
// fill_report converts that label back to seconds via tf_to_seconds.
void test_empty_tf_triggers_detect_timeframe() {
    std::printf("test_empty_tf_triggers_detect_timeframe\n");

    // 5-minute spacing → median delta 300s → detect_timeframe → "5".
    {
        PyramidEntryStrat s;
        constexpr int N = 6;
        Bar bars[N];
        make_bars(bars, N, /*step_ms=*/300'000);  // 5 minutes
        std::unordered_map<std::string, std::string> inputs;
        SymInfo sym;
        StrategyOverrides ov;
        ov.pyramiding = 10;  // irrelevant here, just keep config explicit
        // Empty input_tf + empty script_tf → both go through detect_timeframe.
        s.run(bars, N, "", "", inputs, sym, &ov);
        CHECK(s.last_error().empty());
        ReportC rep{};
        s.fill_report(&rep);
        CHECK(rep.input_tf_seconds == 300);
        CHECK(rep.script_tf_seconds == 300);
        // Same TF on input + script → no aggregation.
        CHECK(rep.needs_aggregation == 0);
        BacktestEngine::free_report(&rep);
    }

    // 60-minute spacing → median delta 3600s → detect_timeframe → "60".
    {
        PyramidEntryStrat s;
        constexpr int N = 6;
        Bar bars[N];
        make_bars(bars, N, /*step_ms=*/3'600'000);  // 1 hour
        std::unordered_map<std::string, std::string> inputs;
        SymInfo sym;
        s.run(bars, N, "", "", inputs, sym, /*overrides=*/nullptr);
        CHECK(s.last_error().empty());
        ReportC rep{};
        s.fill_report(&rep);
        CHECK(rep.input_tf_seconds == 3600);
        CHECK(rep.script_tf_seconds == 3600);
        BacktestEngine::free_report(&rep);
    }

    // Daily spacing → median delta 86400s → detect_timeframe → "D" → 86400s.
    {
        PyramidEntryStrat s;
        constexpr int N = 6;
        Bar bars[N];
        make_bars(bars, N, /*step_ms=*/86'400'000);  // 1 day
        std::unordered_map<std::string, std::string> inputs;
        SymInfo sym;
        s.run(bars, N, "", "", inputs, sym, /*overrides=*/nullptr);
        CHECK(s.last_error().empty());
        ReportC rep{};
        s.fill_report(&rep);
        CHECK(rep.input_tf_seconds == 86400);
        CHECK(rep.script_tf_seconds == 86400);
        BacktestEngine::free_report(&rep);
    }
}

}  // namespace

int main() {
    std::printf("--- run inputs + overrides + tf-detect ---\n");
    test_get_input_double();
    test_get_input_int();
    test_get_input_int64();
    test_get_input_bool();
    test_get_input_string();
    test_overrides_applied_to_config_and_equity();
    test_overrides_large_pyramiding_opens_all_legs();
    test_overrides_null_keeps_defaults();
    test_override_process_orders_on_close_fills_at_close();
    test_override_commission_reduces_pnl();
    test_empty_tf_triggers_detect_timeframe();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
