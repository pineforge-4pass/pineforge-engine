/*
 * test_m_admission_36.cpp -- market fill-price trim and stop reversal equity.
 *
 * Pinned against TradingView behaviour (recorded restricted-market tapes
 * are kept outside this repository). The market cases use invented bars:
 * a close-then-short whose frozen size slightly exceeds fill-time equity
 * loses one unit at the fill, PnL zero, before marking its survivor. Two
 * long controls require the same fill-price trim. A simultaneous pair of
 * signals whose reversal gaps over equity must preserve the existing short.
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
 * The BTC replay remains unchanged. Synthetic controls C1-C4 pin marked
 * reversal equity, flat stop admission and true-flat short gap rejection.
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
std::vector<Bar> synth_bars(const std::vector<Ohlc>& rows,
                            int64_t t0 = 1735689600000LL) {
    std::vector<Bar> out;
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
// on, market fills at the next open, pyramiding 0. Synthetic market pairs
// use percent_of_equity 100; the BTC replay uses a fixed one-contract default.
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
// A. Invented market close-then-open paths at the sub-lot equity boundary.
// ---------------------------------------------------------------------------
void test_market_logic_tape() {
    std::printf("A. synthetic market fill-price trims and dropped reversal\n");
    constexpr int64_t epoch = 946684800000LL;
    constexpr int64_t day = 86400000LL;
    const auto stamp = [=](int index) { return epoch + index * day; };
    std::vector<Row> got;
    std::vector<Row> want;

    const auto run_pair = [&](int offset, bool first_long, double capital,
                              double seed, double signal, double fill,
                              double exit, double survivor) {
        const auto bars = synth_bars({
            {seed, seed, seed, seed},
            {seed, seed, seed, seed},
            {signal, signal, signal, signal},
            {fill, fill + 0.7, fill - 0.3, fill},
            {fill, fill, fill, fill},
            {exit, exit, exit, exit},
        }, stamp(offset));
        Probe p(capital, 0.001, 0.01, QtyType::PERCENT_OF_EQUITY, 100.0);
        p.script = [=](Probe& e, const Bar& bar) {
            if (bar.timestamp == stamp(offset)) {
                e.entry_market(first_long ? "Long" : "Short", first_long, 1.0);
            }
            if (bar.timestamp == stamp(offset + 2)) {
                // Retain the two command orders: close-then-short and
                // reverse-to-long-then-close-short.
                if (first_long) {
                    e.strategy_close("Long");
                    e.entry_market("Short", false);
                } else {
                    e.entry_market("Long", true);
                    e.strategy_close("Short");
                }
            }
            if (bar.timestamp == stamp(offset + 4)) {
                e.strategy_close(first_long ? "Short" : "Long");
            }
        };
        p.run(bars.data(), (int)bars.size());
        const auto rows = p.rows();
        got.insert(got.end(), rows.begin(), rows.end());
        // Initial leg closes at the new entry's fill; trim has zero duration
        // and PnL; remaining leg earns signed (exit - fill) * survivor.
        want.push_back({stamp(offset + 1), seed, 1.0, stamp(offset + 3),
                        fill, kExitClose, first_long ? fill - seed : seed - fill,
                        first_long});
        want.push_back({stamp(offset + 3), fill, 1.0, stamp(offset + 3),
                        fill, kExitMarginCall, 0.0, !first_long});
        want.push_back({stamp(offset + 3), fill, survivor, stamp(offset + 5),
                        exit, kExitClose,
                        first_long ? (fill - exit) * survivor
                                   : (exit - fill) * survivor,
                        !first_long});
        CHECK(p.flat());
    };

    // Short: signal equity 250+(110-100)=260, hence
    // floor(260/110/0.01)*0.01=2.36. Fill equity 260.3, cost
    // 2.36*110.3=260.308: q_restore=0.008/110.3 < 0.01 -> one unit.
    // Survivor 2.36-1=1.36; close PnL=(110.3-112)*1.36=-2.312.
    run_pair(0, true, 250.0, 100.0, 110.0, 110.3, 112.0, 1.36);
    // Long control 1: signal equity 250+(100-90)=260 -> 2.88 units.
    // Fill equity 259.6, cost 2.88*90.4=260.352; deficit/90.4 =
    // 0.008318... < 0.01 -> one unit; (92-90.4)*1.88=3.008 PnL.
    run_pair(10, false, 250.0, 100.0, 90.0, 90.4, 92.0, 1.88);
    // Long control 2: signal equity 300+(120-100)=320 -> 3.20 units.
    // Fill equity 319.8, cost 3.20*100.2=320.64; deficit/100.2 =
    // 0.008383... < 0.01 -> one unit; (101.5-100.2)*2.20=2.86 PnL.
    run_pair(20, false, 300.0, 120.0, 100.0, 100.2, 101.5, 2.20);

    // Dropped reversal: signal equity 250 freezes 2.5 units at 100.
    // At the next open 102, cost 255 exceeds marked equity
    // 250+(100-102)*2=246. Closing the old short must be voided too.
    // A later empty bar confirms no deferred entry; the two-unit short
    // remains until range end at 101, PnL=(100-101)*2=-2.
    const auto bars = synth_bars({
        {100.0, 100.0, 100.0, 100.0},
        {100.0, 100.0, 100.0, 100.0},
        {100.0, 100.0, 100.0, 100.0},
        {102.0, 103.0, 101.0, 102.0},
        {102.0, 103.0, 101.0, 102.0},
        {101.0, 102.0, 100.0, 101.0},
        {101.0, 102.0, 100.0, 101.0},
        {101.0, 102.0, 100.0, 101.0},
    }, stamp(30));
    Probe p(250.0, 0.001, 0.01, QtyType::PERCENT_OF_EQUITY, 100.0);
    p.script = [&](Probe& e, const Bar& bar) {
        if (bar.timestamp == stamp(30)) e.entry_market("Short", false, 2.0);
        if (bar.timestamp == stamp(32)) {
            e.entry_market("Long", true);
            e.strategy_close("Long");
            e.entry_market("Short", false);
            e.strategy_close("Short");
        }
    };
    p.run(bars.data(), (int)bars.size());
    const auto held = p.rows();
    got.insert(got.end(), held.begin(), held.end());
    want.push_back({stamp(31), 100.0, 2.0, stamp(37), 101.0,
                    kExitOpenAtEnd, -2.0, false});
    CHECK(p.is_short());
    CHECK_NEAR(p.pos_qty(), 2.0, 1e-9);
    CHECK(p.closed_count() == 0);
    std::sort(got.begin(), got.end(), row_before);
    std::sort(want.begin(), want.end(), row_before);
    check_rows_match("synthetic market admission", got, want);

    const int64_t short_fill = stamp(3);
    const std::vector<Row> short_rows = rows_entered_at(got, short_fill);
    CHECK(short_rows.size() == 2);
    if (short_rows.size() == 2) {
        CHECK(!short_rows[0].is_long);
        CHECK_NEAR(short_rows[0].entry_price, 110.3, 1e-9);
        CHECK_NEAR(short_rows[0].qty, 1.0, 1e-9);
        CHECK(short_rows[0].exit_ts == short_fill);
        CHECK_NEAR(short_rows[0].exit_price, 110.3, 1e-9);
        CHECK(short_rows[0].kind == kExitMarginCall);
        CHECK_NEAR(short_rows[0].pnl, 0.0, 1e-9);
        CHECK_NEAR(short_rows[1].qty, 1.36, 1e-9);
        CHECK(short_rows[1].exit_ts == stamp(5));
        CHECK_NEAR(short_rows[1].exit_price, 112.0, 1e-9);
        CHECK(short_rows[1].kind == kExitClose);
        CHECK_NEAR(short_rows[1].pnl, -2.312, 5e-3);  // (110.3-112)*1.36
    }
    int slices_at_high = 0;
    for (const Row& r : got) {
        if (r.entry_ts == short_fill && r.kind == kExitMarginCall
            && std::fabs(r.exit_price - r.entry_price) > 1e-9) {
            ++slices_at_high;
        }
    }
    CHECK(slices_at_high == 0);

    for (int64_t ts : {stamp(13), stamp(23)}) {
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
    CHECK(rows_entered_at(got, stamp(33)).empty());
    CHECK(rows_entered_at(got, stamp(36)).empty());
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
