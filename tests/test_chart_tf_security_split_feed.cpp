/*
 * test_chart_tf_security_split_feed.cpp — round 7 family M, mechanism 4:
 * on the split (1m auxiliary) request.security feed path, a request for the
 * CHART's own timeframe reads the chart's bars, and "W" / "M" on a daily
 * chart aggregate those chart bars — never the 1m slice re-aggregated.
 *
 * Source: campaign note "round 7 family M mechanism 4/7" (amandaborgeson06
 * bias-status-dashboard NYSE:F@1D, moderate 80 count 0): the engine flips
 * long on 2026-04-09 (buySignal on the 04-08 close) where TradingView flips
 * 04-15 (signal 04-14). The sensor tape scratchpad/r7/pins/m1d-amanda-sense-f
 * (the probe's vote qty-encoded per bar) reads TradingView's 04-08 vote as
 * bull 5 / bear 5, mtfBull 4, chart stochBull 0 (K < D) -> longCond FALSE;
 * the run_strategy --trace-json of the engine's vote (spark, candidate-h
 * 1f905da, f-1d lane) reads bull 6 / bear 5 with mtfBull 5 — biasD 1 — while
 * the chart's own stoch reads K 90.1298 < D 90.3184 (stochBull 0, rsi 52.44
 * -> the D bias should be 0). The engine's request.security(tickerid, "D",
 * f_calcTfBias()) on the 1D chart was served by the 1-minute auxiliary
 * feed aggregated to daily buckets, whose 04-08 close is 12.195 (the last
 * 1m print) against the chart bar's 12.18: on that series K 90.84 > D 90.60
 * -> biasD 1 -> mtfBull 5 -> mtfBias 1 -> bull 6. TradingView's same-
 * timeframe request.security is the chart series itself (and its weekly
 * bar is the native daily bars aggregated, pinned 2026-09-05 wm-security-
 * buckets), so the D vote is the chart's: mtfBull 4 on 04-08, 5 on 04-09
 * (K 92.93 > D 89.98), 4 on 04-14 (K 94.756 < D 95.497), the TV signal bar.
 *
 * Bars: the registry NYSE:F 1D feed (test_m45_singletons_data.hpp). The
 * auxiliary slice is synthetic — three 1m bars per session whose last print
 * is one cent off the daily close (the real feed's after-hours offset) and
 * whose volumes do not sum to the day's — so a daily bucket built from the
 * slice is distinguishable from the chart bar on every day.
 *
 *   A. Every completed "D" bucket carries the chart bar's OHLCV (2026-04-08:
 *      close 12.18, not the slice's 12.19); the "W" bucket of 2026-04-06..
 *      04-10 is the chart dailies aggregated (o 11.6 h 12.42 l 11.35
 *      c 12.13, volume the sum); the finer "60" request keeps the slice
 *      (its last hourly bucket of 04-08 closes 12.19) — the control.
 *   B. The amanda daily components on the "D" series equal the chart's:
 *      2026-04-08 K 90.1298 < D 90.3184, rsi 52.4367 -> D bias 0; 04-09 K >
 *      D -> bias 1; 04-14 K 94.756 < D 95.497 -> bias 0 (the signal bar).
 *   C. An explicit native "D" feed of the chart bars (the 15m lanes'
 *      FEED_1D routing) gives byte-identical buckets — precedence kept.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/ta.hpp>

#include "test_m45_singletons_data.hpp"

using namespace pineforge;
using namespace m45_data;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "this test requires the auxiliary security feed (V1)"
#endif
#ifndef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
#error "this test requires the native security feed (V1)"
#endif

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

constexpr int64_t kMinute = 60000LL;
constexpr int64_t kApr06 = 1775482200000LL;   // 2026-04-06 13:30Z
constexpr int64_t kApr08 = 1775655000000LL;   // 2026-04-08 13:30Z
constexpr int64_t kApr09 = 1775741400000LL;
constexpr int64_t kApr13 = 1776087000000LL;
constexpr int64_t kApr14 = 1776173400000LL;

struct Bucket {
    int64_t ts;
    double open, high, low, close, volume;
};

// The amanda script's daily components: stoch RSI (14/14, K 3, D 3) and
// RSI 14 on closes — the same ta kernels the chart runs.
struct DailyVote {
    ta::RSI rsi{14};
    ta::RSI stoch_base{14};
    ta::Stoch stoch{14};
    ta::SMA k_sma{3};
    ta::SMA d_sma{3};
    std::array<double, 3> feed(double close) {
        const double r = rsi.compute(close);
        const double base = stoch_base.compute(close);
        const double raw = stoch.compute(base, base, base);
        const double k = k_sma.compute(raw);
        const double d = d_sma.compute(k);
        return {k, d, r};
    }
};

// stochBull = K > D and K > 50; rsiBull = rsi > 50; the EMA stack is bearish
// throughout April (emaFast < emaMid < emaSlow) so the bias is 1 only when
// both stochBull and rsiBull hold.
int d_bias_from(const std::array<double, 3>& kdr) {
    const bool stoch_bull = kdr[0] > kdr[1] && kdr[0] > 50.0;
    const bool rsi_bull = kdr[2] > 50.0;
    const int bull = (stoch_bull ? 1 : 0) + (rsi_bull ? 1 : 0);
    return bull >= 2 ? 1 : 0;
}

class MtfProbe final : public BacktestEngine {
public:
    std::vector<Bucket> daily, weekly, hourly;
    std::map<int64_t, std::array<double, 3>> chart_kdr;
    std::map<int64_t, std::array<double, 3>> d_kdr;
    DailyVote chart_vote, d_vote;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", input_tf_, false, false);
        register_security_eval(1, "W", input_tf_, false, false);
        register_security_eval(2, "60", input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        if (!is_complete) return;
        const Bucket b{bar.timestamp, bar.open, bar.high, bar.low, bar.close,
                       bar.volume};
        if (sec_id == 0) {
            daily.push_back(b);
            d_kdr[bar.timestamp] = d_vote.feed(bar.close);
        } else if (sec_id == 1) {
            weekly.push_back(b);
        } else {
            hourly.push_back(b);
        }
    }
    void on_bar(const Bar& bar) override {
        chart_kdr[bar.timestamp] = chart_vote.feed(bar.close);
    }
};

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = r.volume;
        out.push_back(b);
    }
    return out;
}

// The day's last 1m print, one cent off the daily close inside the range.
double slice_close(const Bar& day) {
    if (day.close + 0.01 <= day.high + 1e-12) return day.close + 0.01;
    if (day.close - 0.01 >= day.low - 1e-12) return day.close - 0.01;
    return day.close;
}

// Three 1m bars per session: the open print, a mid-session bar carrying the
// day's extremes, and the 15:59 ET print (slice_close). Volume 1 each.
std::vector<Bar> synth_aux(const std::vector<Bar>& chart) {
    std::vector<Bar> aux;
    for (const Bar& day : chart) {
        const double c3 = slice_close(day);
        aux.push_back({day.open, day.open, day.open, day.open, 1.0, day.timestamp});
        aux.push_back({day.open, day.high, day.low, 0.5 * (day.open + day.close), 1.0,
                       day.timestamp + 180 * kMinute});
        aux.push_back({c3, c3, c3, c3, 1.0, day.timestamp + 389 * kMinute});
    }
    return aux;
}

void configure(MtfProbe& probe, const std::vector<Bar>& aux) {
    strategy_set_syminfo_timezone(static_cast<pf_strategy_t>(&probe),
                                  "America/New_York");
    strategy_set_syminfo_session(static_cast<pf_strategy_t>(&probe),
                                 "0930-1600");
    CHECK(strategy_set_aux_security_feed(
              static_cast<pf_strategy_t>(&probe),
              reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
}

const Bucket* find_bucket(const std::vector<Bucket>& v, int64_t ts) {
    for (const Bucket& b : v) if (b.ts == ts) return &b;
    return nullptr;
}

void test_chart_tf_and_weekly_read_the_chart_bars() {
    std::printf("A. \"D\" buckets = chart bars, \"W\" = chart dailies aggregated, \"60\" keeps the slice\n");
    const std::vector<Bar> chart = to_bars(kFordDaily);
    const std::vector<Bar> aux = synth_aux(chart);
    MtfProbe probe;
    configure(probe, aux);
    probe.run(chart.data(), static_cast<int>(chart.size()), "1D", "1D",
              false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) {
        std::printf("   engine error: %s\n", probe.last_error().c_str());
        return;
    }
    std::printf("   daily buckets %zu, weekly %zu, hourly %zu, substitutions %lld, misses %lld\n",
                probe.daily.size(), probe.weekly.size(), probe.hourly.size(),
                (long long)probe.native_security_substitutions(),
                (long long)probe.native_security_misses());
    // Every completed daily bucket is the chart bar, not the slice.
    std::map<int64_t, Bar> chart_by_ts;
    for (const Bar& b : chart) chart_by_ts[b.timestamp] = b;
    int mismatched = 0, slice_like = 0;
    for (const Bucket& d : probe.daily) {
        auto it = chart_by_ts.find(d.ts);
        if (it == chart_by_ts.end()) { ++mismatched; continue; }
        const Bar& c = it->second;
        const bool same = std::fabs(d.open - c.open) < 1e-9
            && std::fabs(d.high - c.high) < 1e-9
            && std::fabs(d.low - c.low) < 1e-9
            && std::fabs(d.close - c.close) < 1e-9
            && std::fabs(d.volume - c.volume) < 1e-6;
        if (!same) ++mismatched;
        if (std::fabs(d.close - slice_close(c)) < 1e-9
            && std::fabs(slice_close(c) - c.close) > 1e-9) ++slice_like;
    }
    CHECK(probe.daily.size() >= chart.size() - 1);
    CHECK(mismatched == 0);
    CHECK(slice_like == 0);
    const Bucket* apr08 = find_bucket(probe.daily, kApr08);
    CHECK(apr08 != nullptr);
    if (apr08) {
        CHECK_NEAR(apr08->close, 12.18, 1e-9);
        CHECK_NEAR(apr08->open, 11.96, 1e-9);
        CHECK_NEAR(apr08->high, 12.24, 1e-9);
        CHECK_NEAR(apr08->volume, 58139980.0, 1e-3);
    }
    // The week of 2026-04-06..04-10 from the chart dailies.
    const Bucket* wk = nullptr;
    for (const Bucket& w : probe.weekly) {
        if (w.ts >= kApr06 && w.ts < kApr13) { wk = &w; break; }
    }
    CHECK(wk != nullptr);
    if (wk) {
        CHECK_NEAR(wk->open, 11.6, 1e-9);
        CHECK_NEAR(wk->high, 12.42, 1e-9);
        CHECK_NEAR(wk->low, 11.35, 1e-9);
        CHECK_NEAR(wk->close, 12.13, 1e-9);
        CHECK_NEAR(wk->volume,
                   24374590.0 + 41991577.0 + 58139980.0 + 32576248.0 + 28362740.0,
                   1e-3);
    }
    // Control: the finer "60" request is still the slice — the last hourly
    // bucket of 04-08 closes at the slice's 12.19.
    const Bucket* last_hour = nullptr;
    for (const Bucket& h : probe.hourly) {
        if (h.ts >= kApr08 && h.ts < kApr09) last_hour = &h;
    }
    CHECK(last_hour != nullptr);
    if (last_hour) CHECK_NEAR(last_hour->close, 12.19, 1e-9);
    CHECK(probe.native_security_substitutions() > 0);
    CHECK(probe.native_security_misses() == 0);

    std::printf("B. the amanda daily vote on the \"D\" series is the chart's\n");
    const int64_t days[] = {kApr08, kApr09, kApr14};
    for (int64_t ts : days) {
        CHECK(probe.chart_kdr.count(ts) == 1);
        CHECK(probe.d_kdr.count(ts) == 1);
        if (!probe.chart_kdr.count(ts) || !probe.d_kdr.count(ts)) continue;
        const auto& c = probe.chart_kdr[ts];
        const auto& d = probe.d_kdr[ts];
        std::printf("   %lld chart K %.4f D %.4f rsi %.4f | D-context K %.4f D %.4f rsi %.4f\n",
                    (long long)ts, c[0], c[1], c[2], d[0], d[1], d[2]);
        CHECK_NEAR(d[0], c[0], 1e-9);
        CHECK_NEAR(d[1], c[1], 1e-9);
        CHECK_NEAR(d[2], c[2], 1e-9);
        CHECK(d_bias_from(d) == d_bias_from(c));
    }
    // The engine trace's chart values (full-feed warm-up; the RMA seed of a
    // 170-bar start differs in the 4th decimal at most).
    CHECK_NEAR(probe.chart_kdr[kApr08][0], 90.1298, 0.02);
    CHECK_NEAR(probe.chart_kdr[kApr08][1], 90.3184, 0.02);
    CHECK_NEAR(probe.chart_kdr[kApr08][2], 52.4367, 0.02);
    CHECK(probe.chart_kdr[kApr08][0] < probe.chart_kdr[kApr08][1]);   // stochBull 0
    CHECK(d_bias_from(probe.d_kdr[kApr08]) == 0);                       // mtfBull 4, not 5
    CHECK(probe.chart_kdr[kApr09][0] > probe.chart_kdr[kApr09][1]);   // 04-09: D joins
    CHECK(d_bias_from(probe.d_kdr[kApr09]) == 1);
    CHECK_NEAR(probe.chart_kdr[kApr14][0], 94.7560, 0.02);
    CHECK_NEAR(probe.chart_kdr[kApr14][1], 95.4968, 0.02);
    CHECK(d_bias_from(probe.d_kdr[kApr14]) == 0);                       // TV's signal bar: mtfBull 4

    std::printf("C. an explicit native \"D\" feed of the chart bars is byte-identical\n");
    MtfProbe explicit_probe;
    configure(explicit_probe, aux);
    CHECK(strategy_set_native_security_feed(
              static_cast<pf_strategy_t>(&explicit_probe), "D",
              reinterpret_cast<const pf_bar_t*>(chart.data()),
              static_cast<int>(chart.size())) == 0);
    explicit_probe.run(chart.data(), static_cast<int>(chart.size()), "1D", "1D",
                       false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(explicit_probe.last_error().empty());
    CHECK(explicit_probe.daily.size() == probe.daily.size());
    CHECK(explicit_probe.weekly.size() == probe.weekly.size());
    int diff = 0;
    for (size_t i = 0; i < probe.daily.size() && i < explicit_probe.daily.size(); ++i) {
        const Bucket& a = probe.daily[i];
        const Bucket& b = explicit_probe.daily[i];
        if (a.ts != b.ts || a.close != b.close || a.volume != b.volume) ++diff;
    }
    for (size_t i = 0; i < probe.weekly.size() && i < explicit_probe.weekly.size(); ++i) {
        const Bucket& a = probe.weekly[i];
        const Bucket& b = explicit_probe.weekly[i];
        if (a.ts != b.ts || a.close != b.close || a.volume != b.volume) ++diff;
    }
    CHECK(diff == 0);
}

}  // namespace

int main() {
    std::printf("request.security at the chart's timeframe on the split feed — the chart series (registry NYSE:F 1D)\n");
    test_chart_tf_and_weekly_read_the_chart_bars();
    std::printf("%d checks passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
