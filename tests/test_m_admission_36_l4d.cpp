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
 * test_m_admission_36.cpp — round 7 family M, mechanisms 3 and 6: the
 * short-side entry-price affordability trim and the stop-entry reversal
 * admission equity.
 *
 * Sources: campaign notes "round 7 family M mechanism 3/7" (market-logic-
 * india low-lag-strength-oscillator OANDA:XAUUSD@1D, moderate 100 count 2)
 * and "mechanism 6/7" (jaysharmaofficial alphamojo supertrend HA-with-buffer
 * BINANCE:BTCUSDT@1D, weak 75 count 1), 2026-09-05 11:16Z; the family-G pin
 * (reversal sizing marks at tick(close): "a positive fill-time shortfall
 * becomes the 1-lot entry-bar 'Margin call' trim at the entry price, PnL
 * 0"), the family-H pin (market-entry admission: the fee-only shortfall is
 * filled and trimmed at the fill), the family-E pin (stop-entry admission:
 * "a still-open opposite position costs nothing"; fill check at the level
 * against equity at fill) and the family-L rule (entry-bar margin call over
 * the post-fill OHLC path). The two probe tapes are replayed row-for-row on
 * the registry's own 1D bars (test_m_admission_36_data.hpp).
 *
 * Rule 3 (mechanism 3): a default-sized percent_of_equity 100 SHORT opened
 * by a MARKET order at 100% margin takes the same fill checkpoint as the
 * long side, with or without a commission — when its fill cost exceeds the
 * equity the fill left (the closing leg of a close-then-short realized at
 * the open), TradingView books the floor-before-4x trim (sub-lot shortfall:
 * ONE lot) AT THE FILL PRICE, PnL 0, tagged "Margin call", and only then
 * marks the survivor over the post-fill path. market-logic 2025-12-04
 * 06:00 (bar 12-03 22:00Z, open 4206.465): E_s = 8983.64 + 1.2 x (4203.115
 * - 4088.255) = 9121.47 -> Q = 2.17; cost 2.17 x 4206.465 = 9128.03 against
 * 9125.49 realized -> TV 22 "Margin call" 1.0 @4206.465 (PnL 0), TV 23 =
 * 1.17 carried to 12-11. The engine's short event was scoped to commissioned
 * shapes only, so the whole 2.17 rode into the ordinary cascade (0.04
 * @4219.62 + 0.04 @4259.34, EN 22-24) and every later quantity drifted with
 * the equity (EN 27/28 reversal 01-05 where TV drops it, EN 29-34 re-short
 * 01-08 vs TV 30 held to 03-09). The long side's trims (TV 7 09-22, TV 20
 * 11-20: 1.0 @ the entry price, PnL 0) are the control and stay as they are.
 *
 * Rule 6 (mechanism 6): the fill-time admission of a STOP entry that
 * REVERSES a position costs qty x tick(fill) against realized equity PLUS
 * the open position marked at the fill it closes at (the family-G sizing
 * equity) — the closing leg is free and is realized at this very fill.
 * jaysharma 2025-08-26 (bar 08-26 00:00Z, L 108666.66): the fixed 1 BTC
 * sell stop at haLow x 0.9995 = 109219.46 is admitted against 100000 +
 * 13972.86 (the 04-27 long 95246.60 closed at the level) = 113972.86 (TV 1
 * exit / TV 2-6 entry), then margin-called as BTC rises (0.05516 @112371 on
 * the entry bar's post-fill high, 0.043 @115488.09, 0.0212 @117900, 0.1198
 * @121022.07) and closed by the 10-02 flip's buy stop 121082.59 (TV 6) —
 * whose opening leg TV never fills: 1 x 120529.35 > 102905.5 + (109219.46
 * - 120529.35) x 0.76084 at the 10-02 close, the family-E placement check,
 * so only the closing leg rests (affordability_close_only). The engine's
 * realized-only basis (100000 < 109219.46) declined the 08-26 reversal and
 * held the long to 01-30 (3 trades vs 8). Flat fills are unchanged (no open
 * position); same-direction adds keep the realized-only basis (unpinned).
 *
 *   A. market-logic XAUUSD@1D: 33 TV rows row-for-row; the 12-04 trim and
 *      carry by name; the long trims (TV 7, TV 20) unchanged; the pinned
 *      01-04 dropped reversal (no fill 01-05, short held to 03-09).
 *   B. jaysharma BTCUSDT@1D: 8 TV rows row-for-row; the 08-26 reversal by
 *      name; the 10-03 close-only leg (no long opened); the range-end row.
 *   C. Controls (synthetic, mintick 0.01, 1-share lots):
 *      C1 a stop reversal whose cost exceeds realized + open PnL at the
 *         fill is still DECLINED although realized alone would admit it;
 *      C2 the mirror (jaysharma in miniature): realized alone declines,
 *         realized + the closing leg's profit admits;
 *      C3 a FLAT stop entry is byte-identical (admitted at cost == equity,
 *         declined one cent over);
 *      C4 a zero-commission TRUE-FLAT default short: the gap-reject drops
 *         an over-equity fill and an exact-cost fill carries no PnL-0 trim
 *         (the ordinary cascade only).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "test_m_admission_36_data.hpp"

using namespace pineforge;
using namespace m36_data;

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

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr int kExitClose = 0;
constexpr int kExitMarginCall = 1;
constexpr int kExitOpenAtEnd = 2;

struct Row {
    int64_t entry_ts;
    double entry_price;
    double qty;
    int64_t exit_ts;
    double exit_price;
    int kind;
    double pnl;
    bool is_long;
};

bool row_before(const Row& a, const Row& b) {
    if (a.entry_ts != b.entry_ts) return a.entry_ts < b.entry_ts;
    if (a.exit_ts != b.exit_ts) return a.exit_ts < b.exit_ts;
    return a.qty < b.qty;
}

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

template <size_t N>
std::vector<Row> to_rows(const TapeRow (&rows)[N]) {
    std::vector<Row> out;
    out.reserve(N);
    for (const TapeRow& r : rows) {
        out.push_back({r.entry_ts, r.entry_price, r.qty, r.exit_ts,
                       r.exit_price, r.exit_kind, r.net_pnl, false});
    }
    std::sort(out.begin(), out.end(), row_before);
    return out;
}

struct Ohlc {
    double open, high, low, close;
};

// Synthetic daily bars at 1-day spacing from an arbitrary epoch.
std::vector<Bar> synth_bars(const std::vector<Ohlc>& rows) {
    std::vector<Bar> out;
    const int64_t t0 = 1735689600000LL;  // 2025-01-01 00:00Z
    for (size_t i = 0; i < rows.size(); ++i) {
        Bar b;
        b.timestamp = t0 + (int64_t)i * 86400000LL;
        b.open = rows[i].open; b.high = rows[i].high;
        b.low = rows[i].low; b.close = rows[i].close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

// The probes' broker: zero commission, 1x margin both sides, margin calls
// on, market fills at the next open, pyramiding 0 (both scripts). Sizing is
// the script's default: percent_of_equity 100 (market-logic) or the fixed
// 1-contract default (jaysharma, whose strategy() sets neither).
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double mintick, double lot, QtyType qty_type,
          double qty_value) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        qty_step_ = lot;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    std::function<void(Probe&, const Bar&)> script;
    void on_source_bar(const Bar& bar) override {
        if (script) script(*this, bar);
    }
    void entry_market(const std::string& id, bool is_long, double qty = kNaN) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void entry_stop(const std::string& id, bool is_long, double level,
                    double qty = kNaN) {
        strategy_entry(id, is_long, kNaN, level, qty, "");
    }
    using pineforge::source::PineStrategyHost::strategy_close;

    // Every closed trade plus TV's range-end row for a position still open
    // after the last bar, in the tape's sort order.
    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (const Trade& t : trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price,
                           t.exit_comment == "Margin call" ? kExitMarginCall
                                                           : kExitClose,
                           t.pnl, t.is_long});
        }
        for (const Trade& t : range_end_trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price, kExitOpenAtEnd, t.pnl, t.is_long});
        }
        std::sort(out.begin(), out.end(), row_before);
        return out;
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool is_short() const { return position_side_ == PositionSide::SHORT; }
    bool is_long_pos() const { return position_side_ == PositionSide::LONG; }
    double pos_qty() const { return position_qty_; }
    double pos_entry() const { return position_entry_price_; }
    int closed_count() const { return (int)trades_.size(); }
    int open_at_end_count() const { return (int)range_end_trades_.size(); }
};

void print_row(const char* tag, const Row& r) {
    std::printf("      %s entry %lld @ %.5f qty %.5f exit %lld @ %.5f kind %d pnl %.5f\n",
                tag, (long long)r.entry_ts, r.entry_price, r.qty,
                (long long)r.exit_ts, r.exit_price, r.kind, r.pnl);
}

// Row-for-row comparison of an engine replay against a TV tape.
void check_rows_match(const char* name, const std::vector<Row>& got,
                      const std::vector<Row>& want) {
    std::printf("   %s: engine %zu rows, tape %zu rows\n", name, got.size(),
                want.size());
    CHECK(got.size() == want.size());
    const size_t n = std::min(got.size(), want.size());
    int mismatches = 0;
    for (size_t i = 0; i < n; ++i) {
        const Row& g = got[i];
        const Row& w = want[i];
        const bool same =
            g.entry_ts == w.entry_ts
            && std::fabs(g.entry_price - w.entry_price) <= 1e-6
            && std::fabs(g.qty - w.qty) <= 1e-6
            && g.exit_ts == w.exit_ts
            && std::fabs(g.exit_price - w.exit_price) <= 1e-6
            && g.kind == w.kind
            && std::fabs(g.pnl - w.pnl) <= 5e-3;
        if (!same && mismatches < 12) {
            std::printf("    row %zu differs\n", i);
            print_row("engine", g);
            print_row("tape  ", w);
        }
        mismatches += !same;
    }
    if (got.size() != want.size()) {
        for (size_t i = n; i < got.size(); ++i) print_row("extra engine", got[i]);
        for (size_t i = n; i < want.size(); ++i) print_row("missing tape", want[i]);
    }
    CHECK(mismatches == 0);
}

std::vector<Row> rows_entered_at(const std::vector<Row>& rows, int64_t ts) {
    std::vector<Row> out;
    for (const Row& r : rows) {
        if (r.entry_ts == ts) out.push_back(r);
    }
    return out;
}

// ---------------------------------------------------------------------------
// A. market-logic-india low-lag-strength-oscillator @ OANDA:XAUUSD 1D.
//    strategy(initial_capital=10000, percent_of_equity 100), no commission:
//      if longSig:  strategy.entry("Long", long)
//      if shortSig: strategy.close("Long"); strategy.entry("Short", short)
//      if longSig:  strategy.close("Short")
//    Signal bars from the range-start model; fills at the next bar's open.
// ---------------------------------------------------------------------------
void test_market_logic_tape() {
    std::printf("A. market-logic XAUUSD@1D tape replay (short-side entry-price trim)\n");
    const std::vector<Bar> bars = to_bars(kXauDaily);
    std::map<int64_t, std::pair<bool, bool>> signals;
    for (const Signal& s : kMarketLogicSignals) {
        signals[s.ts] = {s.long_sig, s.short_sig};
    }
    Probe p(10000.0, 0.001, 0.01, QtyType::PERCENT_OF_EQUITY, 100.0);
    p.script = [&](Probe& e, const Bar& bar) {
        auto it = signals.find(bar.timestamp);
        if (it == signals.end()) return;
        const bool long_sig = it->second.first;
        const bool short_sig = it->second.second;
        if (long_sig) e.entry_market("Long", true);
        if (short_sig) {
            e.strategy_close("Long");
            e.entry_market("Short", false);
        }
        if (long_sig) e.strategy_close("Short");
    };
    p.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = p.rows();
    const std::vector<Row> want = to_rows(kMarketLogicTape);
    check_rows_match("market-logic XAUUSD@1D", got, want);

    // The mechanism-3 rows by name: 2025-12-04 06:00 (bar 12-03 22:00Z).
    const int64_t t1204 = 1764799200000LL;
    const std::vector<Row> dec04 = rows_entered_at(got, t1204);
    CHECK(dec04.size() == 2);
    if (dec04.size() == 2) {
        // TV 22: the 1-lot entry-price trim, "Margin call", PnL 0, duration 0.
        CHECK(!dec04[0].is_long);
        CHECK_NEAR(dec04[0].entry_price, 4206.465, 1e-9);
        CHECK_NEAR(dec04[0].qty, 1.0, 1e-9);
        CHECK(dec04[0].exit_ts == t1204);
        CHECK_NEAR(dec04[0].exit_price, 4206.465, 1e-9);
        CHECK(dec04[0].kind == kExitMarginCall);
        CHECK_NEAR(dec04[0].pnl, 0.0, 1e-9);
        // TV 23: the 1.17 survivor carried to the 12-11 06:00 long (bar
        // 12-10 22:00Z open 4228.245) — no slice at the entry bar's high
        // 4219.62 nor at the next day's 4259.34.
        CHECK_NEAR(dec04[1].qty, 1.17, 1e-9);
        CHECK(dec04[1].exit_ts == 1765404000000LL);
        CHECK_NEAR(dec04[1].exit_price, 4228.245, 1e-9);
        CHECK(dec04[1].kind == kExitClose);
        CHECK_NEAR(dec04[1].pnl, -25.4826, 5e-3);
    }
    int slices_at_high = 0;
    for (const Row& r : got) {
        if (r.entry_ts == t1204 && r.kind == kExitMarginCall
            && std::fabs(r.exit_price - r.entry_price) > 1e-9) {
            ++slices_at_high;
        }
    }
    CHECK(slices_at_high == 0);

    // Control: the LONG side's entry-price trims are unchanged — TV 7
    // (2025-09-22 06:00, bar 09-21 21:00Z) and TV 20 (2025-11-20 06:00, bar
    // 11-19 22:00Z): 1.0 @ the entry price, "Margin call", PnL 0.
    for (int64_t ts : {1758488400000LL, 1763589600000LL}) {
        const std::vector<Row> rows = rows_entered_at(got, ts);
        CHECK(rows.size() == 2);
        if (rows.size() == 2) {
            CHECK(rows[0].is_long);
            CHECK_NEAR(rows[0].qty, 1.0, 1e-9);
            CHECK(rows[0].exit_ts == ts);
            CHECK_NEAR(rows[0].exit_price, rows[0].entry_price, 1e-9);
            CHECK(rows[0].kind == kExitMarginCall);
            CHECK_NEAR(rows[0].pnl, 0.0, 1e-9);
        }
    }

    // The pinned dropped reversal: the 2026-01-04 22:00Z bar fires longSig
    // AND shortSig; TV's Q = 2.05 x open 4454.8 = 9132 > E_s 9121.9, the
    // reversal is dropped and the same-bar close("Short") voided — nothing
    // fills 2026-01-05 22:00Z and TV 30 (1.15 short from 12-30) rides to
    // 03-09 05:00. The engine printed EN 27/28 here with its drifted equity.
    CHECK(rows_entered_at(got, 1767650400000LL).empty());
    CHECK(rows_entered_at(got, 1767909600000LL).empty());  // 2026-01-08 22:00Z
}

// ---------------------------------------------------------------------------
// B. jaysharmaofficial alphamojo supertrend HA-with-buffer @ BINANCE:BTCUSDT
//    1D. strategy(initial_capital=100000, pyramiding=0), fixed 1-contract
//    default, no commission:
//      if ta.change(haDirection) < 0: strategy.entry("My Long Entry Id",
//          long, stop = haHigh * (1 + 0.0005))
//      if ta.change(haDirection) > 0: strategy.entry("My Short Entry Id",
//          short, stop = haLow * (1 - 0.0005))
// ---------------------------------------------------------------------------
void test_jaysharma_tape() {
    std::printf("B. jaysharma BTCUSDT@1D tape replay (stop-entry reversal admission)\n");
    const std::vector<Bar> bars = to_bars(kBtcDaily);
    std::map<int64_t, std::pair<bool, double>> flips;
    for (const Flip& f : kJayFlips) flips[f.ts] = {f.is_long, f.ha_extreme};
    Probe p(100000.0, 0.01, 0.00001, QtyType::FIXED, 1.0);
    p.script = [&](Probe& e, const Bar& bar) {
        auto it = flips.find(bar.timestamp);
        if (it == flips.end()) return;
        const bool is_long = it->second.first;
        const double extreme = it->second.second;
        const double buffer = 0.05 / 100.0;
        if (is_long) {
            e.entry_stop("My Long Entry Id", true, extreme + extreme * buffer);
        } else {
            e.entry_stop("My Short Entry Id", false, extreme - extreme * buffer);
        }
    };
    p.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = p.rows();
    const std::vector<Row> want = to_rows(kJaySharmaTape);
    check_rows_match("jaysharma BTCUSDT@1D", got, want);

    // TV 1 / TV 2-6: the 08-26 sell stop REVERSES the 04-27 long at the
    // level — admitted against 100000 + 13972.86, not 100000.
    const int64_t t0826 = 1756166400000LL;
    const std::vector<Row> apr27 = rows_entered_at(got, 1745712000000LL);
    CHECK(apr27.size() == 1);
    if (apr27.size() == 1) {
        CHECK(apr27[0].is_long);
        CHECK_NEAR(apr27[0].entry_price, 95246.6, 1e-6);
        CHECK(apr27[0].exit_ts == t0826);
        CHECK_NEAR(apr27[0].exit_price, 109219.46, 1e-6);
        CHECK(apr27[0].kind == kExitClose);
        CHECK_NEAR(apr27[0].pnl, 13972.86, 5e-3);
    }
    const std::vector<Row> aug26 = rows_entered_at(got, t0826);
    CHECK(aug26.size() == 5);
    if (aug26.size() == 5) {
        // The entry bar's post-fill high (family L): 0.05516 @112371.
        CHECK(!aug26[0].is_long);
        CHECK(aug26[0].exit_ts == t0826);
        CHECK_NEAR(aug26[0].exit_price, 112371.0, 1e-6);
        CHECK_NEAR(aug26[0].qty, 0.05516, 1e-9);
        CHECK(aug26[0].kind == kExitMarginCall);
        // TV 6: the remainder closed by the 10-02 flip's buy stop at
        // 121082.59 (ceil-snapped 121022.07 x 1.0005) on 10-03.
        CHECK(aug26[4].exit_ts == 1759449600000LL);
        CHECK_NEAR(aug26[4].exit_price, 121082.59, 1e-6);
        CHECK_NEAR(aug26[4].qty, 0.76084, 1e-9);
        CHECK(aug26[4].kind == kExitClose);
    }
    // The 10-03 buy stop's OPENING leg never fills: rejected at placement on
    // the 10-02 close (1 x 120529.35 > equity 102905.5 - 8605), it rests
    // close-only. TV's next row is the 01-30 short from flat.
    CHECK(rows_entered_at(got, 1759449600000LL).empty());
    // TV 7 / TV 8: the 01-30 short from flat and the 04-22 long reversal
    // (admitted: 78372.17 <= 102905.5 + 4969.46), open at the range end.
    const std::vector<Row> jan30 = rows_entered_at(got, 1769731200000LL);
    CHECK(jan30.size() == 1);
    if (jan30.size() == 1) {
        CHECK(!jan30[0].is_long);
        CHECK_NEAR(jan30[0].entry_price, 83341.63, 1e-6);
        CHECK(jan30[0].exit_ts == 1776816000000LL);
        CHECK_NEAR(jan30[0].exit_price, 78372.17, 1e-6);
    }
    const std::vector<Row> apr22 = rows_entered_at(got, 1776816000000LL);
    CHECK(apr22.size() == 1);
    if (apr22.size() == 1) {
        CHECK(apr22[0].is_long);
        CHECK(apr22[0].kind == kExitOpenAtEnd);
        CHECK_NEAR(apr22[0].exit_price, 78231.13, 1e-6);
    }
    CHECK(p.is_long_pos());
    CHECK_NEAR(p.pos_qty(), 1.0, 1e-9);
}

// ---------------------------------------------------------------------------
// C. Controls.
// ---------------------------------------------------------------------------

// C1. A stop reversal whose cost exceeds realized + the open position's PnL
//     at the fill is DECLINED, although realized alone would admit it:
//     capital 220, short 1 @100; buy stop 105 x 2 placed at the 100 close
//     (placement 2 x 100 = 200 <= 220) gaps through to 108: cost 216 <=
//     220 realized, but 220 + (100 - 108) = 212 < 216 -> declined, the
//     short is held.
void test_control_reversal_exceeding_marked_equity_declines() {
    std::printf("C1. stop reversal over realized + open PnL at the fill declines\n");
    const std::vector<Bar> bars = synth_bars({
        {100.0, 101.0, 99.0, 100.0},
        {100.0, 101.0, 99.0, 100.0},
        {108.0, 109.0, 107.0, 108.0},
        {108.0, 109.0, 107.0, 108.0},
    });
    Probe p(220.0, 0.01, 1.0, QtyType::FIXED, 1.0);
    p.script = [](Probe& e, const Bar& bar) {
        if (bar.timestamp == 1735689600000LL) e.entry_market("S", false, 1.0);
        if (bar.timestamp == 1735689600000LL + 86400000LL) {
            e.entry_stop("L", true, 105.0, 2.0);
        }
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.closed_count() == 0);
    CHECK(p.is_short());
    CHECK_NEAR(p.pos_qty(), 1.0, 1e-9);
    CHECK_NEAR(p.pos_entry(), 100.0, 1e-9);
    CHECK(p.open_at_end_count() == 1);
}

// C2. The mirror (jaysharma in miniature): capital 100, long 1 @100; sell
//     stop 108 x 1 placed at the 110 close (placement 1 x 110 <= 100 + 10)
//     touched at 108: cost 108 > 100 realized (the old basis declined it)
//     but <= 100 + (108 - 100) = 108 -> admitted: the long closes @108
//     (+8) and the short opens @108.
void test_control_reversal_admitted_on_marked_equity() {
    std::printf("C2. stop reversal admitted against realized + the closing leg's profit\n");
    const std::vector<Bar> bars = synth_bars({
        {100.0, 101.0, 99.0, 100.0},
        {100.0, 101.0, 99.0, 100.0},
        {100.0, 112.0, 99.0, 110.0},
        {110.0, 111.0, 107.0, 108.0},
    });
    Probe p(100.0, 0.01, 1.0, QtyType::FIXED, 1.0);
    p.script = [](Probe& e, const Bar& bar) {
        if (bar.timestamp == 1735689600000LL) e.entry_market("L", true, 1.0);
        if (bar.timestamp == 1735689600000LL + 2 * 86400000LL) {
            e.entry_stop("S", false, 108.0, 1.0);
        }
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.closed_count() == 1);
    if (p.closed_count() == 1) {
        const Row r = p.rows()[0];
        CHECK(r.is_long);
        CHECK_NEAR(r.entry_price, 100.0, 1e-9);
        CHECK(r.exit_ts == 1735689600000LL + 3 * 86400000LL);
        CHECK_NEAR(r.exit_price, 108.0, 1e-9);
        CHECK(r.kind == kExitClose);
        CHECK_NEAR(r.pnl, 8.0, 1e-9);
    }
    CHECK(p.is_short());
    CHECK_NEAR(p.pos_qty(), 1.0, 1e-9);
    CHECK_NEAR(p.pos_entry(), 108.0, 1e-9);
}

// C3. A FLAT stop entry is unchanged: buy stop 105 x 2 placed at the 100
//     close, gapped through to 108 (cost 216): admitted at capital 216,
//     declined at 215 (family E fresh-gap-once: dropped, no partial).
void test_control_flat_stop_unchanged() {
    std::printf("C3. flat stop admission unchanged (216 admits, 215 declines)\n");
    const std::vector<Bar> bars = synth_bars({
        {100.0, 101.0, 99.0, 100.0},
        {108.0, 109.0, 107.0, 108.0},
        {108.0, 109.0, 107.0, 108.0},
    });
    for (double capital : {216.0, 215.0}) {
        Probe p(capital, 0.01, 1.0, QtyType::FIXED, 1.0);
        p.script = [](Probe& e, const Bar& bar) {
            if (bar.timestamp == 1735689600000LL) e.entry_stop("L", true, 105.0, 2.0);
        };
        p.run(bars.data(), (int)bars.size());
        if (capital == 216.0) {
            CHECK(p.is_long_pos());
            CHECK_NEAR(p.pos_qty(), 2.0, 1e-9);
            CHECK_NEAR(p.pos_entry(), 108.0, 1e-9);
        } else {
            CHECK(p.flat());
            CHECK(p.closed_count() == 0);
            CHECK(p.open_at_end_count() == 0);
        }
    }
}

// C4. A zero-commission TRUE-FLAT default short (percent_of_equity 100):
//     the family-H gap-reject still drops an over-equity fill outright (no
//     trim, no fill), and an exact-cost fill takes no PnL-0 entry-price
//     trim — its only broker action is the ordinary post-fill cascade at
//     the bar's high.
void test_control_true_flat_default_short_unchanged() {
    std::printf("C4. zero-commission true-flat default short: gap-reject / no fill-price trim\n");
    // (a) gap up: 1000 x 10.05 = 10050 > 10000 -> dropped.
    {
        const std::vector<Bar> bars = synth_bars({
            {10.0, 10.1, 9.9, 10.0},
            {10.05, 10.1, 9.95, 10.0},
            {10.0, 10.05, 9.95, 10.0},
        });
        Probe p(10000.0, 0.01, 1.0, QtyType::PERCENT_OF_EQUITY, 100.0);
        p.script = [](Probe& e, const Bar& bar) {
            if (bar.timestamp == 1735689600000LL) e.entry_market("S", false);
        };
        p.run(bars.data(), (int)bars.size());
        CHECK(p.flat());
        CHECK(p.closed_count() == 0);
        CHECK(p.open_at_end_count() == 0);
    }
    // (b) exact cost: 1000 x 10.00 = 10000 <= 10000 -> fills; the entry
    //     bar's high 10.1 then slices the ordinary cascade (equity 9900 vs
    //     required 10100: q_min 19.8 -> 19 -> 76 @10.1), never a PnL-0 row
    //     at the 10.0 fill.
    {
        const std::vector<Bar> bars = synth_bars({
            {10.0, 10.1, 9.9, 10.0},
            {10.0, 10.1, 9.95, 10.0},
            {10.0, 10.05, 9.95, 10.0},
        });
        Probe p(10000.0, 0.01, 1.0, QtyType::PERCENT_OF_EQUITY, 100.0);
        p.script = [](Probe& e, const Bar& bar) {
            if (bar.timestamp == 1735689600000LL) e.entry_market("S", false);
        };
        p.run(bars.data(), (int)bars.size());
        CHECK(p.is_short());
        int fill_price_trims = 0;
        int cascade_rows = 0;
        for (const Row& r : p.rows()) {
            if (r.kind != kExitMarginCall) continue;
            if (std::fabs(r.exit_price - 10.0) <= 1e-9) ++fill_price_trims;
            if (std::fabs(r.exit_price - 10.1) <= 1e-9) ++cascade_rows;
        }
        CHECK(fill_price_trims == 0);
        CHECK(cascade_rows == 1);
        CHECK_NEAR(p.pos_qty(), 1000.0 - 76.0, 1e-9);
    }
}

}  // namespace

int main() {
    test_market_logic_tape();
    test_jaysharma_tape();
    test_control_reversal_exceeding_marked_equity_declines();
    test_control_reversal_admitted_on_marked_equity();
    test_control_flat_stop_unchanged();
    test_control_true_flat_default_short_unchanged();
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
