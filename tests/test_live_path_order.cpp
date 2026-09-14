#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
Bar bar(double o, double h, double l, double c, int64_t ts) { return Bar{o, h, l, c, 1.0, ts}; }

// Long from bar 1 with a bracket stop 99 / limit 101 issued on bar 1; bar 2
// is the touch bar, parameterised so both an AUTO-high-first and an
// AUTO-low-first shape can drive the same fixture (Important 1: a test
// bar that is already high-first under AUTO cannot tell HIGH_FIRST from
// AUTO, since both branches agree).
class Bracket final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        // strategy_exit(id, from_entry, limit_price, stop_price, ...) --
        // engine.hpp's real parameter order puts limit_price BEFORE
        // stop_price (the task brief's illustrative call had them swapped).
        if (bar_index_ == 1) strategy_exit("x", "L", 101.0, 99.0);
    }
};
double exit_price_under(const Bar& touch_bar, int mode) {
    const std::vector<Bar> bars = {
        bar(100, 100, 100, 100, 0), bar(100, 100, 100, 100, 60'000),
        touch_bar,
    };
    Bracket s;
    s.set_path_order(mode);
    s.run(bars.data(), 3);
    return s.trade_count() == 1 ? s.get_trade(0).exit_price : NAN;
}
// |H-O| = 1.5 < |O-L| = 2 -> AUTO is high first (limit at 101 touched first).
const Bar kHighFirstTouchBar = bar(100, 101.5, 98.0, 100, 120'000);
// |H-O| = 2 > |O-L| = 1.5 -> AUTO is low first (stop at 99 touched first).
const Bar kLowFirstTouchBar = bar(100, 102.0, 98.5, 100, 120'000);

// Flat position resting one long stop-only ENTRY above open and one short
// stop-only ENTRY below open (both placed on bar 0's close); bar 1 touches
// both, at path positions that differ under HIGH_FIRST vs LOW_FIRST, so the
// forced order actually decides the winner (unlike a degenerate O=H=L=C
// bar, where both stops are marketable at the open and tie at position 0
// regardless of leg order).
class DualEntryPair final : public pineforge::source::PineStrategyHost {
public:
    DualEntryPair() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 0;
        margin_long_ = 100;
        margin_short_ = 100;
        syminfo_mintick_ = 0.01;
        // process_orders_on_close_ defaults to false (single
        // process_pending_orders call per bar), which would make the
        // no-tail-suppression variant below indistinguishable from the
        // tail-suppressed one. Force it on so that variant actually
        // exercises the two-pass (old-order settlement, then new-order
        // fills) structure Important 2 flagged.
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, na<double>(), 101.0, 1.0);
            strategy_entry("S", false, na<double>(), 99.0, 1.0);
        }
    }
};
// H=102 >= 101 (long stop) and L=98 <= 99 (short stop): both touched.
// HIGH_FIRST path O->H->L->C: long stop reached at pos 0.5, short stop at
// pos 1.75 -> LongFirst. LOW_FIRST path O->L->H->C: short stop at pos 0.5,
// long stop at pos 1.75 -> ShortFirst.
const Bar kDualEntryTouchBar = bar(100, 102.0, 98.0, 100, 60'000);

int dual_entry_winner_probe(int mode) {
    const std::vector<Bar> bars = {bar(100, 100, 100, 100, 0), kDualEntryTouchBar};
    DualEntryPair s;
    s.set_path_order(mode);
    // The live probe's actual read: the touch bar is the tail-suppressed
    // forming bar, so process_pending_orders runs exactly once for it.
    s.set_probe_suppress_tail_logic(true);
    s.run(bars.data(), (int)bars.size());
    return s.last_bar_dual_entry_path();
}
// Stale-value check: a pair-less bar appended after the touch bar must read
// None, not the touch bar's leftover decision (proves the per-bar reset,
// not just the per-pass one dual_entry_path_ already had).
int dual_entry_winner_after_pairless_bar() {
    const std::vector<Bar> bars = {
        bar(100, 100, 100, 100, 0), kDualEntryTouchBar,
        bar(100, 100, 100, 100, 120'000),
    };
    DualEntryPair s;
    s.set_path_order(1);
    s.set_probe_suppress_tail_logic(true);
    s.run(bars.data(), (int)bars.size());
    return s.last_bar_dual_entry_path();
}
// POOC variant with NO tail suppression: the touch bar dispatches BOTH
// process_pending_orders passes (old-order settlement, then new-order
// fills) and the winning stop actually fills by the second pass, which
// resets the per-pass dual_entry_path_ back to None (position no longer
// flat). last_bar_dual_entry_path() must still report the real decision --
// this is what the per-bar snapshot buys over reading dual_entry_path_
// directly.
int dual_entry_winner_pooc_no_tail_suppression() {
    const std::vector<Bar> bars = {bar(100, 100, 100, 100, 0), kDualEntryTouchBar};
    DualEntryPair s;
    s.set_path_order(1);
    s.run(bars.data(), (int)bars.size());
    return s.last_bar_dual_entry_path();
}
// F6 pin (final review): a rerun that dispatches ZERO script bars never
// reaches dispatch_bar()'s own per-bar reset of last_bar_dual_entry_decision_
// (the per-bar loop bodies never execute), so reset_run_state() must clear
// it itself -- otherwise a reused handle's last_bar_dual_entry_path() would
// still read the PRIOR run's decision instead of the documented "no
// decision" value (0 / None).
int dual_entry_winner_after_empty_rerun() {
    const std::vector<Bar> bars = {bar(100, 100, 100, 100, 0), kDualEntryTouchBar};
    DualEntryPair s;
    s.set_path_order(1);
    s.set_probe_suppress_tail_logic(true);
    s.run(bars.data(), (int)bars.size());
    if (s.last_bar_dual_entry_path() != 1) return -99;  // sanity: fixture still decides LongFirst
    s.run(bars.data(), 0);  // zero script bars -- dispatch_bar() never runs this call
    return s.last_bar_dual_entry_path();
}
// final-rereview.md N4: the F6 fix also added a reset at the top of
// stream_dispatch_script_bar (engine_stream.cpp) -- stream mode calls
// process_pending_orders() directly and never goes through dispatch_bar(),
// so that function's own per-bar reset (already pinned above by the
// plain-run tests) never runs for a realtime stream bar. Drive the same
// dual-entry fixture through stream_begin/stream_advance_time so bar k's
// arbitration happens in the warmup run() (ordinary dispatch_bar(), ALREADY
// reset pre-fix) and bar k+1 -- pairless -- is dispatched entirely through
// stream_dispatch_script_bar, the one reset site this file's other cases
// never reach.
int dual_entry_winner_stream_after_pairless_bar() {
    const std::vector<Bar> warmup = {bar(100, 100, 100, 100, 0), kDualEntryTouchBar};
    DualEntryPair s;
    s.set_path_order(1);  // HIGH_FIRST -> LongFirst, as in dual_entry_winner_probe(1)
    if (!s.stream_begin(warmup.data(), (int)warmup.size(), "1", "1")) return -98;
    if (s.last_bar_dual_entry_path() != 1) return -99;  // sanity: warmup's touch bar decided LongFirst
    // No ticks for the next input bar: advance the stream clock past its
    // boundary so stream_finalize_until synthesizes a zero-volume
    // carry-forward bar and dispatches it via stream_dispatch_script_bar --
    // a pairless bar (no strategy_entry calls, no fresh arbitration).
    if (!s.stream_advance_time(180'000)) return -97;
    const int result = s.last_bar_dual_entry_path();
    s.stream_end(false);
    return result;
}
}
int main() {
    CHECK(near(exit_price_under(kHighFirstTouchBar, 0), 101.0));  // AUTO: limit first
    CHECK(near(exit_price_under(kHighFirstTouchBar, 1), 101.0));  // HIGH_FIRST forced
    CHECK(near(exit_price_under(kHighFirstTouchBar, 2), 99.0));   // LOW_FIRST forced

    CHECK(near(exit_price_under(kLowFirstTouchBar, 0), 99.0));    // AUTO: stop first
    CHECK(near(exit_price_under(kLowFirstTouchBar, 1), 101.0));   // HIGH_FIRST forced flips it
    CHECK(near(exit_price_under(kLowFirstTouchBar, 2), 99.0));    // LOW_FIRST forced

    Bracket s;                                  // no dual entry pair -> None
    const std::vector<Bar> bars = {bar(100, 100, 100, 100, 0)};
    s.run(bars.data(), 1);
    CHECK(s.last_bar_dual_entry_path() == 0);

    CHECK(dual_entry_winner_probe(1) == 1);      // HIGH_FIRST -> LongFirst
    CHECK(dual_entry_winner_probe(2) == 2);      // LOW_FIRST -> ShortFirst
    CHECK(dual_entry_winner_after_pairless_bar() == 0);
    CHECK(dual_entry_winner_pooc_no_tail_suppression() == 1);
    CHECK(dual_entry_winner_after_empty_rerun() == 0);
    CHECK(dual_entry_winner_stream_after_pairless_bar() == 0);

    return failures == 0 ? 0 : 1;
}
