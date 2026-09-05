/*
 * test_dropped_reversal_mc_first.cpp — round 7 family M, mechanism 2a: on a
 * bar whose OPEN carries a MARKET reversal that admission DROPS (its same-bar
 * strategy.close voided), the position's standing stop bracket is dormant for
 * the bar (finding-311), TradingView evaluates the margin call at the bar's
 * adverse EXTREME first, and the revived bracket then closes the remainder AT
 * THAT EXTREME on the same bar — not at its level, not on a later bar.
 *
 * Sources: campaign pin "PINNED (dropped-reversal bar: margin call first,
 * stop fills at the extreme; round 7 family M rhyme17)" 2026-09-05 11:16Z
 * (log-20260905t111645z-e1783b94) and the "round 7 family M mechanism 2/7"
 * note; lab tv tapes scratchpad/r7/pins/m1d-mcbar-stop-{rev,norev}
 * (OANDA:XAUUSD 1D 2025-06-01..08-01, ws-report-v1, rangeProof covered;
 * tv_trades.csv sha256 ed4c62d6... / 3c901d1b...); the rhyme17
 * trendline-and-horizontal-breakout XAUUSD@1D probe rows TV 3/4; the family-N
 * mechanism-2 pin (aapl15-mcopen1-stop-algoai) as the OPEN-slice control.
 *
 * The pinned bar: 2025-07-13 21:00Z, O 3362.375 H 3375.085 L 3341.09
 * C 3343.755 (bearish, high first), a 2.92-lot short @3322.825 (100% of
 * 10,000 at the 06-23 21:00Z close, filled at the 06-24 open, trimmed 0.04
 * @3337.205 on its entry bar and 0.04 @3358.085 on 06-30), its stop resting
 * at avg + 47.5 = 3370.325 — BETWEEN the open and the high.
 *
 *   norev (no reversal issued): the stop precedes the extreme on the path,
 *         so it fills AT ITS LEVEL 3370.325 x2.92 and there is no margin
 *         call (finding-308 chronology: exit first, tie or earlier).
 *   rev   (strategy.entry Long + strategy.close("Short") at the 07-10 21:00Z
 *         close, dropped at the 07-13 open: E_s 9902.1, Q 2.95 x 3362.375 =
 *         9919 > E_s; the close voided): "Margin call" 1.0 @3375.085 THEN
 *         "Short Exit" 1.92 @3375.085 — both at the HIGH, on the same bar.
 *
 * The engine (8d9ec8d) sliced 1.0 @3375.085 but never filled the stop that
 * bar; the re-issued stop filled two bars later 07-15 21:00Z @3370.537
 * (-91.61 vs TV -100.34), the seed of rhyme17's equity knock-ons.
 *
 *   A. rev tape row-for-row (4 rows) and the 07-14 pair by name.
 *   B. norev tape row-for-row (3 rows): the stop at its level, no slice.
 *   C. The rhyme17 probe shape — the bracket carries a limit AND a stop and
 *      is re-issued every bar (frozen whole-position qty): the same 07-14
 *      pair as A.
 *   D. Family-N mechanism 2 control (aapl15-mcopen1-stop-algoai): the
 *      declined reversal on an OPEN-slice bar leaves the bracket live and it
 *      fills AT ITS LEVEL — 1 @271.96 then 'X' 2814 @273.69 — unchanged. The
 *      pinned difference between N and M-2a is WHERE the slice comes: at the
 *      open (the bracket revives at the open, rests, fills at its level) or
 *      at the extreme (it revives there, already marketable, fills there).
 *   E. Synthetic controls (mintick 0.01, 1-share lots):
 *      E1 a declined reversal with NO deficit at the extreme: the dormant
 *         stop does not fill on that bar (finding-311) and the fresh
 *         re-issue fills at its level on the next touch;
 *      E2 a declined reversal on a LOW-first bar of a short (the extreme
 *         comes AFTER the stop's level on the path): the slice and the
 *         bracket fill still both book at the extreme — the dormant bracket
 *         cannot fill before the revive;
 *      E3 the same with the bracket issued ONCE (no re-issue): the pair's
 *         close held it dormant at placement rather than cancelling it, so
 *         the cascade's revive still finds it;
 *      E4 an ADMITTED reversal pair: the close fills at the open, the entry
 *         flips, and the held bracket is purged with its cycle (no zombie).
 *
 * Round 9 family V (campaign note log-20260905t165205z-69e4be06) NARROWS the
 * rule: the pair's strategy.close is a CLOSE-TIME act, issued after the bar's
 * intrabar broker events, so the dormancy it imposes must not feed that same
 * bar's forced-liquidation pass. 2b5e8e7 held the bracket dormant inside the
 * script body and the end-of-bar process_margin_call revived it at the
 * extreme — the round-8 candidate-i regressions on ETH/EURUSD/XAUUSD@15
 * (rhyme17 ETH 2025-04-07 13:45Z: TV "Margin call" 2.294 @1557.76 then
 * "Long" 4.662 @1549.51 at the 14:00Z open; the engine closed the 4.662
 * "Short Exit" @1557.76). lab tv tapes scratchpad/famV/pins (ws-report-v1,
 * rangeProof covered):
 *   F. BINANCE:ETHUSDT.P 15, 2025-04-01..04-20, the 13:45Z bar
 *      (O 1493.53 H 1557.76 L 1489 C 1549.52), 100% short at the 13:30Z
 *      close, filled at the 13:45Z open:
 *      F1 famV-eth-pair-mcbar-reissue (stop avg+30 re-issued every bar; the
 *         pair Long + close("Short") at the 13:45Z close) and F2 -once (the
 *         stop issued once, with the pair): "Margin call" 2.208 @1557.76
 *         THEN "Long" 4.4875 @1549.51 at the 14:00Z open, then the long
 *         6.1999 closed by close_all at the 14:30Z open 1557.92 (csv
 *         33cb2aac). The close-time bracket does NOT fill at the extreme.
 *      F3 -norev (no pair): the same slice, then "Short Exit" 4.4875
 *         @1549.51 at the 14:00Z open — a bracket born at the close with a
 *         breached level fills at the next open (632e3afe).
 *      F4 -prevbar-admitted (short at the 13:00Z close, resting stop 1530
 *         from the 13:15Z close, the pair at the 13:30Z close, ADMITTED at
 *         the 13:45Z open 1493.53): 0.0708 @1515.35 "Margin call" on the
 *         entry bar, "Long" 6.5372 @1493.53, the purged stop never acts,
 *         the long 6.7819 rides to the 14:30Z open 1557.92 (583a6b81).
 *   G. OANDA:XAUUSD 1D, 2025-06-01..08-01, margin_short=50 — the 100% short
 *      (3 lots @3322.825) has headroom, so NO cascade anywhere:
 *      G1 famV-xau1d-noMC-rev (the pair at the 07-10 close, DECLINED at the
 *         07-13 open — no Long row): the resting stop 3370.325 does NOT
 *         fill on the 07-13 bar although H 3375.085 crosses it; the re-issue
 *         fills 07-15 21:00Z @3370.325 x3 (a82f6b99) — finding-311's kill on
 *         a bar with no revive, TV-pinned (E1's shape).
 *      G2 -norev: the stop fills on the 07-13 bar @3370.325 x3 (2886cc24).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

#include "test_m_admission_36_data.hpp"

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

// Bar-open UTC ms of the OANDA:XAUUSD 1D bars the tapes name (the CSV stamps
// are UTC+8: "2025-06-25 05:00" is the 2025-06-24 21:00Z bar).
constexpr int64_t kT0601 = 1748811600000LL;  // 2025-06-01 21:00Z (range start)
constexpr int64_t kT0623 = 1750712400000LL;  // 2025-06-23 21:00Z (short signal)
constexpr int64_t kT0624 = 1750798800000LL;  // 2025-06-24 21:00Z (entry bar)
constexpr int64_t kT0630 = 1751317200000LL;  // 2025-06-30 21:00Z
constexpr int64_t kT0710 = 1752181200000LL;  // 2025-07-10 21:00Z (reversal signal)
constexpr int64_t kT0713 = 1752440400000LL;  // 2025-07-13 21:00Z (the pinned bar)
constexpr int64_t kT0724 = 1753390800000LL;  // 2025-07-24 21:00Z (close_all)
constexpr int64_t kT0731 = 1753995600000LL;  // 2025-07-31 21:00Z (returnedRange.to)

struct Row {
    int64_t entry_ts;
    double entry_price;
    double qty;
    int64_t exit_ts;
    double exit_price;
    int kind;
    double pnl;
    bool is_long;
    std::string exit_id;
};

bool row_before(const Row& a, const Row& b) {
    if (a.entry_ts != b.entry_ts) return a.entry_ts < b.entry_ts;
    if (a.exit_ts != b.exit_ts) return a.exit_ts < b.exit_ts;
    return a.qty < b.qty;
}

// m1d-mcbar-stop-rev tv_trades.csv (4 trades, 8 rows) as (entry, exit) pairs.
static const Row kRevTape[] = {
    {kT0624, 3322.825, 0.04, kT0624, 3337.205, kExitMarginCall, -0.5752, false, ""},
    {kT0624, 3322.825, 0.04, kT0630, 3358.085, kExitMarginCall, -1.4104, false, ""},
    {kT0624, 3322.825, 1.0, kT0713, 3375.085, kExitMarginCall, -52.26, false, ""},
    {kT0624, 3322.825, 1.92, kT0713, 3375.085, kExitClose, -100.3392, false, "Short Exit"},
};

// m1d-mcbar-stop-norev tv_trades.csv (3 trades, 6 rows).
static const Row kNorevTape[] = {
    {kT0624, 3322.825, 0.04, kT0624, 3337.205, kExitMarginCall, -0.5752, false, ""},
    {kT0624, 3322.825, 0.04, kT0630, 3358.085, kExitMarginCall, -1.4104, false, ""},
    {kT0624, 3322.825, 2.92, kT0713, 3370.325, kExitClose, -138.7, false, "Short Exit"},
};

// The registry OANDA:XAUUSD 1D feed (79cdcfb671e5, test_m_admission_36_data
// .hpp) restricted to the tapes' returned range 2025-06-01 21:00Z ..
// 2025-07-31 21:00Z.
std::vector<Bar> xau_tape_bars() {
    std::vector<Bar> out;
    for (const m36_data::BarRow& r : m36_data::kXauDaily) {
        if (r.ts < kT0601 || r.ts > kT0731) continue;
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

struct BarRow15 {
    int64_t ts;
    double open, high, low, close;
};

// NASDAQ:AAPL 15, 2025-10-29 19:00Z .. 2025-10-30 14:15Z (the family-N
// mechanism-2 control bars, tests/test_aapl15_margin_brackets.cpp).
static const BarRow15 kAaplAlgoai1030[] = {
    {1761764400000LL, 269.52, 269.62, 268.28, 268.64},  // [0] 10-29 19:00
    {1761765300000LL, 268.65, 268.96, 268.3, 268.32},   // [1] 19:15 signal
    {1761766200000LL, 268.27, 269.2, 267.8, 269.2},     // [2] 19:30 entry bar
    {1761767100000LL, 269.21, 270.38, 269.05, 269.84},  // [3] 19:45 reversal signal
    {1761831000000LL, 271.96, 274.11, 270.61, 271.21},  // [4] 10-30 13:30 gap open
    {1761831900000LL, 271.18, 271.86, 270.84, 271.075}, // [5] 13:45
    {1761832800000LL, 271.08, 271.37, 270.01, 270.3},   // [6] 14:00
    {1761833700000LL, 270.3, 270.5, 268.99, 269.08},    // [7] 14:15
};

template <size_t N>
std::vector<Bar> to_bars(const BarRow15 (&rows)[N]) {
    std::vector<Bar> out;
    for (const BarRow15& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

struct Ohlc {
    double open, high, low, close;
};

// Synthetic daily bars at 1-day spacing.
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

// The tapes' broker: zero commission, 1x margin both sides, margin calls on,
// market fills at the next open, pyramiding 0.
class Probe : public BacktestEngine {
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
    std::function<void(Probe&, const Bar&, int)> script;
    void on_bar(const Bar& bar) override {
        if (script) script(*this, bar, bar_index_);
    }
    void all_in() {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
    }
    void set_margin_short(double pct) { margin_short_ = pct; }
    void entry_default(const std::string& id, bool is_long) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, "");
    }
    void entry_market(const std::string& id, bool is_long, double qty) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void exit_stop(const std::string& id, const std::string& from, double stop) {
        strategy_exit(id, from, kNaN, stop);
    }
    void exit_limit_stop(const std::string& id, const std::string& from,
                         double limit, double stop) {
        strategy_exit(id, from, limit, stop);
    }
    void close_id(const std::string& id, const std::string& comment) {
        strategy_close(id, comment);
    }
    void close_all() { strategy_close_all(); }
    // strategy.position_avg_price: na when flat.
    double avg_price() const {
        return position_side_ == PositionSide::FLAT ? kNaN
                                                    : position_entry_price_;
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    bool is_short() const { return position_side_ == PositionSide::SHORT; }
    double pos_qty() const { return position_qty_; }
    // EXIT orders bound to `from_entry` still in the book.
    int brackets_bound_to(const std::string& from_entry) const {
        int n = 0;
        for (const PendingOrder& o : pending_orders_) {
            if (o.type == OrderType::EXIT && o.from_entry == from_entry) ++n;
        }
        return n;
    }

    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (const Trade& t : trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price,
                           t.exit_comment == "Margin call" ? kExitMarginCall
                                                           : kExitClose,
                           t.pnl, t.is_long, t.exit_id});
        }
        for (const Trade& t : range_end_trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price, kExitOpenAtEnd, t.pnl, t.is_long,
                           t.exit_id});
        }
        std::sort(out.begin(), out.end(), row_before);
        return out;
    }
    int margin_call_rows() const {
        int n = 0;
        for (const Trade& t : trades_) {
            if (t.exit_comment == "Margin call") ++n;
        }
        return n;
    }
    int long_rows() const {
        int n = 0;
        for (const Trade& t : trades_) {
            if (t.is_long) ++n;
        }
        return n;
    }
};

void print_row(const char* tag, const Row& r) {
    std::printf("      %s entry %lld @ %.5f qty %.5f exit %lld @ %.5f kind %d pnl %.5f [%s]\n",
                tag, (long long)r.entry_ts, r.entry_price, r.qty,
                (long long)r.exit_ts, r.exit_price, r.kind, r.pnl,
                r.exit_id.c_str());
}

void print_trades(const Probe& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.5f qty %.4f exit bar %d @ %.5f pnl %.5f [%s|%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str(), t.exit_id.c_str());
    }
}

// Row-for-row comparison of an engine replay against a TV tape. A tape row's
// exit_id names the strategy.exit id the fill must carry ("" = don't care).
template <size_t N>
void check_rows_match(const char* name, const std::vector<Row>& got,
                      const Row (&tape)[N]) {
    std::vector<Row> want(tape, tape + N);
    std::sort(want.begin(), want.end(), row_before);
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
            && g.is_long == w.is_long
            && std::fabs(g.pnl - w.pnl) <= 5e-3
            && (w.exit_id.empty() || g.exit_id == w.exit_id);
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

std::vector<Row> rows_exited_at(const std::vector<Row>& rows, int64_t ts) {
    std::vector<Row> out;
    for (const Row& r : rows) {
        if (r.exit_ts == ts) out.push_back(r);
    }
    return out;
}

// The tapes' script, verbatim:
//   if time == 2025-06-23 21:00Z: strategy.entry("Short", short)
//   strategy.exit("Short Exit", from_entry="Short",
//                 stop=strategy.position_avg_price + 47.5)   // every bar
//   rev only — if time == 2025-07-10 21:00Z:
//       strategy.entry("Long", long); strategy.close("Short", "Reverse to Long")
//   if time == 2025-07-24 21:00Z: strategy.close_all()
// with_limit adds the rhyme17 probe's TP leg on the same bracket (a limit far
// below the market, never touched in the window).
Probe run_tape(bool with_reversal, bool with_limit) {
    Probe p(10000.0, 0.001, 0.01, QtyType::PERCENT_OF_EQUITY, 100.0);
    p.script = [&](Probe& e, const Bar& bar, int) {
        if (bar.timestamp == kT0623) e.entry_default("Short", false);
        const double avg = e.avg_price();
        if (with_limit) {
            e.exit_limit_stop("Short Exit", "Short", avg - 150.0, avg + 47.5);
        } else {
            e.exit_stop("Short Exit", "Short", avg + 47.5);
        }
        if (with_reversal && bar.timestamp == kT0710) {
            e.entry_default("Long", true);
            e.close_id("Short", "Reverse to Long");
        }
        if (bar.timestamp == kT0724) e.close_all();
    };
    const std::vector<Bar> bars = xau_tape_bars();
    p.run(bars.data(), (int)bars.size());
    return p;
}

// The pinned 07-14 pair: "Margin call" 1.0 @3375.085 THEN "Short Exit" 1.92
// @3375.085, both on the 07-13 21:00Z bar; nothing at the open (the voided
// close), nothing at the level, nothing later.
void check_pinned_pair(const std::vector<Row>& got) {
    const std::vector<Row> jul13 = rows_exited_at(got, kT0713);
    CHECK(jul13.size() == 2);
    if (jul13.size() == 2) {
        CHECK(!jul13[0].is_long);
        CHECK_NEAR(jul13[0].qty, 1.0, 1e-9);
        CHECK_NEAR(jul13[0].exit_price, 3375.085, 1e-9);
        CHECK(jul13[0].kind == kExitMarginCall);
        CHECK_NEAR(jul13[0].pnl, -52.26, 5e-3);
        CHECK(!jul13[1].is_long);
        CHECK_NEAR(jul13[1].qty, 1.92, 1e-9);
        CHECK_NEAR(jul13[1].exit_price, 3375.085, 1e-9);
        CHECK(jul13[1].kind == kExitClose);
        CHECK(jul13[1].exit_id == "Short Exit");
        CHECK_NEAR(jul13[1].pnl, -100.3392, 5e-3);
    }
    int rows_at_level = 0;
    int rows_at_open = 0;
    int rows_after = 0;
    for (const Row& r : got) {
        if (std::fabs(r.exit_price - 3370.325) <= 1e-9) ++rows_at_level;
        if (r.exit_ts == kT0713 && std::fabs(r.exit_price - 3362.375) <= 1e-9) {
            ++rows_at_open;
        }
        if (r.exit_ts > kT0713) ++rows_after;
    }
    CHECK(rows_at_level == 0);
    CHECK(rows_at_open == 0);
    CHECK(rows_after == 0);
}

// ---------------------------------------------------------------------------
// A. rev tape.
// ---------------------------------------------------------------------------
void test_rev_tape() {
    std::printf("A. m1d-mcbar-stop-rev: dropped reversal -> margin call at the high, stop at the high\n");
    Probe p = run_tape(/*with_reversal=*/true, /*with_limit=*/false);
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("m1d-mcbar-stop-rev", got, kRevTape);
    check_pinned_pair(got);
    CHECK(p.long_rows() == 0);   // the reversal never filled
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// B. norev tape.
// ---------------------------------------------------------------------------
void test_norev_tape() {
    std::printf("B. m1d-mcbar-stop-norev: no reversal -> stop at its level, no margin call\n");
    Probe p = run_tape(/*with_reversal=*/false, /*with_limit=*/false);
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("m1d-mcbar-stop-norev", got, kNorevTape);
    const std::vector<Row> jul13 = rows_exited_at(got, kT0713);
    CHECK(jul13.size() == 1);
    if (jul13.size() == 1) {
        CHECK_NEAR(jul13[0].qty, 2.92, 1e-9);
        CHECK_NEAR(jul13[0].exit_price, 3370.325, 1e-9);
        CHECK(jul13[0].kind == kExitClose);
        CHECK(jul13[0].exit_id == "Short Exit");
    }
    CHECK(p.margin_call_rows() == 2);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// C. The rhyme17 probe shape: strategy.exit("Short Exit", from_entry="Short",
//    limit=short_tp, stop=short_sl) re-issued on every bar in position — the
//    07-14 rows TV 3/4 are the tape's rows.
// ---------------------------------------------------------------------------
void test_rev_probe_shape_limit_and_stop() {
    std::printf("C. rhyme17 shape (limit + stop bracket, re-issued every bar): the same 07-14 pair\n");
    Probe p = run_tape(/*with_reversal=*/true, /*with_limit=*/true);
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("rev, limit+stop bracket", got, kRevTape);
    check_pinned_pair(got);
    CHECK(p.long_rows() == 0);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// D. Family-N mechanism 2 control — algoai 10-30 13:30Z (lab tv tape
//    aapl15-mcopen1-stop-algoai): fixed 2891 short @268.27, capital
//    775,794.02, stop 273.69; an all-in Long placed at the 10-29 19:45Z close
//    is dropped at the 10-30 open 271.96. The OPEN breaches: 1 @271.96 open
//    slice (decline -> dormant -> slice -> revive, bracket LIVE), then 'X'
//    2814 @273.69 AT ITS LEVEL on the same bar. Unchanged by mechanism 2a.
// ---------------------------------------------------------------------------
void test_algoai_open_slice_control_unchanged() {
    std::printf("D. family-N M2 control: algoai 10-30 open slice 1 @271.96 then 'X' 2814 @273.69 at its level\n");
    Probe p(775794.02, 0.01, 1.0, QtyType::FIXED, 1.0);
    p.all_in();
    p.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) {
            e.entry_market("S", false, 2891.0);
            e.exit_stop("X", "S", 273.69);
        }
        if (bar == 3) e.entry_default("L", true);   // declined at the open
    };
    std::vector<Bar> bars = to_bars(kAaplAlgoai1030);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 3);
    CHECK(p.margin_call_rows() == 2);
    CHECK(p.long_rows() == 0);
    if (p.trade_count() == 3) {
        const Trade& t0 = p.get_trade(0);
        CHECK(t0.entry_bar_index == 2);
        CHECK_NEAR(t0.entry_price, 268.27, 1e-9);
        CHECK_NEAR(t0.qty, 76.0, 1e-9);
        CHECK(t0.exit_bar_index == 2);
        CHECK_NEAR(t0.exit_price, 269.20, 1e-9);
        CHECK(t0.exit_comment == "Margin call");
        const Trade& t1 = p.get_trade(1);
        CHECK_NEAR(t1.qty, 1.0, 1e-9);
        CHECK(t1.exit_bar_index == 4);
        CHECK_NEAR(t1.exit_price, 271.96, 1e-9);
        CHECK(t1.exit_comment == "Margin call");
        const Trade& t2 = p.get_trade(2);
        CHECK_NEAR(t2.qty, 2814.0, 1e-9);
        CHECK(t2.exit_bar_index == 4);
        CHECK_NEAR(t2.exit_price, 273.69, 1e-9);   // its LEVEL, not the 274.11 high
        CHECK(t2.exit_id == "X");
        CHECK_NEAR(t2.pnl, -15251.88, 5e-3);
    }
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// E. Synthetic controls (mintick 0.01, 1-share lots, no commission, 10,000):
//    a FIXED short whose all-in Long reversal (default percent_of_equity 100,
//    switched on at the reversal signal) is declined at the next open by the
//    pinned admission (Q x open > E_s), the stop bracket re-issued every bar
//    like rhyme17's.
// ---------------------------------------------------------------------------

// E1: no deficit anywhere — a 50-share short @100.00 (required at 101.50 =
//     5075 vs equity 9925). Bar 3 declines the reversal at its 101.40 open
//     (Q = floor(9975 / 100.50) = 99 x 101.40 = 10038.6 > 9975), the open
//     having gapped through the 101.00 stop: the dormant stop does not fill
//     on bar 3 (finding-311), the bar-3 re-issue is the fresh order and fills
//     at its level 101.00 on bar 4.
void test_synth_no_deficit_dormant_stop_waits() {
    std::printf("E1. declined reversal, no deficit at the extreme: the dormant stop does not fill on that bar\n");
    Probe p(10000.0, 0.01, 1.0, QtyType::FIXED, 50.0);
    p.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) e.entry_default("S", false);
        if (bar >= 2 && e.is_short()) e.exit_stop("X", "S", 101.00);
        if (bar == 2) {
            e.all_in();                    // the reversal is an all-in default long
            e.entry_default("L", true);
            e.close_id("S", "Reverse to Long");
        }
    };
    const std::vector<Bar> bars = synth_bars({
        {100.00, 100.20, 99.80, 100.00},   // [0]
        {100.00, 100.20, 99.80, 100.00},   // [1] signal close
        {100.00, 100.30, 99.70, 100.50},   // [2] entry bar @100.00; reversal signal
        {101.40, 101.50, 100.20, 100.30},  // [3] reversal declined at 101.40; stop level 101.00 already gapped through
        {100.40, 101.20, 100.10, 100.50},  // [4] the fresh re-issued stop fills at its level 101.00
        {100.00, 100.20, 99.80, 100.00},   // [5]
    });
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.margin_call_rows() == 0);
    CHECK(p.long_rows() == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK_NEAR(t.qty, 50.0, 1e-9);
        CHECK(t.exit_bar_index == 4);            // not bar 3 (dormant), bar 4
        CHECK_NEAR(t.exit_price, 101.00, 1e-9);  // at its level
        CHECK(t.exit_id == "X");
    }
    CHECK(p.flat());
}

// E2: the M-2a shape on a LOW-first (bullish) bar — an 80-share short
//     @100.00, stop 105.00; bar 3 O 101.50 L 100.80 H 114.00 C 112.00: the
//     reversal is declined at the open (Q = floor(9960 / 100.50) = 99 x
//     101.50 = 10048.5 > 10,000 on any basis), the stop's level 105.00 is
//     touched on the L->H leg BEFORE the extreme. With no reversal the stop
//     fills at 105.00 x80 and nothing is sliced (finding-308: the exit
//     precedes the extreme). With the declined reversal the bracket is
//     dormant on the path, the 114.00 high breaches (equity 8880 vs required
//     9120: q_min = 80 - 8880 / 114 = 2.105 -> 2 -> 4x = 8 @114.00 "Margin
//     call"), and the revived bracket closes the 72 remainder @114.00 on the
//     same bar — the re-issue at bar 3's close notwithstanding.
std::vector<Bar> synth_e2_bars() {
    return synth_bars({
        {100.00, 100.20, 99.80, 100.00},   // [0]
        {100.00, 100.20, 99.80, 100.00},   // [1] signal close (short)
        {100.00, 100.30, 99.70, 100.50},   // [2] entry bar @100.00; reversal signal at its close
        {101.50, 114.00, 100.80, 112.00},  // [3] the declined-reversal bar (low first)
        {110.00, 111.00, 109.00, 110.00},  // [4]
        {110.00, 110.20, 109.80, 110.00},  // [5]
    });
}

void test_synth_low_first_bar_slice_then_stop_at_extreme() {
    std::printf("E2. declined reversal on a low-first bar: 8 @114.00 'Margin call' then 'X' 72 @114.00\n");
    Probe p(10000.0, 0.01, 1.0, QtyType::FIXED, 80.0);
    p.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) e.entry_default("S", false);
        if (bar >= 2 && e.is_short()) e.exit_stop("X", "S", 105.00);
        if (bar == 2) {
            e.all_in();
            e.entry_default("L", true);
            e.close_id("S", "Reverse to Long");
        }
    };
    const std::vector<Bar> bars = synth_e2_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.long_rows() == 0);
    CHECK(p.margin_call_rows() == 1);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t0 = p.get_trade(0);
        CHECK(!t0.is_long);
        CHECK_NEAR(t0.qty, 8.0, 1e-9);
        CHECK(t0.exit_bar_index == 3);
        CHECK_NEAR(t0.exit_price, 114.00, 1e-9);
        CHECK(t0.exit_comment == "Margin call");
        CHECK_NEAR(t0.pnl, -112.0, 5e-3);
        const Trade& t1 = p.get_trade(1);
        CHECK(!t1.is_long);
        CHECK_NEAR(t1.qty, 72.0, 1e-9);
        CHECK(t1.exit_bar_index == 3);
        CHECK_NEAR(t1.exit_price, 114.00, 1e-9);   // the extreme, not 105.00
        CHECK(t1.exit_id == "X");
        CHECK_NEAR(t1.pnl, -1008.0, 5e-3);
    }
    CHECK(p.flat());

    // Control: without the reversal the same bar fills the stop at its
    // level and slices nothing.
    Probe q(10000.0, 0.01, 1.0, QtyType::FIXED, 80.0);
    q.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) e.entry_default("S", false);
        if (bar >= 2 && e.is_short()) e.exit_stop("X", "S", 105.00);
    };
    q.run(bars.data(), (int)bars.size());
    print_trades(q);
    CHECK(q.margin_call_rows() == 0);
    CHECK(q.trade_count() == 1);
    if (q.trade_count() == 1) {
        const Trade& t = q.get_trade(0);
        CHECK_NEAR(t.qty, 80.0, 1e-9);
        CHECK(t.exit_bar_index == 3);
        CHECK_NEAR(t.exit_price, 105.00, 1e-9);
        CHECK(t.exit_id == "X");
        CHECK_NEAR(t.pnl, -400.0, 5e-3);
    }
    CHECK(q.flat());
}

// E3: E2 with the bracket issued once, on the entry bar only — the
//     placement-time hold (not the re-issue inheritance) carries it to the
//     cascade: the same 8 @114.00 + 72 @114.00.
void test_synth_bracket_issued_once_held_through_pair_close() {
    std::printf("E3. bracket issued once, reversal pair declined: 8 @114.00 'Margin call' then 'X' 72 @114.00\n");
    Probe p(10000.0, 0.01, 1.0, QtyType::FIXED, 80.0);
    p.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) e.entry_default("S", false);
        if (bar == 2) {
            e.exit_stop("X", "S", 105.00);
            e.all_in();
            e.entry_default("L", true);
            e.close_id("S", "Reverse to Long");
        }
    };
    const std::vector<Bar> bars = synth_e2_bars();
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.long_rows() == 0);
    CHECK(p.margin_call_rows() == 1);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& t0 = p.get_trade(0);
        CHECK_NEAR(t0.qty, 8.0, 1e-9);
        CHECK(t0.exit_bar_index == 3);
        CHECK_NEAR(t0.exit_price, 114.00, 1e-9);
        CHECK(t0.exit_comment == "Margin call");
        const Trade& t1 = p.get_trade(1);
        CHECK_NEAR(t1.qty, 72.0, 1e-9);
        CHECK(t1.exit_bar_index == 3);
        CHECK_NEAR(t1.exit_price, 114.00, 1e-9);
        CHECK(t1.exit_id == "X");
    }
    CHECK(p.flat());
    CHECK(p.brackets_bound_to("S") == 0);
}

// E4: the ADMITTED pair. Bar 3 opens 100.20 (no gap): Q = floor(9960 /
//     100.50) = 99 x 100.20 = 9919.8 <= 9984 (equity at the open) — the
//     close fills 80 @100.20, the Long 99 @100.20 flips the cycle, and the
//     bracket the pair's close held dormant is purged as stale (no fill at
//     105.00 on bar 4's 105.50 high, no bracket bound to "S" left).
void test_synth_admitted_pair_purges_held_bracket() {
    std::printf("E4. admitted reversal pair: the close fills at the open, the held bracket is purged\n");
    Probe p(10000.0, 0.01, 1.0, QtyType::FIXED, 80.0);
    p.script = [](Probe& e, const Bar&, int bar) {
        if (bar == 1) e.entry_default("S", false);
        if (bar == 2) {
            e.exit_stop("X", "S", 105.00);
            e.all_in();
            e.entry_default("L", true);
            e.close_id("S", "Reverse to Long");
        }
    };
    const std::vector<Bar> bars = synth_bars({
        {100.00, 100.20, 99.80, 100.00},   // [0]
        {100.00, 100.20, 99.80, 100.00},   // [1] signal close (short)
        {100.00, 100.30, 99.70, 100.50},   // [2] entry bar @100.00; reversal signal at its close
        {100.20, 100.60, 99.90, 100.40},   // [3] the pair is admitted at 100.20
        {100.40, 105.50, 100.10, 105.00},  // [4] a live "X" would fill @105.00 here
        {105.00, 105.20, 104.80, 105.00},  // [5]
    });
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.margin_call_rows() == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK_NEAR(t.qty, 80.0, 1e-9);
        CHECK(t.exit_bar_index == 3);
        CHECK_NEAR(t.exit_price, 100.20, 1e-9);
        CHECK(t.exit_id != "X");
        CHECK_NEAR(t.pnl, -16.0, 5e-3);
    }
    CHECK(!p.flat());
    CHECK(!p.is_short());
    CHECK_NEAR(p.pos_qty(), 99.0, 1e-9);
    CHECK(p.brackets_bound_to("S") == 0);
}

// ---------------------------------------------------------------------------
// F. Round 9 family V — BINANCE:ETHUSDT.P 15 tapes (scratchpad/famV/pins).
// ---------------------------------------------------------------------------
constexpr int64_t kE1300 = 1744030800000LL;  // 2025-04-07 13:00Z
constexpr int64_t kE1315 = 1744031700000LL;  // 13:15Z
constexpr int64_t kE1330 = 1744032600000LL;  // 13:30Z (the probe's short signal)
constexpr int64_t kE1345 = 1744033500000LL;  // 13:45Z (entry bar, H 1557.76)
constexpr int64_t kE1400 = 1744034400000LL;  // 14:00Z (O 1549.51)
constexpr int64_t kE1415 = 1744035300000LL;  // 14:15Z (close_all signal)
constexpr int64_t kE1430 = 1744036200000LL;  // 14:30Z (O 1557.92)

// The registry BINANCE:ETHUSDT.P 15 feed (27b62431096e) 2025-04-07 12:15Z ..
// 15:00Z (lab bars).
static const BarRow15 kEth0407[] = {
    {1744028100000LL, 1484.81, 1519.12, 1480.77, 1516.79},  // 12:15
    {1744029000000LL, 1516.78, 1523.99, 1503.27, 1505.59},  // 12:30
    {1744029900000LL, 1505.58, 1526.48, 1497.0, 1520.01},   // 12:45
    {kE1300, 1520.01, 1523.69, 1508.72, 1513.3},            // 13:00
    {kE1315, 1513.31, 1515.35, 1501.01, 1502.54},           // 13:15
    {kE1330, 1502.54, 1518.0, 1486.23, 1493.54},            // 13:30
    {kE1345, 1493.53, 1557.76, 1489.0, 1549.52},            // 13:45
    {kE1400, 1549.51, 1599.46, 1538.22, 1587.34},           // 14:00
    {kE1415, 1587.34, 1638.47, 1549.0, 1558.24},            // 14:15
    {kE1430, 1557.92, 1594.41, 1544.33, 1562.93},           // 14:30
    {1744037100000LL, 1562.92, 1580.16, 1551.74, 1569.04},  // 14:45
    {1744038000000LL, 1569.08, 1576.8, 1545.39, 1548.14},   // 15:00
};

// famV-eth-pair-mcbar-reissue == famV-eth-pair-mcbar-once (csv 33cb2aac).
static const Row kEthPairMcbarTape[] = {
    {kE1345, 1493.53, 2.208, kE1345, 1557.76, kExitMarginCall, -141.81984, false, ""},
    {kE1345, 1493.53, 4.4875, kE1400, 1549.51, kExitClose, -251.21025, false, ""},
    {kE1400, 1549.51, 6.1999, kE1430, 1557.92, kExitClose, 52.14116, true, ""},
};
// famV-eth-pair-mcbar-norev (csv 632e3afe).
static const Row kEthMcbarNorevTape[] = {
    {kE1345, 1493.53, 2.208, kE1345, 1557.76, kExitMarginCall, -141.81984, false, ""},
    {kE1345, 1493.53, 4.4875, kE1400, 1549.51, kExitClose, -251.21025, false, "Short Exit"},
};
// famV-eth-pair-prevbar-admitted (csv 583a6b81).
static const Row kEthPrevbarAdmittedTape[] = {
    {kE1315, 1513.31, 0.0708, kE1315, 1515.35, kExitMarginCall, -0.144432, false, ""},
    {kE1315, 1513.31, 6.5372, kE1345, 1493.53, kExitClose, 129.30582, false, ""},
    {kE1345, 1493.53, 6.7819, kE1430, 1557.92, kExitClose, 436.68655, true, ""},
};

// The ETH tapes' broker: 10,000 USDT, mintick 0.01, lot 0.0001, 100% of
// equity, zero commission, 1x margin, margin calls on.
Probe run_eth(std::function<void(Probe&, const Bar&)> script) {
    Probe p(10000.0, 0.01, 0.0001, QtyType::PERCENT_OF_EQUITY, 100.0);
    p.script = [&](Probe& e, const Bar& bar, int) { script(e, bar); };
    const std::vector<Bar> bars = to_bars(kEth0407);
    p.run(bars.data(), (int)bars.size());
    return p;
}

// The 13:45Z pair: the slice at the high, then the short closed at the 14:00Z
// open by the admitted reversal — NOT by "Short Exit", and nothing at 1557.76
// beyond the slice.
void check_eth_pair_rows(const std::vector<Row>& got) {
    const std::vector<Row> at1345 = rows_exited_at(got, kE1345);
    CHECK(at1345.size() == 1);
    if (at1345.size() == 1) {
        CHECK(at1345[0].kind == kExitMarginCall);
        CHECK_NEAR(at1345[0].qty, 2.208, 1e-6);
    }
    const std::vector<Row> at1400 = rows_exited_at(got, kE1400);
    CHECK(at1400.size() == 1);
    if (at1400.size() == 1) {
        CHECK(!at1400[0].is_long);
        CHECK(at1400[0].kind == kExitClose);
        CHECK(at1400[0].exit_id != "Short Exit");
        CHECK_NEAR(at1400[0].exit_price, 1549.51, 1e-9);
    }
}

void test_famv_eth_pair_mcbar_reissue() {
    std::printf("F1. famV-eth-pair-mcbar-reissue: the close-time pair leaves the bar's slice alone; 'Long' closes the rest at the next open\n");
    Probe p = run_eth([](Probe& e, const Bar& bar) {
        if (bar.timestamp == kE1330) e.entry_default("Short", false);
        if (bar.timestamp == kE1345) e.entry_default("Long", true);
        e.exit_stop("Short Exit", "Short", e.avg_price() + 30.0);
        if (bar.timestamp == kE1345) e.close_id("Short", "Reverse to Long");
        if (bar.timestamp == kE1415) e.close_all();
    });
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-eth-pair-mcbar-reissue", got, kEthPairMcbarTape);
    check_eth_pair_rows(got);
    CHECK(p.margin_call_rows() == 1);
    CHECK(p.flat());
}

void test_famv_eth_pair_mcbar_once() {
    std::printf("F2. famV-eth-pair-mcbar-once: the stop issued once, with the pair — same rows\n");
    Probe p = run_eth([](Probe& e, const Bar& bar) {
        if (bar.timestamp == kE1330) e.entry_default("Short", false);
        if (bar.timestamp == kE1345) {
            e.entry_default("Long", true);
            e.exit_stop("Short Exit", "Short", e.avg_price() + 30.0);
            e.close_id("Short", "Reverse to Long");
        }
        if (bar.timestamp == kE1415) e.close_all();
    });
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-eth-pair-mcbar-once", got, kEthPairMcbarTape);
    check_eth_pair_rows(got);
    CHECK(p.flat());
}

void test_famv_eth_mcbar_norev() {
    std::printf("F3. famV-eth-pair-mcbar-norev: no pair — the close-born stop fills at the next open, not at the extreme\n");
    Probe p = run_eth([](Probe& e, const Bar& bar) {
        if (bar.timestamp == kE1330) e.entry_default("Short", false);
        e.exit_stop("Short Exit", "Short", e.avg_price() + 30.0);
        if (bar.timestamp == kE1415) e.close_all();
    });
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-eth-pair-mcbar-norev", got, kEthMcbarNorevTape);
    CHECK(p.long_rows() == 0);
    CHECK(p.flat());
}

void test_famv_eth_prevbar_admitted() {
    std::printf("F4. famV-eth-pair-prevbar-admitted: the pair admitted at the next open purges the resting stop\n");
    Probe p = run_eth([](Probe& e, const Bar& bar) {
        if (bar.timestamp == kE1300) e.entry_default("Short", false);
        if (bar.timestamp == kE1315) e.exit_stop("Short Exit", "Short", 1530.0);
        if (bar.timestamp == kE1330) {
            e.entry_default("Long", true);
            e.close_id("Short", "Reverse to Long");
        }
        if (bar.timestamp == kE1415) e.close_all();
    });
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-eth-pair-prevbar-admitted", got, kEthPrevbarAdmittedTape);
    int rows_at_1530 = 0;
    for (const Row& r : got) {
        if (std::fabs(r.exit_price - 1530.0) <= 1e-9) ++rows_at_1530;
    }
    CHECK(rows_at_1530 == 0);
    CHECK(p.brackets_bound_to("Short") == 0);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// G. Round 9 family V — OANDA:XAUUSD 1D, margin_short=50: no cascade, so the
//    declined-reversal bar shows finding-311's kill on its own.
// ---------------------------------------------------------------------------
constexpr int64_t kT0715 = 1752613200000LL;  // 2025-07-15 21:00Z

static const Row kNoMcRevTape[] = {
    {kT0624, 3322.825, 3.0, kT0715, 3370.325, kExitClose, -142.5, false, "Short Exit"},
};
static const Row kNoMcNorevTape[] = {
    {kT0624, 3322.825, 3.0, kT0713, 3370.325, kExitClose, -142.5, false, "Short Exit"},
};

Probe run_tape_no_mc(bool with_reversal) {
    Probe p(10000.0, 0.001, 0.01, QtyType::PERCENT_OF_EQUITY, 100.0);
    p.set_margin_short(50.0);
    p.script = [&](Probe& e, const Bar& bar, int) {
        if (bar.timestamp == kT0623) e.entry_default("Short", false);
        e.exit_stop("Short Exit", "Short", e.avg_price() + 47.5);
        if (with_reversal && bar.timestamp == kT0710) {
            e.entry_default("Long", true);
            e.close_id("Short", "Reverse to Long");
        }
        if (bar.timestamp == kT0724) e.close_all();
    };
    const std::vector<Bar> bars = xau_tape_bars();
    p.run(bars.data(), (int)bars.size());
    return p;
}

void test_famv_xau1d_no_mc_rev() {
    std::printf("G1. famV-xau1d-noMC-rev: declined reversal, no cascade — the dormant stop skips the 07-13 breach and the re-issue fills 07-15 at its level\n");
    Probe p = run_tape_no_mc(/*with_reversal=*/true);
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-xau1d-noMC-rev", got, kNoMcRevTape);
    CHECK(p.margin_call_rows() == 0);
    CHECK(p.long_rows() == 0);   // the reversal was declined
    CHECK(rows_exited_at(got, kT0713).empty());
    CHECK(p.flat());
}

void test_famv_xau1d_no_mc_norev() {
    std::printf("G2. famV-xau1d-noMC-norev: no reversal — the stop fills on the 07-13 bar at its level\n");
    Probe p = run_tape_no_mc(/*with_reversal=*/false);
    const std::vector<Row> got = p.rows();
    for (const Row& r : got) print_row("engine", r);
    check_rows_match("famV-xau1d-noMC-norev", got, kNoMcNorevTape);
    CHECK(p.margin_call_rows() == 0);
    CHECK(p.flat());
}

}  // namespace

int main() {
    std::printf("test_dropped_reversal_mc_first: round 7 family M mechanism 2a + round 9 family V\n");
    test_rev_tape();
    test_norev_tape();
    test_rev_probe_shape_limit_and_stop();
    test_algoai_open_slice_control_unchanged();
    test_synth_no_deficit_dormant_stop_waits();
    test_synth_low_first_bar_slice_then_stop_at_extreme();
    test_synth_bracket_issued_once_held_through_pair_close();
    test_synth_admitted_pair_purges_held_bracket();
    test_famv_eth_pair_mcbar_reissue();
    test_famv_eth_pair_mcbar_once();
    test_famv_eth_mcbar_norev();
    test_famv_eth_prevbar_admitted();
    test_famv_xau1d_no_mc_rev();
    test_famv_xau1d_no_mc_norev();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
