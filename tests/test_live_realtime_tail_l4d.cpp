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
Bar flat_bar(double p, int64_t ts) { return Bar{p, p, p, p, 1.0, ts}; }
// Enters long on bar 2 and holds; records islast, islastbar and last_bar_index per bar.
class HoldStrategy final : public pineforge::source::PineStrategyHost {
public:
    std::vector<bool> islast, islastbar;
    std::vector<int> last_index;
    int64_t last_time = 0;
    void on_source_bar(const Bar&) override {
        islast.push_back(barstate_islast_);
        islastbar.push_back(session_islastbar_);
        last_index.push_back(pine_last_bar_index());
        last_time = last_bar_time_;
        if (bar_index_ == 2) strategy_entry("L", true);
    }
};
std::vector<Bar> bars_1m(int n) {
    std::vector<Bar> v;
    for (int i = 0; i < n; ++i) v.push_back(flat_bar(100.0 + i, i * 60'000LL));
    return v;
}
// 12 one-minute bars with a 3-bar hole after bar 5 (final-rereview.md N2):
// bars[0..5] one minute apart, then a 4-minute step (the 3-bar hole) into
// bars[6..11], one minute apart again. Discriminates the exact/extrapolate-
// from-last-bar rule (script_bar_geometry == true) from the pre-fix
// extrapolate-from-bars[0] rule, which a gapless feed cannot.
std::vector<Bar> bars_1m_gapped_after_5() {
    std::vector<Bar> v;
    for (int i = 0; i <= 5; ++i) v.push_back(flat_bar(100.0 + i, i * 60'000LL));
    int64_t t = 5 * 60'000LL;
    for (int i = 6; i < 12; ++i) {
        t += 4 * 60'000LL;  // one normal step + the 3-bar hole, then 1-minute steps
        v.push_back(flat_bar(100.0 + i, t));
        t -= 3 * 60'000LL;  // subsequent bars are 1 minute apart again
    }
    return v;
}
}
int main() {
    const auto bars = bars_1m(10);
    // Baseline: flag off.
    HoldStrategy off;
    off.run(bars.data(), 10);
    CHECK(off.islast.back());
    CHECK(off.last_index.back() == 9);
    CHECK(off.last_time == 9 * 60'000LL);            // unchanged with the flag off
    ReportC r_off{};
    off.fill_report(&r_off);
    CHECK(r_off.trades_len == 1 && r_off.trades[0].open_at_end == 1);   // range-end row
    const double eq_off_last = r_off.equity_curve[r_off.equity_curve_len - 1].equity;
    CHECK(near(r_off.equity_curve[r_off.equity_curve_len - 1].open_profit, 0.0));  // range-end re-mark
    BacktestEngine::free_report(&r_off);

    // Flag on, horizon 1000 bars.
    HoldStrategy on;
    on.set_realtime_tail(true, 1000);
    on.run(bars.data(), 10);
    CHECK(!on.islast.back());                        // tail is not islast
    for (int i = 0; i + 1 < 10; ++i) CHECK(on.islast[i] == off.islast[i]);   // interior identical
    CHECK(on.last_index.back() == 999);              // frozen horizon
    CHECK(!on.islastbar.back());                     // 24x7 default session: never last bar
    CHECK(on.last_time == 999LL * 60'000LL);         // last_bar_time_ frozen at the horizon bar
    ReportC r_on{};
    on.fill_report(&r_on);
    CHECK(r_on.trades_len == 0);                     // no open_at_end row
    const double eq_on_last = r_on.equity_curve[r_on.equity_curve_len - 1].equity;
    // Both runs hold 1 unit bought at bar 3's open (103) and the last close is 109:
    // open_profit 6 is kept on the tail equity point; the flag-off curve was
    // re-marked by the range-end close and equals initial + realized 6 too.
    CHECK(near(eq_on_last, eq_off_last));
    CHECK(near(r_on.equity_curve[r_on.equity_curve_len - 1].open_profit, 6.0));
    BacktestEngine::free_report(&r_on);

    // TF-aware path (run_tf_impl -> run_simple_bar_loop): the only path that
    // sets session state (session.islastbar), and the path pineforge-live
    // drives. input_tf == script_tf == "1" selects run_simple_bar_loop with
    // no aggregation/magnifier.
    HoldStrategy tf_off;
    tf_off.run(bars.data(), 10, "1", "1");
    CHECK(tf_off.islast.back());
    CHECK(tf_off.islastbar.back());                  // old rule: fires on the array's last bar
    CHECK(tf_off.last_index.back() == 9);

    HoldStrategy tf_on;
    tf_on.set_realtime_tail(true, 1000);
    tf_on.run(bars.data(), 10, "1", "1");
    CHECK(!tf_on.islast.back());
    CHECK(!tf_on.islastbar.back());                  // bucket rule: next minute is in a 24x7 session
    for (int i = 0; i + 1 < 10; ++i) {
        CHECK(tf_on.islast[i] == tf_off.islast[i]);
        CHECK(tf_on.islastbar[i] == tf_off.islastbar[i]);   // interior untouched
    }
    CHECK(tf_on.last_index.back() == 999);           // freeze survives run_tf_impl's own assignment
    CHECK(tf_on.last_time == 999LL * 60'000LL);
    ReportC r_tf{};
    tf_on.fill_report(&r_tf);
    CHECK(r_tf.trades_len == 0);                     // range-end guard on this path too
    BacktestEngine::free_report(&r_tf);

    // --- final-rereview.md N2: gapped script-TF feed, H <= n (exact) -----
    // Single-TF path: bars IS the script-bar array, so the exact rule
    // applies. bars_1m_gapped_after_5() has a 4-minute step between bars 5
    // and 6, so the exact bars[H-1] timestamp differs from what the pre-fix
    // "extrapolate from bars[0]" formula would have produced -- the gapless
    // feed above cannot discriminate the two formulas, this one does.
    {
        const auto gapped = bars_1m_gapped_after_5();
        HoldStrategy exact;
        exact.set_realtime_tail(true, 9);             // H = 9 <= n = 12
        exact.run(gapped.data(), (int)gapped.size());
        CHECK(exact.last_index.back() == 8);
        CHECK(exact.last_time == gapped[8].timestamp);                    // exact
        const int64_t pre_fix_value = gapped[0].timestamp + 8LL * 60'000LL;
        CHECK(exact.last_time != pre_fix_value);       // discriminates old vs. new formula
    }

    // --- final-rereview.md N2: gapped script-TF feed, H > n (extrapolate) -
    {
        const auto gapped = bars_1m_gapped_after_5();
        HoldStrategy extrap;
        extrap.set_realtime_tail(true, 20);            // H = 20 > n = 12
        extrap.run(gapped.data(), (int)gapped.size());
        CHECK(extrap.last_index.back() == 19);
        CHECK(extrap.last_time
              == gapped.back().timestamp + 8LL * 60'000LL);  // extrapolated from bars[n-1]
    }

    // --- final-rereview.md N1/N2: aggregated (1m -> 5m) tail ---------------
    // Under needs_aggregation, apply_realtime_tail_horizon's `bars` argument
    // is the *input* array, not script bars, so it must extrapolate from
    // the first *input* bar's timestamp rather than indexing input bars by
    // a script-bar horizon.
    {
        const auto bars_agg = bars_1m(15);             // 15 one-minute input bars -> 3 5m script bars
        HoldStrategy agg;
        agg.set_realtime_tail(true, 10);                // H = 10, well past expected_script_bars == 3
        agg.run(bars_agg.data(), (int)bars_agg.size(), "1", "5");
        CHECK(agg.last_index.back() == 9);                                       // H - 1
        CHECK(agg.last_time == bars_agg[0].timestamp + 9LL * 300'000LL);         // extrapolated from first input bar
    }

    return failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
