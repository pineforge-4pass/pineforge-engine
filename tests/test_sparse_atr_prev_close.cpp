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
class SparseAtrProbe : public BacktestEngine {
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
    void on_bar(const Bar&) override {
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

int main() {
    test_atr_four_arg_reads_chart_prev_close();
    test_atr_four_arg_recompute_is_idempotent();
    test_tr_four_arg();
    test_engine_prev_chart_close_tracker();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
