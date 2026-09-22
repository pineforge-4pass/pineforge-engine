#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R5 lane R5 — the adapter's TradingView margin call, re-lowered onto the
// kernel's generic margin model (L4 + L4b).
//
// This is a DIFFERENTIAL: every expectation below is a value harvested from
// the adapter as it stood on main b0cec54 ("Port the wave-2 lanes'
// hash-neutrality guards to native_run_spec_digest()"), BEFORE any part of
// the margin call moved into the kernel. The harvest was taken by compiling
// this same translation unit against that library in a worktree at that sha
// (/private/tmp/pf-r5-margin-base) and running it with PF_DUMP=1; the printed
// rows are the literals pinned in `kExpected` below. After the re-lowering
// the adapter must reproduce them bit for bit from the kernel's model, its
// two host hooks and its check gate.
//
// Rows are named for the R5 analysis table (docs/design/native-feature-parity
// §1.4 MG*): MG-F/MG-F2 the 4x shortfall sizing, MG-H2 the tick-quantized
// call fill price, MG-A the fee-adjusted trigger equity, MG-B the rounded-
// money admission, MG-G the whole-drop band (family R), MG-E the 1x-long
// money call, MG-I the TV-gated scheduling sites.
//
// Each observation is taken through the PUBLIC report surface only -- closed
// trade rows plus closed_trade_close_cause() -- so a re-lowering that changed
// the liquidation's ticket id, its comment or its lot attribution would fail
// here even if the money were right.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

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

Bar bar(std::int64_t timestamp, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

// One observed liquidation row, as the public report shows it.
struct MarginRow {
    double units;
    double price;
    const char* exit_id;
    const char* comment;
    int close_cause;
    bool is_long;
};

// One scenario's whole observable margin outcome.
struct Outcome {
    std::vector<MarginRow> rows;
    std::string owned;           // storage for the ids/comments above
    double final_position = 0.0;
    int total_trades = 0;
};

class Probe : public source::PineStrategyHost {
public:
    // Every closed row whose close cause is MARGIN_CALL, in report order.
    std::vector<MarginRow> margin_rows(std::vector<std::string>& storage) const {
        std::vector<MarginRow> out;
        for (int i = 0; i < report_trade_count(); ++i) {
            if (closed_trade_close_cause(i) != 3) continue;
            const auto& row = get_report_trade(i);
            storage.push_back(row.exit_id);
            storage.push_back(row.exit_comment);
            out.push_back({row.qty, row.exit_price, nullptr, nullptr, 3, row.is_long});
        }
        return out;
    }
    double position() const { return physical_position().signed_units; }
};

// --------------------------------------------------------------- scenarios
// Every configuration below is one of the L4a margin family's own, or the L4b
// twin's (tests/test_native_margin_hooks.cpp), so the two lanes measure the
// same books.

// MG-F / MG-H. test_margin_call_l4a.cpp's leveraged long: 20 units at 100 on
// 1000 of capital at 50 % margin, then a bar whose low is 95.
//   restore = (20*95*0.5 - (1000 + 20*(95-100))) / (95*0.5) = 1.0526315789473684
//   4x      = 4.2105263157894735, booked at the adverse waypoint 95.
class LeveragedLong final : public Probe {
public:
    LeveragedLong() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-F2. The same rule on the short side, where the adverse extreme is the
// bar's high: 10 units short at 100 on a 100 % margin, then a bar reaching
// 105. 4x restore = 3.8095238095238093 at 105.
class LeveragedShort final : public Probe {
public:
    LeveragedShort() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 10.0);
    }
};

// MG-H2. The call's fill price: the tick-rounded fire price plus the EXIT
// side's own market slippage, never the generic slipped default. Capital 1020
// so the two-tick-up entry at 100.02 is itself not a breach.
class SlippedLong final : public Probe {
public:
    SlippedLong() {
        initial_capital_ = 1020.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        slippage_ = 2;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-A. A cash-per-order entry fee. TradingView's margin equity does not
// charge a still-open entry's cash commission against the account, so this
// book -- marked equity 990 against a requirement of 1000 at the entry mark --
// is NOT called, though the kernel's own marked-equity numbers would call it.
class CashFeeLong final : public Probe {
public:
    CashFeeLong() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 10.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-B + MG-G. A lot worth less than one unit of account puts the requirement
// on TradingView's ten-significant-digit money ladder, and that rounding alone
// IS the call: the exact requirement is below the equity and the rounded one
// above it. The restore it implies is sub-lot, so the family R whole-drop band
// closes one whole contract rather than nibbling.
class RoundedMoneyLong final : public Probe {
public:
    RoundedMoneyLong() {
        initial_capital_ = 1000.00000005;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        qty_step_ = 0.001;
        syminfo_mintick_ = 1e-9;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-E. The 1x-long money call of test_tv_money_long_margin_call_eth_l4a.cpp
// (corpus probe anomaly-equity-mirror-strategy-equity-01, 04-21 tape). This is
// NOT a maintenance liquidation: there is no maintenance fraction in it, no
// restore and no 4x. One whole contract is closed because the position's
// ten-significant-digit valuation exceeds an equity its exact valuation does
// not, at the first path point where that holds -- which for a long is a HIGH
// price, i.e. the opposite side of the path from any liquidation mark.
class MoneyLong final : public Probe {
public:
    MoneyLong() {
        initial_capital_ = 992399.54089 + 0.00013;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && position() == 0.0)
            strategy_entry("E", true, kNaN, kNaN, 623.163);
        if (position() > 0.0 && pine_bar_index() > entry_bar_) {
            if (entry_bar_ < 0) entry_bar_ = pine_bar_index();
            if (pine_bar_index() > entry_bar_) strategy_close("E", "next-bar flatten");
        }
    }
private:
    int entry_bar_ = -1;
};

// MG-I. The once-per-script-bar latch. A book that keeps breaching as the
// tape falls takes exactly one slice per script bar, never two, and each
// bar's slice is sized against the book the previous one left.
class RepeatedBreach final : public Probe {
public:
    RepeatedBreach() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-I2. The post-exit re-size. A path slice rests at the bar's adverse
// extreme, sized on the book standing at the bar open; a priced bracket leg
// of the SAME script bar then fills and reduces that book before the extreme
// is reached. ab9714be pine_scheduler.cpp:267-282 cancels the live slice at
// that fill and re-schedules from the reduced book, so the slice sized on the
// pre-exit position never executes: here the remainder is comfortably funded
// at the remaining path's adverse mark, and the whole bar books NO margin row.
//
// Short 10 at 100 on 1050 of capital at 100 % margin, a stop-5 bracket leg at
// 102, and a bar reaching 105. The capital puts the breach threshold at
// (1050 + 1000) / 20 = 102.5, above the previous bar's high and above the
// leg's own stop, so the ONLY breach on the tape is the last bar's extreme:
//   bar open 100  required 1000 < equity 1050, no call;
//   path slice    at 105: (10*105 - 1000)/105 * 4 = 1.9047619047619047 rests;
//   leg fills     5 at 102, leaving 5 and a realized -10;
//   re-size       at the remaining adverse 104: 5*104 = 520 < equity 1020 ->
//                 the slice is withdrawn and nothing replaces it.
// Without the re-size the stale slice reaches 105 and liquidates the
// remainder (regression: stockhunter2025-btcusd-4h-ema-swing-strategy on
// BINANCE:BTCUSDT@15, wave-4 sweep exp-r5-int4-wave4-20260920).
class PostExitResizeShort final : public Probe {
public:
    PostExitResizeShort() {
        initial_capital_ = 1050.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1) strategy_entry("S", false, kNaN, kNaN, 10.0);
        if (pine_bar_index() == 2)
            strategy_exit("X", "S", kNaN, 102.0, kNaN, kNaN, kNaN, kNaN, "leg", 5.0);
    }
};

// MG-F3. The gridded shortfall, R5 N11. TradingView floors the restore onto
// the lot grid BEFORE the multiple: 20 long at 100 on 1000 at 50 % margin on
// a one-unit lot grid, a bar reaching 93.9:
//   restore = (20*93.9*0.5 - (1000 + 20*(93.9-100))) / (93.9*0.5)
//           = 1.2992545260915842
//   TV      = floor(1.2992...) * 4 = 4 lots, floored again = 4
// where the kernel's own ShortfallMultiple(4.0) would book floor(5.197) = 5.
// The adapter's units hook is the sizing authority, so the run spec declares
// no sizing knob at all (project()); this row is the measurement that the
// number comes from the hook and from nothing the spec says.
class GriddedShortfallLong final : public Probe {
public:
    GriddedShortfallLong() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        qty_step_ = 1.0;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// MG-K. set_margin_call_enabled(false) must still suppress every call: that is
// the C setter's pinned semantics, whatever owns the model underneath.
class DisabledLong final : public Probe {
public:
    DisabledLong() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 50.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
        set_margin_call_enabled(false);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

// ------------------------------------------------------------------ driver

std::vector<Bar> long_break_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0), bar(60000, 100.0, 101.0, 95.0, 96.0)};
}
std::vector<Bar> short_break_tape() {
    return {bar(0, 100.0, 100.0, 99.0, 100.0), bar(60000, 100.0, 105.0, 99.5, 104.0)};
}
std::vector<Bar> gridded_break_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0), bar(60000, 100.0, 101.0, 93.9, 95.0)};
}
std::vector<Bar> quiet_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0), bar(60000, 100.0, 100.5, 100.0, 100.2)};
}
std::vector<Bar> rounded_money_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0),
            bar(60000, 100.0, 100.000000004, 99.999999996, 100.0)};
}
// The 04-21 ETHUSDT.P@15 lab bars of the money-call family (feed 27b62431096e).
std::vector<Bar> eth_0421_tape() {
    return {bar(1745192700000LL, 1583.8, 1587.0, 1583.49, 1586.56),
            bar(1745193600000LL, 1586.57, 1593.75, 1585.28, 1592.52),
            bar(1745194500000LL, 1592.52, 1613.8, 1592.52, 1608.96),
            bar(1745195400000LL, 1608.96, 1619.86, 1606.17, 1613.78),
            bar(1745196300000LL, 1613.78, 1620.0, 1608.08, 1609.49),
            bar(1745197200000LL, 1609.5, 1618.0, 1607.26, 1610.81)};
}
// The entry fills at bar 2's open, the bracket leg rests over bar 3, and bar
// 3's high is the adverse extreme the path slice would be taken at.
std::vector<Bar> post_exit_resize_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0), bar(60000, 100.0, 100.0, 100.0, 100.0),
            bar(120000, 100.0, 100.5, 99.5, 100.0), bar(180000, 100.0, 105.0, 99.5, 104.0)};
}
// A tape that keeps breaching, one script bar after another.
std::vector<Bar> staircase_tape() {
    return {bar(0, 100.0, 100.0, 100.0, 100.0), bar(60000, 100.0, 101.0, 95.0, 96.0),
            bar(120000, 96.0, 96.0, 88.0, 89.0), bar(180000, 89.0, 89.0, 80.0, 81.0),
            bar(240000, 81.0, 82.0, 74.0, 75.0)};
}

template <typename HostT>
Outcome run(const std::vector<Bar>& bars) {
    HostT host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    Outcome out;
    std::vector<std::string> storage;
    out.rows = host.margin_rows(storage);
    for (std::size_t i = 0; i < out.rows.size(); ++i) {
        out.rows[i].exit_id = nullptr;
        out.rows[i].comment = nullptr;
    }
    // Keep the strings alive alongside the rows.
    for (std::size_t i = 0; i < out.rows.size(); ++i) {
        out.owned += storage[2 * i] + "\x1f" + storage[2 * i + 1] + "\x1e";
    }
    out.final_position = host.position();
    out.total_trades = host.report_trade_count();
    return out;
}

// The harvested ("before") expectation for one scenario.
struct Expected {
    const char* name;
    int row_count;
    const double* units;   // row_count entries
    const double* prices;  // row_count entries
    const char* ids;       // "\x1f"/"\x1e" packed id+comment pairs
    double final_position;
    int total_trades;
};

void report(const char* name, const Outcome& got) {
    std::printf("DUMP %s rows=%zu pos=%.17g trades=%d\n", name, got.rows.size(),
                got.final_position, got.total_trades);
    std::printf("DUMP %s ids=%s\n", name, got.owned.c_str());
    for (std::size_t i = 0; i < got.rows.size(); ++i) {
        std::printf("DUMP %s [%zu] units=%.17g price=%.17g cause=%d long=%d\n", name, i,
                    got.rows[i].units, got.rows[i].price, got.rows[i].close_cause,
                    got.rows[i].is_long ? 1 : 0);
    }
}

void verify(const Expected& e, const Outcome& got) {
    if (dumping) { report(e.name, got); return; }
    bool ok = static_cast<int>(got.rows.size()) == e.row_count;
    if (!ok) {
        std::printf("FAIL %s: %zu margin rows, expected %d\n", e.name, got.rows.size(),
                    e.row_count);
    }
    CHECK(static_cast<int>(got.rows.size()) == e.row_count);
    for (int i = 0; ok && i < e.row_count; ++i) {
        const bool same = got.rows[i].units == e.units[i] && got.rows[i].price == e.prices[i];
        if (!same) {
            std::printf("FAIL %s[%d]: %.17g @ %.17g, expected %.17g @ %.17g\n", e.name, i,
                        got.rows[i].units, got.rows[i].price, e.units[i], e.prices[i]);
        }
        CHECK(same);
        // Every liquidation row must still be reported as MARGIN_CALL (close
        // cause 3), which is derived from the ticket id "__margin_call__".
        CHECK(got.rows[i].close_cause == 3);
    }
    if (got.owned != e.ids) {
        std::printf("FAIL %s ids: %s, expected %s\n", e.name, got.owned.c_str(), e.ids);
    }
    CHECK(got.owned == e.ids);
    CHECK(got.final_position == e.final_position);
    CHECK(got.total_trades == e.total_trades);
}

} // namespace

#include "adapter_margin_relower_expectations.inc"

int main() {
    dumping = std::getenv("PF_DUMP") != nullptr;
    verify(kLeveragedLong, run<LeveragedLong>(long_break_tape()));
    verify(kLeveragedShort, run<LeveragedShort>(short_break_tape()));
    verify(kSlippedLong, run<SlippedLong>(long_break_tape()));
    verify(kCashFeeLong, run<CashFeeLong>(quiet_tape()));
    verify(kRoundedMoneyLong, run<RoundedMoneyLong>(rounded_money_tape()));
    verify(kMoneyLong, run<MoneyLong>(eth_0421_tape()));
    verify(kRepeatedBreach, run<RepeatedBreach>(staircase_tape()));
    verify(kPostExitResizeShort, run<PostExitResizeShort>(post_exit_resize_tape()));
    verify(kGriddedShortfallLong, run<GriddedShortfallLong>(gridded_break_tape()));
    verify(kDisabledLong, run<DisabledLong>(long_break_tape()));
    // R5 lane F7: the adapter defines no margin-scheduling predicate that no
    // path consults. These three were defined, declared and reached from
    // nowhere -- two carried-POOC-short scopes and a COOF waypoint price, a
    // silent second copy of rules the live schedule no longer reads.
#if defined(PINEFORGE_F7_ADAPTER_FILE)
    {
        std::string adapter;
        if (std::FILE* in = std::fopen(PINEFORGE_F7_ADAPTER_FILE, "rb")) {
            char buffer[4096];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof buffer, in)) > 0) adapter.append(buffer, got);
            std::fclose(in);
        }
        CHECK(!adapter.empty());
        for (const char* name : {"carried_pooc_short_margin_before_script_scope(",
                                 "carried_pooc_short_priced_exit_after_adverse_scope(",
                                 "next_coof_waypoint_price("}) {
            const bool defined = adapter.find(name) != std::string::npos;
            if (defined) std::printf("  adapter still defines %s)\n", name);
            CHECK(!defined);
        }
    }
#else
    std::printf("PINEFORGE_F7_ADAPTER_FILE undefined\n");
    CHECK(false);
#endif
    std::printf("%d checks, %d failures\n", passed + failed, failed);
    return failed == 0 ? 0 : 1;
}
