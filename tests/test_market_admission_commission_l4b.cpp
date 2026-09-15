/*
 * test_market_admission_commission.cpp — TradingView's admission of a flat,
 * default-sized (percent_of_equity 100, margin 100) MARKET entry that carries
 * a percent COMMISSION (round 7 family H, market-entry-admission pin).
 *
 * Rule (campaign notes log-20260905t071818z-e57e7235 PINNED and
 * log-20260905t071819z-ece9b623; lab tv tapes scratchpad/r7/pins/
 * macd1d-mktadmit-{f-long,f-short,xau-long}, 206 placements, 0 violations):
 *
 *   qty      = floor_lot( equity / (tick(close(S)) * (1 + comm)) )
 *   admitted iff  qty * tick(fill) <= equity          (commission EXCLUDED)
 *
 * A placement that fails the test is DROPPED outright — no partial fill, no
 * margin call, no later fill — until the entry condition fires again (TV
 * dropped at +0.008% over equity and filled at -0.005% under). A placement
 * with cost <= equity < cost + fee FILLS and is margin-called on the entry
 * bar (the KI-61 entry-bar trim). Only pct == 100 / margin 100 / flat
 * placement is pinned. Engine: the design-cntvxiao-gap-reject drop in
 * engine_fills.cpp used to run only for a zero opening commission (a
 * commissioned gap filled and margin-called); it now drops regardless of the
 * commission, KI-61 kept for the fee-only shortfall.
 *
 * The three tapes are replayed on the registry bars of NYSE:F 1D and
 * OANDA:XAUUSD 1D (test_market_admission_commission_data.hpp): every TV row
 * must be reproduced — entry bar, fill price, quantity, exit bar, exit price,
 * "Margin call" vs close vs open-at-range-end, and net PnL — and the pinned
 * facts are asserted by name on top. The two probe cases the pin repairs
 * (z8830 bb-macd NYSE:F@1D 2025-09-18 / OANDA:XAUUSD@1D 2025-07-14) are
 * replayed as explicit signals with the probe's equity.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"

#include "test_market_admission_commission_data.hpp"

using namespace pineforge;
using namespace admission_tape_data;

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
                       r.exit_price, r.exit_kind, r.net_pnl});
    }
    std::sort(out.begin(), out.end(), row_before);
    return out;
}

// The bars whose open stamp falls in [from, to) — a contiguous slice that
// starts the engine's bar_index at 0 on the slice's first bar.
std::vector<Bar> slice(const std::vector<Bar>& bars, int64_t from, int64_t to) {
    std::vector<Bar> out;
    for (const Bar& b : bars) {
        if (b.timestamp >= from && b.timestamp < to) out.push_back(b);
    }
    return out;
}

// Broker/account of the tapes: 10,000 USD, percent_of_equity 100 with a 0.1%
// percent commission, 1x margin on both sides, margin calls on, market fills
// at the next bar's open. The instrument is set by (mintick, qty_step):
// NYSE:F = (0.01, 1 share), OANDA:XAUUSD cfd = (0.005, 0.01 lot).
class AdmissionProbe : public pineforge::source::PineStrategyHost {
public:
    AdmissionProbe(double mintick, double qty_step, double capital) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = mintick;
        qty_step_ = qty_step;
        process_orders_on_close_ = false;
        margin_call_enabled_ = true;
    }

    // Every closed trade plus TV's range-end row for a position still open
    // after the last bar, in the tape's sort order.
    std::vector<Row> rows() const {
        std::vector<Row> out;
        for (const Trade& t : trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price,
                           t.exit_comment == "Margin call" ? kExitMarginCall
                                                           : kExitClose,
                           t.pnl});
        }
        for (const Trade& t : range_end_trades_) {
            out.push_back({t.entry_time, t.entry_price, t.qty, t.exit_time,
                           t.exit_price, kExitOpenAtEnd, t.pnl});
        }
        std::sort(out.begin(), out.end(), row_before);
        return out;
    }

    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::round_to_mintick;
};

// The tapes' script: a default-sized entry on every 4th bar when flat, closed
// two bars later (fills at the next open, so a filled cycle is flat again on
// the next entry bar). is_long selects the long or the short tape.
class TapeProbe : public AdmissionProbe {
public:
    TapeProbe(double mintick, double qty_step, bool is_long)
        : AdmissionProbe(mintick, qty_step, 10000.0), is_long_(is_long) {}
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ % 4 == 0
            && std::fabs(signed_position_size()) <= 1e-12) {
            strategy_entry("E", is_long_);
        }
        if (bar_index_ % 4 == 2) strategy_close("E");
    }
private:
    bool is_long_;
};

// A default long entry on each listed signal bar (by bar-open stamp), nothing
// else — the probe cases' entry signals with the probe's equity.
class SignalProbe : public AdmissionProbe {
public:
    SignalProbe(double mintick, double qty_step, double capital,
                std::set<int64_t> signals)
        : AdmissionProbe(mintick, qty_step, capital),
          signals_(std::move(signals)) {}
    void on_source_bar(const Bar& bar) override {
        if (signals_.count(bar.timestamp)) strategy_entry("E", true);
    }
private:
    std::set<int64_t> signals_;
};

void print_row(const char* tag, const Row& r) {
    std::printf("      %s entry %lld @ %.5f qty %.4f exit %lld @ %.5f kind %d pnl %.5f\n",
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
    CHECK(mismatches == 0);
}

// Every engine entry fills on the bar right after a signal bar (bar_index
// 1 mod 4): a dropped placement never fills on a later bar — it is gone until
// the script's next strategy.entry call.
void check_entries_only_on_fill_bars(const std::vector<Row>& rows,
                                     const std::vector<Bar>& bars) {
    int off_phase = 0;
    for (const Row& r : rows) {
        int idx = -1;
        for (size_t i = 0; i < bars.size(); ++i) {
            if (bars[i].timestamp == r.entry_ts) { idx = (int)i; break; }
        }
        if (idx < 0 || idx % 4 != 1) ++off_phase;
    }
    CHECK(off_phase == 0);
}

// TV's quantity on every filled placement: floor_lot(equity / (tick(close_S)
// * 1.001)) with equity = 10,000 + the TAPE's cumulative net PnL of every
// trade closed by the signal bar (TV's own equity, so the check is on the
// sizing formula alone).
void check_qty_formula(const char* name, const std::vector<Row>& tape,
                       const std::vector<Bar>& bars, double mintick,
                       double qty_step) {
    int placements = 0, bad = 0;
    for (size_t s = 0; s + 1 < bars.size(); s += 4) {
        const int64_t fill_ts = bars[s + 1].timestamp;
        double filled_qty = 0.0;
        bool filled = false;
        double equity = 10000.0;
        for (const Row& r : tape) {
            if (r.entry_ts == fill_ts) { filled = true; filled_qty += r.qty; }
            if (r.exit_ts <= bars[s].timestamp && r.kind != kExitOpenAtEnd) {
                equity += r.pnl;
            }
        }
        if (!filled) continue;
        ++placements;
        const double tick_close =
            std::floor(bars[s].close / mintick + 0.5) * mintick;
        const double raw = equity / (tick_close * 1.001);
        const double want = std::floor(raw / qty_step + 1e-6) * qty_step;
        if (std::fabs(want - filled_qty) > 1e-6) {
            ++bad;
            std::printf("    %s signal bar %zu: tape qty %.4f, formula %.4f "
                        "(equity %.2f, tick close %.3f)\n",
                        name, s, filled_qty, want, equity, tick_close);
        }
    }
    std::printf("   %s: qty formula on %d filled placements\n", name, placements);
    CHECK(placements > 0);
    CHECK(bad == 0);
}

// The rows that entered on `fill_ts`.
std::vector<Row> rows_entered_at(const std::vector<Row>& rows, int64_t fill_ts) {
    std::vector<Row> out;
    for (const Row& r : rows) if (r.entry_ts == fill_ts) out.push_back(r);
    return out;
}

// ---------------------------------------------------------------------------
// NYSE:F 1D long tape: 68 placements, 43 filled (4 of them trimmed on the
// entry bar), 25 dropped; 47 TV rows.
// ---------------------------------------------------------------------------
void test_ford_long_tape() {
    std::printf("-- NYSE:F 1D long tape replay (macd1d-mktadmit-f-long) --\n");
    const std::vector<Bar> bars = to_bars(kFordDaily);
    TapeProbe eng(/*mintick=*/0.01, /*qty_step=*/1.0, /*is_long=*/true);
    eng.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = eng.rows();
    const std::vector<Row> want = to_rows(kFordLongTape);
    check_rows_match("f-long", got, want);
    check_entries_only_on_fill_bars(got, bars);
    check_qty_formula("f-long", want, bars, 0.01, 1.0);

    // DROPPED at +0.008% over equity: signal 2025-09-29 (bar 124, close
    // 12.09, equity 9214.95 -> 761 shares), fill 2025-09-30 open 12.11:
    // 761 x 12.11 = 9215.71 > 9214.95. No row, no partial, no margin call.
    CHECK(bars[124].timestamp == 1759152600000LL);
    CHECK(rows_entered_at(got, bars[125].timestamp).empty());
    // ... and not resurrected: nothing fills on bars 126..128 either; the
    // next entry is the script's next call (signal bar 128 -> fill bar 129).
    for (int i = 126; i <= 128; ++i) {
        CHECK(rows_entered_at(got, bars[i].timestamp).empty());
    }
    CHECK(!rows_entered_at(got, bars[129].timestamp).empty());
    // The other over-equity gaps: +0.023% (bar 24 -> 25), +0.049% (196 ->
    // 197), +0.014% (268 -> 269).
    CHECK(rows_entered_at(got, bars[25].timestamp).empty());
    CHECK(rows_entered_at(got, bars[197].timestamp).empty());
    CHECK(rows_entered_at(got, bars[269].timestamp).empty());

    // Fee-only shortfall FILLS and is trimmed on the entry bar: signal
    // 2025-07-28 (bar 80, close 11.28, equity 10125.50 -> 896), fill
    // 2025-07-29 open 11.29: 896 x 11.29 = 10115.84 <= 10125.50 admits,
    // + fee 10.12 = 10125.96 > 10125.50 -> KI-61 slice on the entry bar,
    // the remainder closes at the script's close. Same shape on bars 108,
    // 160 and 184.
    for (int s : {80, 108, 160, 184}) {
        const std::vector<Row> at = rows_entered_at(got, bars[s + 1].timestamp);
        CHECK(at.size() == 2);
        int trims = 0, closes = 0;
        for (const Row& r : at) {
            if (r.kind == kExitMarginCall) {
                ++trims;
                CHECK(r.exit_ts == bars[s + 1].timestamp);
            } else if (r.kind == kExitClose) {
                ++closes;
                CHECK(r.exit_ts == bars[s + 3].timestamp);
            }
        }
        CHECK(trims == 1);
        CHECK(closes == 1);
    }
    const std::vector<Row> at81 = rows_entered_at(got, bars[81].timestamp);
    double qty81 = 0.0;
    for (const Row& r : at81) { qty81 += r.qty; CHECK_NEAR(r.entry_price, 11.29, 1e-9); }
    CHECK_NEAR(qty81, 896.0, 1e-9);

    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// ---------------------------------------------------------------------------
// NYSE:F 1D short tape: the short side pays the same placement check — a
// favourable gap-down fills, an over-equity gap-up drops — 44 filled / 24
// dropped, 99 TV rows. Replayed ROW-FOR-ROW since the round-7 family-L
// entry-bar margin path (pineforge-engine round7/entry-bar-margin-path,
// tests/test_entry_bar_margin_path.cpp): the tape's 55 "Margin call" rows
// are the short's entry-bar liquidation — the fee-only shortfall trims ONE
// share at the fill (2025-09-30: 788 x 12.11 = 9542.68 <= 9547.86 < +9.54
// fee -> 1 @12.11), then the survivor cascades at the post-fill high (40 @
// 12.20; the engine used to print 44 @12.20 from the untrimmed 788, and its
// drifted equity moved three later quantities by one share: 2026-02-18 633,
// 03-12 802, 04-06 858) — and the two gap-open cycles whose pending
// strategy.close fills the whole position at the open before the open's
// margin evaluation (2025-04-23 1025 @9.84, 2026-04-08 842 @11.96; the
// engine used to slice 48 / 140 at the open first).
// ---------------------------------------------------------------------------

struct Placement {
    double entry_price;
    double qty;   // summed over the rows that entered on the fill bar
};

std::map<int64_t, Placement> placements_of(const std::vector<Row>& rows) {
    std::map<int64_t, Placement> out;
    for (const Row& r : rows) {
        auto it = out.find(r.entry_ts);
        if (it == out.end()) {
            out.emplace(r.entry_ts, Placement{r.entry_price, r.qty});
        } else {
            it->second.qty += r.qty;
        }
    }
    return out;
}

void test_ford_short_tape() {
    std::printf("-- NYSE:F 1D short tape replay (macd1d-mktadmit-f-short) --\n");
    const std::vector<Bar> bars = to_bars(kFordDaily);
    TapeProbe eng(/*mintick=*/0.01, /*qty_step=*/1.0, /*is_long=*/false);
    eng.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = eng.rows();
    const std::vector<Row> want = to_rows(kFordShortTape);
    check_rows_match("f-short", got, want);
    check_entries_only_on_fill_bars(got, bars);
    check_qty_formula("f-short", want, bars, 0.01, 1.0);

    const std::map<int64_t, Placement> got_p = placements_of(got);
    const std::map<int64_t, Placement> want_p = placements_of(want);
    std::printf("   f-short: engine %zu fills, tape %zu fills\n", got_p.size(),
                want_p.size());
    CHECK(got_p.size() == 44);
    CHECK(want_p.size() == 44);

    // The fee-only shortfall's one-share trim AT THE FILL, then the cascade
    // at the high over the survivor: 2025-09-30 (fill bar 125) 1 @12.11 +
    // 40 @12.20 + 747 closed 10-02; 2025-11-19 (bar 161) 1 @13.03 + 48 @13.15;
    // 2025-12-24 (bar 185) 1 @13.30 + 28 @13.38.
    for (int fb : {125, 161, 185}) {
        const std::vector<Row> at = rows_entered_at(got, bars[fb].timestamp);
        CHECK(at.size() == 3);
        int fill_price_trims = 0, high_slices = 0, closes = 0;
        for (const Row& r : at) {
            if (r.kind == kExitMarginCall && r.exit_ts == bars[fb].timestamp
                && std::fabs(r.exit_price - r.entry_price) <= 1e-9) {
                ++fill_price_trims;
                CHECK_NEAR(r.qty, 1.0, 1e-9);
            } else if (r.kind == kExitMarginCall) {
                ++high_slices;
                CHECK(r.exit_ts == bars[fb].timestamp);
                CHECK_NEAR(r.exit_price, eng.round_to_mintick(bars[fb].high), 1e-9);
            } else {
                ++closes;
            }
        }
        CHECK(fill_price_trims == 1);
        CHECK(high_slices == 1);
        CHECK(closes == 1);
    }
    // The pending close at a gap-open closes the WHOLE position, no open
    // slice: 2025-04-21's cycle (fill bar 13) closes 1025 @9.84 on 04-23
    // (bar 15) after its two slices 20 @9.63 (04-21) and 16 @9.72 (04-22);
    // 2026-04-06's cycle (bar 253) closes 842 @11.96 on 04-08 (bar 255).
    for (const auto& [fb, close_qty] :
         std::vector<std::pair<int, double>>{{13, 1025.0}, {253, 842.0}}) {
        const std::vector<Row> at = rows_entered_at(got, bars[fb].timestamp);
        int closes = 0;
        for (const Row& r : at) {
            if (r.kind == kExitClose) {
                ++closes;
                CHECK(r.exit_ts == bars[fb + 2].timestamp);
                CHECK_NEAR(r.qty, close_qty, 1e-9);
            } else {
                CHECK(r.exit_ts < bars[fb + 2].timestamp);   // no slice at that open
            }
        }
        CHECK(closes == 1);
    }

    // Dropped shorts: signal 2025-12-01 (bar 168, equity 9877.08 -> 749),
    // fill 2025-12-02 open 13.19: 749 x 13.19 = 9879.31 > 9877.08 (+0.023%)
    // — the third of five consecutive TV drops (signals 164..180, the
    // December gap-ups); nothing fills until the signal on bar 184 fills on
    // 185. Bars 196 -> 197 (+0.047%) and 268 -> 269 (+0.014%) likewise.
    for (int i = 165; i <= 184; ++i) CHECK(rows_entered_at(got, bars[i].timestamp).empty());
    CHECK(!rows_entered_at(got, bars[185].timestamp).empty());
    for (int i = 197; i <= 200; ++i) CHECK(rows_entered_at(got, bars[i].timestamp).empty());
    CHECK(!rows_entered_at(got, bars[201].timestamp).empty());
    for (int i = 269; i <= 271; ++i) CHECK(rows_entered_at(got, bars[i].timestamp).empty());
    // Filled at -0.05% under equity: signal 2025-09-29 (bar 124, 788
    // shares), fill 2025-09-30 open 12.11: 788 x 12.11 = 9542.68 <= 9547.86
    // — for a SHORT that gap-up is the ADVERSE side, and it still fills.
    const std::vector<Row> at125 = rows_entered_at(got, bars[125].timestamp);
    double qty125 = 0.0;
    for (const Row& r : at125) { qty125 += r.qty; CHECK_NEAR(r.entry_price, 12.11, 1e-9); }
    CHECK_NEAR(qty125, 788.0, 1e-9);
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// ---------------------------------------------------------------------------
// OANDA:XAUUSD 1D long tape (mintick 0.005, lot 0.01): 70 placements, 63
// filled (5 trimmed), 7 dropped; 68 TV rows, the last one open at the range
// end.
// ---------------------------------------------------------------------------
void test_xau_long_tape() {
    std::printf("-- OANDA:XAUUSD 1D long tape replay (macd1d-mktadmit-xau-long) --\n");
    const std::vector<Bar> bars = to_bars(kXauDaily);
    TapeProbe eng(/*mintick=*/0.005, /*qty_step=*/0.01, /*is_long=*/true);
    eng.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = eng.rows();
    const std::vector<Row> want = to_rows(kXauLongTape);
    check_rows_match("xau-long", got, want);
    check_entries_only_on_fill_bars(got, bars);
    check_qty_formula("xau-long", want, bars, 0.005, 0.01);

    // FILLED at -0.005% under equity: signal bar 144 (close 4099.40, equity
    // 12043.12 -> 2.93 lots), fill bar 145 open 4110.085: 2.93 x 4110.085 =
    // 12042.55 <= 12043.12 admits; + fee 12.04 > equity -> trimmed on the
    // entry bar. Bar 232 -> 233 is the same shape at -0.0046%.
    for (int s : {144, 232}) {
        const std::vector<Row> at = rows_entered_at(got, bars[s + 1].timestamp);
        CHECK(at.size() == 2);
        int trims = 0;
        for (const Row& r : at) {
            if (r.kind == kExitMarginCall) {
                ++trims;
                CHECK(r.exit_ts == bars[s + 1].timestamp);
            }
        }
        CHECK(trims == 1);
    }
    const std::vector<Row> at145 = rows_entered_at(got, bars[145].timestamp);
    double qty145 = 0.0;
    for (const Row& r : at145) { qty145 += r.qty; CHECK_NEAR(r.entry_price, 4110.085, 1e-9); }
    CHECK_NEAR(qty145, 2.93, 1e-9);

    // DROPPED at +0.039% (bar 24 -> 25: 3.12 x 3372.725 = 10522.90 >
    // 10518.77) and +0.031% (252 -> 253: 2.48 x 4521.855 = 11214.20 >
    // 11210.70).
    CHECK(rows_entered_at(got, bars[25].timestamp).empty());
    CHECK(rows_entered_at(got, bars[253].timestamp).empty());

    // The last cycle (signal bar 276, close signal bar 278) is still open
    // after the final bar: TV's Open row = the engine's range-end row.
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 2.42, 1e-9);
    const std::vector<Row> open_rows = rows_entered_at(got, bars[277].timestamp);
    CHECK(open_rows.size() == 1);
    if (!open_rows.empty()) {
        CHECK(open_rows[0].kind == kExitOpenAtEnd);
        CHECK(open_rows[0].exit_ts == bars[278].timestamp);
        CHECK_NEAR(open_rows[0].exit_price, 4613.835, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// The z8830 bb-macd probe cases the pin repairs, replayed as explicit signals
// on the registry bars with the probe's equity at the time.
// ---------------------------------------------------------------------------

// NYSE:F@1D, equity 10667.80 flat: the 2025-09-18 signal (close 11.74 ->
// 907 shares) gaps to 11.77 on 09-19: 907 x 11.77 = 10675.39 > 10667.80 ->
// TV drops it (the engine used to fill and margin-call: 4 + 903). The 09-23
// signal (close 11.73 -> 908) fills 09-24 at the open 11.73 = TV's trade 2.
void test_probe_ford_0918_dropped_0924_filled() {
    std::printf("-- probe z8830 NYSE:F@1D: 09-18 dropped, 09-24 filled @11.73 --\n");
    const std::vector<Bar> all = to_bars(kFordDaily);
    const int64_t sig_0918 = 1758202200000LL;   // 2025-09-18 13:30 UTC
    const int64_t fill_0919 = 1758288600000LL;  // 2025-09-19 13:30 UTC
    const int64_t sig_0923 = 1758634200000LL;   // 2025-09-23 13:30 UTC
    const int64_t fill_0924 = 1758720600000LL;  // 2025-09-24 13:30 UTC
    // 2025-09-02 .. 2025-09-30 (the engine's bar_index restarts at 0, the
    // signals are keyed by stamp).
    const std::vector<Bar> bars = slice(all, 1756700000000LL, 1759300000000LL);
    CHECK(bars.size() > 10);
    SignalProbe eng(0.01, 1.0, 10667.80, {sig_0918, sig_0923});
    eng.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = eng.rows();
    CHECK(rows_entered_at(got, fill_0919).empty());
    const std::vector<Row> at = rows_entered_at(got, fill_0924);
    CHECK(got.size() == 1);
    CHECK(at.size() == 1);
    if (!at.empty()) {
        CHECK_NEAR(at[0].entry_price, 11.73, 1e-9);
        CHECK_NEAR(at[0].qty, 908.0, 1e-9);
        CHECK(at[0].kind == kExitOpenAtEnd);
    }
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 908.0, 1e-9);
}

// OANDA:XAUUSD@1D, equity 10083.46 flat: the Friday 2025-07-11 signal (the
// registry bar stamped 2025-07-10 21:00 UTC, close 3355.665 -> 3.00 lots)
// gaps to 3362.375 on 07-14: 3.00 x 3362.375 = 10087.12 > 10083.46 -> TV
// drops it. The 07-16 signal (close 3347.60 -> 3.00) fills 07-17 at
// 3350.96 = TV's trade.
void test_probe_xau_0714_dropped_0717_filled() {
    std::printf("-- probe z8830 OANDA:XAUUSD@1D: 07-14 dropped, 07-17 filled @3350.96 --\n");
    const std::vector<Bar> all = to_bars(kXauDaily);
    const int64_t sig_0711 = 1752181200000LL;   // 2025-07-10 21:00 UTC (TV 07-11)
    const int64_t fill_0714 = 1752440400000LL;  // 2025-07-13 21:00 UTC (TV 07-14)
    const int64_t sig_0716 = 1752613200000LL;   // 2025-07-15 21:00 UTC (TV 07-16)
    const int64_t fill_0717 = 1752699600000LL;  // 2025-07-16 21:00 UTC (TV 07-17)
    const std::vector<Bar> bars = slice(all, 1751300000000LL, 1753100000000LL);
    CHECK(bars.size() > 8);
    SignalProbe eng(0.005, 0.01, 10083.46, {sig_0711, sig_0716});
    eng.run(bars.data(), (int)bars.size());
    const std::vector<Row> got = eng.rows();
    CHECK(rows_entered_at(got, fill_0714).empty());
    const std::vector<Row> at = rows_entered_at(got, fill_0717);
    CHECK(got.size() == 1);
    CHECK(at.size() == 1);
    if (!at.empty()) {
        CHECK_NEAR(at[0].entry_price, 3350.96, 1e-9);
        CHECK_NEAR(at[0].qty, 3.00, 1e-9);
        CHECK(at[0].kind == kExitOpenAtEnd);
    }
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 3.00, 1e-9);
}

}  // namespace

int main() {
    std::printf("--- market_admission_commission ---\n");
    test_ford_long_tape();
    test_ford_short_tape();
    test_xau_long_tape();
    test_probe_ford_0918_dropped_0924_filled();
    test_probe_xau_0714_dropped_0717_filled();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
