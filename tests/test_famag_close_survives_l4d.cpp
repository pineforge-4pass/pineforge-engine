// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

/*
 * test_famag_close_survives.cpp — a strategy.close co-queued with a default
 * all-in reversal SURVIVES a rule-5 whole drop (round 11 family AG; campaign
 * notes log-20260905t232805z-41661c90 and log-20260905t233556z-7f5ce2ed).
 *
 * The scrapper wrapper's reversal idiom is strategy.entry(opposite) THEN
 * strategy.close(current) on one signal bar. When TradingView's price-scale
 * margin check (engine.hpp rule 5: P = sig10(sig10(E_s) / Q) < tick(close_S))
 * drops the reversal, the reversal order is gone at placement and the
 * co-queued strategy.close still fills at the next open — the position goes
 * flat, no new position, the tape prints 'Close entry(s) order <id>'. Only the
 * fill-open gap reject (KI-54, Q x open > E_s) is atomic with its co-queued
 * close: TradingView holds the position and the close never fires (#91,
 * suppress_declined_reversal_close_legs). Rule 2 (E_s < sig10(cost)) keeps
 * the reversal's own close leg, whose row carries the reversal's id.
 *
 * Pinned by 17 lab tv tapes on OANDA:EURUSD 15 (scratch ~/pf-scratch/pins on
 * the campaign machine; strategy.equity / position_size in every order
 * comment, the solved E_s read back to 1e-9), replayed here on the registry
 * feed's bars 2025-05-19..05-27 (test_famag_close_survives_data.hpp) ROW FOR
 * ROW — entry bar, side, fill price, quantity, exit bar, exit price, 'Margin
 * call' vs close, net PnL:
 *   famag-B-* : explicit short 870000 @1.13523 (2025-05-21 10:45Z, in profit
 *               through T1 so no slice), at T1 a default 100 % long reversal
 *               and a strategy.close("Short"), initial_capital solved so that
 *               E_s = Q x tick(close_S) + delta;
 *               z (11:15Z, zero gap, tick one ulp above the double):
 *                 tie +0.0002  ef -> the close fills 'C1' @1.13384, no long
 *                              eo -> HOLD to the end (the bare whole drop)
 *                              cf -> 'C1' then Long 881957.65 + 1 'Margin call'
 *                 rule 2 -0.0001 ef/eo -> the reversal's close leg 'L1', flat
 *                 admitted +0.0007 ef -> 'L1' + Long 881958.65
 *               g (12:30Z, +2 pips): tie -> 'C1' fills; +0.0007 = the KI-54
 *                 gap reject -> HOLD; rule 2 -> 'L1' close leg fills
 *               d (13:00Z, -1 pip): tie -> 'C1' fills
 *   famag-A1-*: hossa-nostra's own bars (short 05-22 10:00Z, reversal at the
 *               05-23 07:45Z open, +2 pips) at C = 1e6: E_s - Q x close_S =
 *               +0.0036, no tie -> gap reject -> ef HOLDS (identical to eo),
 *               cf closes 'C1' and longs 878831.93 with a 123.88 sliver;
 *   famag-A3/A4: version-sk's bars and a no-slice window, same gap shapes;
 *   famag-A6-adm: a gap-down control, the reversal admitted ('L1').
 * Every tape replays row for row before and after the fix EXCEPT the three
 * rule-5 ties with an entry-then-close pair (B-z-tie-ef, B-g-tie-ef,
 * B-d-tie-ef), which the pre-fix engine held (the KI-54 suppression applied
 * at the rule-5 decline site) — RED 3, GREEN 17/17.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "test_famag_close_survives_data.hpp"

using namespace pineforge;
using namespace famag_close_survives_data;

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

namespace {

struct Row {
    int64_t entry_ts;
    bool is_long;
    double entry_price;
    double qty;
    int64_t exit_ts;
    double exit_price;
    int kind;   // 0 close, 1 margin call, 2 range end
    double pnl;
};

bool row_before(const Row& a, const Row& b) {
    if (a.entry_ts != b.entry_ts) return a.entry_ts < b.entry_ts;
    if (a.exit_ts != b.exit_ts) return a.exit_ts < b.exit_ts;
    return a.qty < b.qty;
}

std::vector<Bar> tape_bars() {
    std::vector<Bar> out;
    for (const BarRow& r : kBars) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

// The tapes' account: initial_capital as declared, percent_of_equity 100,
// commission 0, margin 100/100, OANDA:EURUSD (mintick 1e-5, lot 0.01),
// market fills at the next open, margin calls on. Same-bar actions are issued
// in the tape's script order (entry then close, or close then entry).
class TapeProbe : public pineforge::source::PineStrategyHost {
public:
    TapeProbe(double capital, const Action* actions, int n_actions)
        : actions_(actions), n_actions_(n_actions) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 1;
        slippage_ = 0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        qty_step_ = 0.01;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar& bar) override {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (int i = 0; i < n_actions_; ++i) {
            const Action& a = actions_[i];
            if (a.ts != bar.timestamp) continue;
            switch (a.kind) {
                case -2: strategy_entry("Short", false, nan, nan, a.qty); break;
                case -1: strategy_entry("Short", false); break;
                case 1: strategy_entry("Long", true); break;
                case 2: strategy_close("Short"); break;
                case 0: strategy_close_all(); break;
                default: break;
            }
        }
    }
    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (const Trade& t : trades_) {
            out.push_back({t.entry_time, t.is_long, t.entry_price, t.qty,
                           t.exit_time, t.exit_price,
                           t.exit_comment == "Margin call" ? 1 : 0, t.pnl});
        }
        for (const Trade& t : range_end_trades_) {
            out.push_back({t.entry_time, t.is_long, t.entry_price, t.qty,
                           t.exit_time, t.exit_price, 2, t.pnl});
        }
        std::sort(out.begin(), out.end(), row_before);
        return out;
    }
private:
    const Action* actions_;
    int n_actions_;
};

std::vector<Row> tape_rows(const Tape& t) {
    std::vector<Row> out;
    for (int i = 0; i < t.n_rows; ++i) {
        const TapeRow& r = t.rows[i];
        out.push_back({r.entry_ts, r.is_long, r.entry_price, r.qty, r.exit_ts,
                       r.exit_price, r.exit_kind, r.net_pnl});
    }
    std::sort(out.begin(), out.end(), row_before);
    return out;
}

void print_row(const char* tag, const Row& r) {
    std::printf("      %s %s entry %lld @ %.5f qty %.2f exit %lld @ %.5f kind %d pnl %.5f\n",
                tag, r.is_long ? "long " : "short", (long long)r.entry_ts,
                r.entry_price, r.qty, (long long)r.exit_ts, r.exit_price,
                r.kind, r.pnl);
}

int rows_mismatch(const std::vector<Row>& got, const std::vector<Row>& want,
                  bool verbose) {
    int mismatches = got.size() != want.size() ? 1 : 0;
    const size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i) {
        const Row& g = got[i];
        const Row& w = want[i];
        const bool same =
            g.entry_ts == w.entry_ts && g.is_long == w.is_long
            && std::fabs(g.entry_price - w.entry_price) <= 1e-6
            && std::fabs(g.qty - w.qty) <= 1e-6
            && g.exit_ts == w.exit_ts
            && std::fabs(g.exit_price - w.exit_price) <= 1e-6
            && g.kind == w.kind
            && std::fabs(g.pnl - w.pnl) <= 5e-3;
        if (!same && verbose) {
            std::printf("    row %zu differs\n", i);
            print_row("engine", g);
            print_row("tape  ", w);
        }
        mismatches += !same;
    }
    if (verbose && got.size() != want.size()) {
        std::printf("    engine %zu rows, tape %zu rows\n", got.size(), want.size());
        for (size_t i = n; i < got.size(); ++i) print_row("engine+", got[i]);
        for (size_t i = n; i < want.size(); ++i) print_row("tape+  ", want[i]);
    }
    return mismatches;
}

const Tape* find_tape(const char* name) {
    for (const Tape& t : kTapes) if (std::strcmp(t.name, name) == 0) return &t;
    return nullptr;
}

// The position after the T1 fill bar: >0 long, <0 short, 0 flat — read off the
// rows (a trade open across the bar after T1's fill).
int position_after(const std::vector<Bar>& bars, const Tape& t, int64_t fill_ts) {
    TapeProbe eng(t.capital, t.actions, t.n_actions);
    eng.run(bars.data(), bars.size());
    int64_t next = 0;
    for (size_t i = 0; i + 1 < bars.size(); ++i) {
        if (bars[i].timestamp == fill_ts) next = bars[i + 1].timestamp;
    }
    int pos = 0;
    for (const Row& r : eng.rows()) {
        if (r.entry_ts <= fill_ts && r.exit_ts >= next) pos = r.is_long ? 1 : -1;
    }
    return pos;
}

}  // namespace

static void test_tape_replays() {
    std::printf("-- famag-A*/famag-B* tapes: row-for-row replay on the 2025-05-19..05-27 bars --\n");
    const std::vector<Bar> bars = tape_bars();
    CHECK(bars.size() >= 700);
    int asserted = 0, ok = 0;
    std::vector<std::string> failed;
    for (const Tape& t : kTapes) {
        TapeProbe eng(t.capital, t.actions, t.n_actions);
        eng.run(bars.data(), bars.size());
        const std::vector<Row> got = eng.rows();
        const std::vector<Row> want = tape_rows(t);
        ++asserted;
        const int mm = rows_mismatch(got, want, /*verbose=*/true);
        if (mm == 0) ++ok; else failed.push_back(t.name);
        if (mm != 0) std::printf("   %s (%s): %d row mismatch(es)\n", t.name, t.expect, mm);
    }
    std::printf("   %d/%d tapes replay row for row\n", ok, asserted);
    for (const std::string& f : failed) std::printf("   FAILED tape: %s\n", f.c_str());
    CHECK(asserted == 17);
    CHECK(ok == asserted);
}

// The rule stated on the position after the reversal bar.
static void test_named_pins() {
    std::printf("-- named pins --\n");
    const std::vector<Bar> bars = tape_bars();
    const int64_t z_fill = 1747827900000LL;   // 2025-05-21 11:30Z open
    const int64_t g_fill = 1747832400000LL;   // 2025-05-21 12:45Z open
    const int64_t h_fill = 1747986300000LL;   // 2025-05-23 07:45Z open
    // Rule-5 tie + co-queued close: flat at the open (the close fills, no long).
    CHECK(position_after(bars, *find_tape("famag-B-z-tie-ef"), z_fill) == 0);
    CHECK(position_after(bars, *find_tape("famag-B-g-tie-ef"), g_fill) == 0);
    // The same tie without a close order: the position is held (bare whole drop).
    CHECK(position_after(bars, *find_tape("famag-B-z-tie-eo"), z_fill) == -1);
    // KI-54 gap reject (no tie, +2 pips): the co-queued close is suppressed, held.
    CHECK(position_after(bars, *find_tape("famag-B-g-gap-ef"), g_fill) == -1);
    CHECK(position_after(bars, *find_tape("famag-A1-ef"), h_fill) == -1);
    // Rule 2: the reversal's own close leg fills, flat either way.
    CHECK(position_after(bars, *find_tape("famag-B-z-r2-ef"), z_fill) == 0);
    CHECK(position_after(bars, *find_tape("famag-B-z-r2-eo"), z_fill) == 0);
    // Admitted: the reversal fills (long).
    CHECK(position_after(bars, *find_tape("famag-B-z-adm-ef"), z_fill) == 1);
    // Close placed BEFORE the entry: the close fills and the entry fills from flat.
    CHECK(position_after(bars, *find_tape("famag-B-z-tie-cf"), z_fill) == 1);
}

int main() {
    test_tape_replays();
    test_named_pins();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
