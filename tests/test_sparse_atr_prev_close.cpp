/*
 * test_sparse_atr_prev_close.cpp — issue #178: ta.atr() / ta.tr() called
 * inside a block that does not execute every bar.
 *
 * TradingView rule (pinned 2026-09-06, lab tv tape i178-sparse-atr-sense,
 * BINANCE:BTCUSDT 60 2025-04-01..07-01, ws-report-v1 rangeProof covered,
 * tv_trades.csv sha256 93147961ed5540bb6a413475e3db102869b63455d4bab1a331ca683218ec9960):
 * four qty-encoded sensors on every sparse execution (398 executions of
 * `if close[1] > open[1] and close < open`):
 *   B = ta.atr(3) inside the block
 *   C = ta.rma(chartTR, 3) inside the block, chartTR = ta.tr(true) EVERY bar
 *   A = ta.rma(trA, 3) inside the block, trA built from the close of the
 *       PREVIOUS EXECUTION of the block (the engine's per-object prev_close)
 *   T = ta.rma(ta.tr(true), 3) with ta.tr(true) itself inside the block
 * Result: B == C == T on 398/398 executions, B == A on 0/398. So the RMA
 * advances on the executions only, but the true range always reads the
 * previous CHART bar's close (close[1]) — never the previous execution's.
 *
 * The engine's ta::ATR / ta::TR keep a per-object prev_close that only
 * moves when compute() is called, which is exactly model A. This test pins
 * the 4-argument form (prev chart close handed in by the caller) and the
 * BacktestEngine::prev_chart_close() tracker a sparse call site must feed it.
 *
 * NDEBUG-PROOF: every assertion uses the returning CHECK macro.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

using namespace pineforge;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);\
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-12) {
    return std::fabs(a - b) <= tol;
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kT0 = 1743465600000LL;  // 2025-04-01 00:00 UTC
constexpr int64_t k1h = 3'600'000LL;

static Bar mk(int i, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 1000;
    b.timestamp = kT0 + i * k1h;
    return b;
}

// Ten chart bars; the sparse site executes on bars 1, 3, 4, 7, 9. Bars 2,
// 5, 6, 8 move the close far enough that "previous execution's close" and
// "previous chart bar's close" give different true ranges on 3, 7 and 9.
static std::vector<Bar> chart() {
    return {
        mk(0, 100, 101,  99,  100),
        mk(1, 100, 103,  99.5, 102),   // sparse
        mk(2, 102, 110, 101,  109),
        mk(3, 109, 111, 105,  106),   // sparse: chart prev 109 -> TR 6; prev exec 102 -> TR 9
        mk(4, 106, 107, 100,  101),   // sparse: TR 7 either way
        mk(5, 101, 120, 100,  118),
        mk(6, 118, 119, 112,  113),
        mk(7, 113, 115, 110,  111),   // sparse: chart prev 113 -> TR 5; prev exec 101 -> TR 14
        mk(8, 111, 112, 108,  109),
        mk(9, 109, 110, 104,  105),   // sparse: chart prev 109 -> TR 6; prev exec 111 -> TR 7
    };
}
static bool sparse(int i) { return i == 1 || i == 3 || i == 4 || i == 7 || i == 9; }

// Reference: TR against a given previous close, RMA(3) advanced on the
// executions only (SMA seed over the first 3 executions, then the Pine
// formula (src + (n-1) * rma[1]) / n — the same expression order ta::RMA uses).
struct RefRma3 {
    int n = 0; double sum = 0; double v = kNaN;
    double step(double tr) {
        ++n;
        if (n < 3) { sum += tr; return kNaN; }
        if (n == 3) { sum += tr; v = sum / 3.0; return v; }
        v = (tr + 2.0 * v) / 3.0;
        return v;
    }
};
static double tr_against(const Bar& b, double prev_close) {
    if (std::isnan(prev_close)) return b.high - b.low;
    return std::max({b.high - b.low, std::fabs(b.high - prev_close), std::fabs(b.low - prev_close)});
}
}  // namespace

// 1. ta::ATR: the 4-arg form follows the chart's previous close; the 3-arg
//    form (per-object prev_close) is the refuted model and differs.
static void test_atr_four_arg_reads_chart_prev_close() {
    std::printf("test_atr_four_arg_reads_chart_prev_close\n");
    const auto bars = chart();
    ta::ATR atr_chart(3), atr_legacy(3);
    RefRma3 ref_chart, ref_exec;
    double prev_exec_close = kNaN;
    int differing = 0;
    for (int i = 0; i < (int)bars.size(); ++i) {
        if (!sparse(i)) continue;
        const double prev_chart = (i > 0) ? bars[i - 1].close : kNaN;
        const double got = atr_chart.compute(bars[i].high, bars[i].low, bars[i].close, prev_chart);
        const double want = ref_chart.step(tr_against(bars[i], prev_chart));
        const double legacy = atr_legacy.compute(bars[i].high, bars[i].low, bars[i].close);
        const double want_legacy = ref_exec.step(tr_against(bars[i], prev_exec_close));
        prev_exec_close = bars[i].close;
        if (std::isnan(want)) { CHECK(std::isnan(got)); CHECK(std::isnan(legacy)); continue; }
        CHECK(near(got, want));
        CHECK(near(legacy, want_legacy));   // the 3-arg path is unchanged (every-bar callers)
        if (!near(got, legacy)) ++differing;
    }
    // Executions 3 (bar 4, seed 5.5 vs 6.5), 4 (bar 7) and 5 (bar 9) differ.
    CHECK(differing == 3);
}

// 2. recompute() restores and re-applies the 4-arg step (intrabar re-evaluation).
static void test_atr_four_arg_recompute_is_idempotent() {
    std::printf("test_atr_four_arg_recompute_is_idempotent\n");
    const auto bars = chart();
    ta::ATR a(3);
    double last = kNaN;
    for (int i = 0; i < (int)bars.size(); ++i) {
        if (!sparse(i)) continue;
        const double prev_chart = bars[i - 1].close;
        const double first = a.compute(bars[i].high, bars[i].low, bars[i].close, prev_chart);
        const double again = a.recompute(bars[i].high, bars[i].low, bars[i].close, prev_chart);
        if (std::isnan(first)) CHECK(std::isnan(again)); else CHECK(near(first, again));
        last = again;
    }
    // Same final value as a straight compute() walk.
    ta::ATR b(3);
    double straight = kNaN;
    for (int i = 0; i < (int)bars.size(); ++i)
        if (sparse(i)) straight = b.compute(bars[i].high, bars[i].low, bars[i].close, bars[i - 1].close);
    CHECK(near(last, straight));
}

// 3. ta::TR 4-arg: chart previous close; the first chart bar (na prev) is
//    na for ta.tr(false) and high-low for ta.tr(true).
static void test_tr_four_arg() {
    std::printf("test_tr_four_arg\n");
    const auto bars = chart();
    ta::TR tr_true(true), tr_false(false);
    CHECK(near(tr_true.compute(bars[0].high, bars[0].low, bars[0].close, kNaN), 2.0));
    CHECK(std::isnan(tr_false.compute(bars[0].high, bars[0].low, bars[0].close, kNaN)));
    // Sparse: bar 3 after bar 1 — chart prev close 109, not the execution's 102.
    ta::TR t(true);
    CHECK(near(t.compute(bars[1].high, bars[1].low, bars[1].close, bars[0].close), 3.5));
    CHECK(near(t.compute(bars[3].high, bars[3].low, bars[3].close, bars[2].close), 6.0));
    CHECK(near(t.recompute(bars[3].high, bars[3].low, bars[3].close, bars[2].close), 6.0));
    CHECK(near(t.compute(bars[7].high, bars[7].low, bars[7].close, bars[6].close), 5.0));
}

// 4. BacktestEngine::prev_chart_close() is the previous chart bar's close on
//    every on_bar dispatch (na on bar 0), and a sparse ATR site fed with it
//    reproduces the pinned values inside a running strategy.
class SparseAtrProbe : public pineforge::source::PineStrategyHost {
public:
    ta::ATR atr_{3};
    std::vector<double> prev_seen;
    std::vector<double> atr_seen;   // one per sparse execution
    SparseAtrProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        prev_seen.push_back(prev_chart_close());
        if (sparse(bar_index_)) {
            atr_seen.push_back(history_advances_new_bar()
                ? atr_.compute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close())
                : atr_.recompute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close()));
        }
    }
};

static void test_engine_prev_chart_close_tracker() {
    std::printf("test_engine_prev_chart_close_tracker\n");
    const auto bars = chart();
    SparseAtrProbe p;
    p.run(bars.data(), (int)bars.size());
    CHECK(p.prev_seen.size() == bars.size());
    for (int i = 0; i < (int)bars.size() && i < (int)p.prev_seen.size(); ++i) {
        if (i == 0) CHECK(std::isnan(p.prev_seen[0]));
        else CHECK(near(p.prev_seen[i], bars[i - 1].close));
    }
    RefRma3 ref;
    std::vector<double> want;
    for (int i = 0; i < (int)bars.size(); ++i)
        if (sparse(i)) want.push_back(ref.step(tr_against(bars[i], bars[i - 1].close)));
    CHECK(p.atr_seen.size() == want.size());
    for (size_t k = 0; k < want.size() && k < p.atr_seen.size(); ++k) {
        if (std::isnan(want[k])) CHECK(std::isnan(p.atr_seen[k]));
        else CHECK(near(p.atr_seen[k], want[k]));
    }
    // The pinned numbers themselves: seed (3.5 + 6 + 7) / 3 = 5.5, then
    // (5 + 2 * 5.5) / 3, then (6 + 2 * that) / 3.
    if (p.atr_seen.size() == 5) {
        CHECK(near(p.atr_seen[2], 5.5));
        CHECK(near(p.atr_seen[3], (5.0 + 2.0 * 5.5) / 3.0));
        CHECK(near(p.atr_seen[4], (6.0 + 2.0 * ((5.0 + 2.0 * 5.5) / 3.0)) / 3.0));
    }
    // Handle reuse: the tracker resets with the source series.
    SparseAtrProbe q;
    q.run(bars.data(), 3);
    q.run(bars.data(), (int)bars.size());
    CHECK(q.prev_seen.size() == 3 + bars.size());
    if (q.prev_seen.size() == 3 + bars.size()) {
        CHECK(std::isnan(q.prev_seen[3]));
        CHECK(near(q.prev_seen[4], bars[0].close));
    }
}


// 5. issue #178 follow-up (round 9: the first cut of this fix regressed JOAT
//    aureate on NASDAQ:AAPL / NYSE:F / OANDA:EURUSD @15 — an UNCONDITIONAL
//    ta.atr(14) under calc_on_order_fills = true). Under COOF every historical
//    fill recalculation and the ordinary close execution of a bar start from
//    the bar-start checkpoint and push the bar's history slot again, so the
//    chart-close tracker must roll back with that checkpoint: the close
//    execution of a bar whose open filled an order still reads the previous
//    CHART bar's close, never the recalc's own close (which turns the true
//    range max(h-l, |h-close[1]|, |l-close[1]|) into h-l and drops the gap —
//    exactly what an overnight gap on a stock lane exposes).
//    Pinned on TradingView 2026-09-06 (lab tv i178-joat-coof-atr-aapl15,
//    NASDAQ:AAPL 15, 2025-04-01..07-01, rangeProof covered, tv_trades.csv
//    sha256 4c7ac673dd35d1c290e51cc4d1ccf076ef315ef3bd013c6988b8f9b9c3198caa):
//    the qty-encoded ta.atr(14) at the close execution of every fill bar
//    equals the every-bar RMA over chart-close true ranges (e.g. 2025-05-02
//    13:30Z, the post-earnings gap: atr 1.596219 with close[1] = 212.85).
static std::vector<Bar> gapped_chart() {
    return {
        mk(0, 100, 101,  99, 100),
        mk(1, 104, 106, 103, 105),   // fill bar, gap up:   TR vs 100 = 6, h-l = 3
        mk(2, 105, 107, 104, 106),
        mk(3, 100, 101,  98,  99),   // fill bar, gap down: TR vs 106 = 8, h-l = 3
        mk(4,  99, 100,  97,  98),
        mk(5, 103, 105, 102, 104),   // fill bar, gap up:   TR vs 98 = 7, h-l = 3
        mk(6, 104, 106, 103, 105),
        mk(7, 100, 101,  99, 100),   // fill bar, gap down: TR vs 105 = 6, h-l = 2
        mk(8, 100, 102,  99, 101),
        mk(9, 101, 103, 100, 102),
    };
}

class CoofAtrProbe : public pineforge::source::PineStrategyHost {
public:
    struct Seen { int bar; double prev; double atr; bool fill_recalc; };
    std::vector<Seen> seen;
    ta::ATR atr_{3};
    ta::ATR atr_ckpt_{3};
    CoofAtrProbe() {
        calc_on_order_fills_ = true;
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        syminfo_mintick_ = 0.01;
    }
    // The generated subclass checkpoints its TA members exactly like this.
    void snapshot_script_state() override { atr_ckpt_ = atr_; }
    void restore_script_state() override { atr_ = atr_ckpt_; }
    void commit_script_state() override { atr_ckpt_ = atr_; }
    void on_source_bar(const Bar&) override {
        const double v = history_advances_new_bar()
            ? atr_.compute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close())
            : atr_.recompute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close());
        seen.push_back({bar_index_, prev_chart_close(), v, coof_fill_recalc_active_});
        // A market order at every even bar's close execution (entry when
        // flat, whole close when long): each fills at the next, odd bar's
        // open and triggers a fill recalculation there before that bar's
        // ordinary close execution.
        if (bar_index_ % 2 == 0 && !coof_fill_recalc_active_) {
            if (position_side_ == PositionSide::FLAT) strategy_entry("L", true);
            else strategy_close("L");
        }
    }
};

static void test_engine_prev_chart_close_rolls_back_with_coof_checkpoint() {
    std::printf("test_engine_prev_chart_close_rolls_back_with_coof_checkpoint\n");
    const auto bars = gapped_chart();
    CoofAtrProbe p;
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    // Two closed round trips (entry filled at bar 1, closed at 3; entry at 5,
    // closed at 7) and a third entry filled at bar 9, still open at the end.
    CHECK(p.trade_count() == 2);
    // Every odd bar ran twice (fill recalc + ordinary close), every even bar once.
    int per_bar[10] = {0};
    int recalcs = 0;
    for (const auto& s : p.seen) {
        if (s.bar >= 0 && s.bar < 10) ++per_bar[s.bar];
        if (s.fill_recalc) ++recalcs;
    }
    CHECK(recalcs == 5);
    for (int i = 0; i < 10; ++i) CHECK(per_bar[i] == ((i % 2 == 1) ? 2 : 1));
    // Every execution — the recalc AND the close execution of a fill bar —
    // sees the previous chart bar's close and the chart-close ATR.
    RefRma3 ref;
    std::vector<double> want;
    for (int i = 0; i < (int)bars.size(); ++i)
        want.push_back(ref.step(tr_against(bars[i], i > 0 ? bars[i - 1].close : kNaN)));
    for (const auto& s : p.seen) {
        if (s.bar == 0) CHECK(std::isnan(s.prev));
        else CHECK(near(s.prev, bars[s.bar - 1].close));
        if (std::isnan(want[s.bar])) CHECK(std::isnan(s.atr));
        else CHECK(near(s.atr, want[s.bar]));
    }
    // The pinned arithmetic: seed (2 + 6 + 3) / 3 on bar 2, then bar 3's
    // gap-down true range 8 (not h-l = 3) enters as (8 + 2 * 11/3) / 3.
    CHECK(near(want[2], 11.0 / 3.0));
    CHECK(near(want[3], (8.0 + 2.0 * (11.0 / 3.0)) / 3.0));
    // Handle reuse under COOF: the checkpointed tracker resets too.
    CoofAtrProbe q;
    q.run(bars.data(), 4);
    q.run(bars.data(), (int)bars.size());
    bool second_run_ok = true;
    int second_first = -1;
    for (size_t k = 0; k < q.seen.size(); ++k) {
        if (second_first < 0 && k > 0 && q.seen[k].bar == 0) second_first = (int)k;
    }
    CHECK(second_first > 0);
    if (second_first > 0) {
        for (size_t k = second_first; k < q.seen.size(); ++k) {
            const auto& s = q.seen[k];
            if (s.bar == 0) { if (!std::isnan(s.prev)) second_run_ok = false; }
            else if (!near(s.prev, bars[s.bar - 1].close)) second_run_ok = false;
        }
    }
    CHECK(second_run_ok);
}

// 6. The same rollback with a fill recalculation on EVERY bar (round 9, JOAT aureate): the tracker is
//    bar history and rolls back with the COOF checkpoint. On a bar whose open
//    fills the previous close's market order, the fill recalculation AND the
//    ordinary close execution both read prev_chart_close() == close[1] of
//    the chart, so an every-bar ta.atr fed with it reproduces the chart-close
//    RMA on every close execution. Before the fix the close execution read
//    the recalc's own close (the full script bar), i.e. true range high - low
//    (TradingView pin: lab tv i178-coof-atr-sense-aapl15, ta.atr(14) ==
//    ta.rma(ta.tr(true), 14) on 4315/4315 executions).
class CoofEveryBarAtrProbe : public pineforge::source::PineStrategyHost {
public:
    ta::ATR atr_{3};
    ta::ATR atr_ckpt_{3};
    struct Seen { int bar; bool recalc; double prev; double atr; };
    std::vector<Seen> seen;
    CoofEveryBarAtrProbe() {
        calc_on_order_fills_ = true;
        pyramiding_ = 0;
        initial_capital_ = 1'000'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        syminfo_mintick_ = 0.01;
        slippage_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
    }
    // The generated script's checkpoint discipline for its TA objects.
    void snapshot_script_state() override { atr_ckpt_ = atr_; }
    void restore_script_state() override { atr_ = atr_ckpt_; }
    void commit_script_state() override { atr_ckpt_ = atr_; }
    void on_source_bar(const Bar&) override {
        const double v = history_advances_new_bar()
            ? atr_.compute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close())
            : atr_.recompute(current_bar_.high, current_bar_.low, current_bar_.close, prev_chart_close());
        seen.push_back({bar_index_, coof_fill_recalc_active_, prev_chart_close(), v});
        // JOAT's shape reduced to its sensor: a reversing market order at every
        // close-time execution, so every bar's open carries a fill recalc.
        if (!coof_fill_recalc_active_) {
            if (bar_index_ % 2 == 0) strategy_entry("L", true);
            else strategy_entry("S", false);
        }
    }
};

// Ten chart bars that GAP: open != previous close on 1, 3, 4, 6, 8, so the
// chart-close true range differs from high - low there (the every-bar shape
// of chart() above has no gaps and cannot tell the two apart).
static std::vector<Bar> gap_chart_every_bar() {
    return {
        mk(0, 100, 101,  99, 100),   // TR 2 (first bar)
        mk(1, 104, 105, 103, 104),   // prev 100 -> TR 5; high - low 2
        mk(2, 104, 106, 103, 105),   // prev 104 -> TR 3
        mk(3, 110, 111, 109, 110),   // prev 105 -> TR 6; high - low 2
        mk(4, 108, 109, 107, 108),   // prev 110 -> TR 3; high - low 2
        mk(5, 108, 110, 106, 107),   // prev 108 -> TR 4
        mk(6, 100, 101,  99, 100),   // prev 107 -> TR 8; high - low 2
        mk(7, 100, 102,  98, 101),   // prev 100 -> TR 4
        mk(8, 105, 106, 104, 105),   // prev 101 -> TR 5; high - low 2
        mk(9, 105, 107, 103, 104),   // prev 105 -> TR 4
    };
}

static void test_engine_prev_chart_close_survives_coof_recalc_every_bar() {
    std::printf("test_engine_prev_chart_close_survives_coof_recalc_every_bar\n");
    const auto bars = gap_chart_every_bar();
    CoofEveryBarAtrProbe p;
    p.run(bars.data(), (int)bars.size());
    int recalcs = 0, closes = 0;
    RefRma3 ref;
    std::vector<double> want;
    for (int i = 0; i < (int)bars.size(); ++i)
        want.push_back(ref.step(tr_against(bars[i], i == 0 ? kNaN : bars[i - 1].close)));
    for (const auto& s : p.seen) {
        CHECK(s.bar >= 0 && s.bar < (int)bars.size());
        if (s.bar < 0 || s.bar >= (int)bars.size()) continue;
        // Every execution of bar i — recalc or close — reads the chart's close[1].
        if (s.bar == 0) CHECK(std::isnan(s.prev));
        else CHECK(near(s.prev, bars[s.bar - 1].close));
        if (s.recalc) { ++recalcs; continue; }
        ++closes;
        if (std::isnan(want[s.bar])) CHECK(std::isnan(s.atr));
        else CHECK(near(s.atr, want[s.bar]));
    }
    CHECK(closes == (int)bars.size());
    // Bars 1..9 open with a fill of the previous close's market order.
    CHECK(recalcs >= (int)bars.size() - 1);
    // The pinned numbers: seed (2 + 5 + 3) / 3 on bar 2 (high - low would seed
    // (2 + 2 + 3) / 3), then (6 + 2 * seed) / 3 on bar 3 (high - low: 2).
    for (const auto& s : p.seen) {
        if (s.recalc) continue;
        if (s.bar == 2) CHECK(near(s.atr, (2.0 + 5.0 + 3.0) / 3.0));
        if (s.bar == 3) CHECK(near(s.atr, (6.0 + 2.0 * ((2.0 + 5.0 + 3.0) / 3.0)) / 3.0));
    }
}

int main() {
    test_atr_four_arg_reads_chart_prev_close();
    test_atr_four_arg_recompute_is_idempotent();
    test_tr_four_arg();
    test_engine_prev_chart_close_tracker();
    test_engine_prev_chart_close_rolls_back_with_coof_checkpoint();
    test_engine_prev_chart_close_survives_coof_recalc_every_bar();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
