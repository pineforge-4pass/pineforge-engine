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

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "auxiliary security feed test requires the V1 feature probe"
#endif

namespace {

class SplitFeedProbe final : public pineforge::source::PineStrategyHost {
public:
    std::vector<int> chart_indexes;
    std::vector<double> chart_closes;
    std::vector<double> security_closes;
    std::vector<double> security_at_chart_close;
    std::vector<double> lower_tf_current;
    std::vector<std::vector<double>> lower_tf_at_chart_close;
    std::vector<int> completion_publishes_at_chart_close;
    int completion_publish_count = 0;
    double latest_security_close = na<double>();

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        // The generated form still passes input_tf_ here. The runtime must
        // redirect registration to the installed auxiliary TF.
        register_security_eval(0, "1", input_tf_, false, false);
        register_security_lower_tf_eval(1, "1", input_tf_);
        register_security_eval(2, "1", input_tf_, true, false);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        if (sec_id == 0) {
            if (!is_complete) return;
            latest_security_close = bar.close;
            security_closes.push_back(bar.close);
        } else if (sec_id == 1) {
            if (!is_complete) return;
            if (security_lower_tf_sub_bar_index(1) == 0) {
                lower_tf_current.clear();
            }
            lower_tf_current.push_back(bar.close);
        } else if (sec_id == 2 && is_complete) {
            completion_publish_count++;
        }
    }

    void on_source_bar(const Bar& bar) override {
        chart_indexes.push_back(bar_index_);
        chart_closes.push_back(bar.close);
        security_at_chart_close.push_back(latest_security_close);
        lower_tf_at_chart_close.push_back(lower_tf_current);
        completion_publishes_at_chart_close.push_back(
            completion_publish_count);
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close_all();
    }

};


class OvernightLowerTfProbe final : public pineforge::source::PineStrategyHost {
public:
    std::vector<double> current;
    std::vector<double> chart_array;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(0, "1", input_tf_);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        if (security_lower_tf_sub_bar_index(0) == 0) current.clear();
        current.push_back(bar.close);
    }

    void on_source_bar(const Bar&) override { chart_array = current; }
};


class RoutingOnlyProbe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
};

bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

void test_native_chart_and_auxiliary_security_are_isolated() {
    constexpr int64_t day1 = 1704205800000;  // 2024-01-02 09:30 America/New_York
    constexpr int64_t day2 = 1704292200000;
    constexpr int64_t day3 = 1704378600000;
    constexpr int64_t day = 86400000;
    constexpr int64_t minute = 60000;

    const Bar chart[] = {
        {100.0, 160.0, 90.0, 150.0, 1000.0, day1},
        {200.0, 260.0, 190.0, 250.0, 2000.0, day2},
        {300.0, 360.0, 290.0, 350.0, 3000.0, day3},
    };
    const Bar aux[] = {
        {90.0, 90.0, 90.0, 90.0, 1.0, day1 - day},
        {10.0, 11.5, 9.5, 11.0, 10.0, day1},
        {11.0, 12.5, 10.5, 12.0, 11.0, day1 + minute},
        {20.0, 21.5, 19.5, 21.0, 20.0, day2},
        {21.0, 22.5, 20.5, 22.0, 21.0, day2 + minute},
        {30.0, 31.5, 29.5, 31.0, 30.0, day3},
        {31.0, 32.5, 30.5, 32.0, 31.0, day3 + minute},
        {80.0, 80.0, 80.0, 80.0, 1.0, day3 + day},
    };

    SplitFeedProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600:23456");
    const int installed = strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 8, "1");
    assert(installed == 0);

    probe.run(chart, 3, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());

    assert((probe.chart_indexes == std::vector<int>{0, 1, 2}));
    assert((probe.chart_closes == std::vector<double>{150.0, 250.0, 350.0}));
    assert((probe.security_closes
            == std::vector<double>{11.0, 12.0, 21.0, 22.0, 31.0, 32.0}));
    assert((probe.security_at_chart_close
            == std::vector<double>{12.0, 22.0, 32.0}));
    assert((probe.lower_tf_at_chart_close
            == std::vector<std::vector<double>>{
                {11.0, 12.0}, {21.0, 22.0}, {31.0, 32.0}}));
    // A finer lookahead_on request reads the calling bar's FIRST intrabar
    // (calling_open_latches_first, round 7): every completed requested bar
    // is published and the chart body runs right after the slice's first
    // one -- 1 on day 1, then day 1's second + day 2's first = 3, then 5.
    // (Until round 7 the gate published the slice's LAST completion only:
    // 1, 2, 3.)
    assert((probe.completion_publishes_at_chart_close
            == std::vector<int>{1, 3, 5}));
    assert(probe.completion_publish_count == 6);

    // Orders created on chart bars fill at the next native chart opens. If
    // the auxiliary feed contaminated the broker, these would be 20/30.
    assert(probe.trade_count() == 1);
    assert(near(probe.get_trade(0).entry_price, 200.0));
    assert(near(probe.get_trade(0).exit_price, 300.0));
    assert(probe.get_trade(0).entry_bar_index == 1);
    assert(probe.get_trade(0).exit_bar_index == 2);
}


// The harness bounds the CHART feed at TradingView's range end
// (run_strategy.py _load_tv_range_end_ms: the bars opening at or before the
// tape's metrics.json `to`, 2026-05-01 00:00 UTC on every campaign lane) and
// leaves the finer auxiliary feed as exported -- on the ETH lane the 1m
// FEED_1M runs on to 05-04 15:00 UTC while the chart now ends at 05-01
// 00:00. The tail prefilter treats aux bars labelled past the last chart
// bar as inert coverage: the last chart bar keeps its full slice, no chart
// bar is dropped, and nothing errors. Pin that on the intraday (15m chart,
// 1m aux) shape, since the calendar shape above already carries a trailing
// aux day (day3 + day).
void test_intraday_aux_feed_running_past_the_chart_range_end_is_inert() {
    constexpr int64_t range_end = 1777593600000;  // 2026-05-01 00:00 UTC
    constexpr int64_t minute = 60000;
    constexpr int64_t quarter = 15 * minute;
    // Chart: the two 15m bars before the range end and the one opening at
    // it -- the last bar the harness keeps.
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.0, 10.0, range_end - 2 * quarter},
        {200.0, 201.0, 199.0, 200.0, 10.0, range_end - quarter},
        {300.0, 301.0, 299.0, 300.0, 10.0, range_end},
    };
    // Aux: every minute of those three bars (45), then 165 more minutes
    // past the last chart bar's span that the chart never sees.
    std::vector<Bar> aux;
    for (int64_t ts = range_end - 2 * quarter; ts < range_end + 180 * minute;
         ts += minute) {
        const double v = static_cast<double>((ts - (range_end - 2 * quarter))
                                             / minute);
        aux.push_back({v, v, v, v, 1.0, ts});
    }
    assert(aux.size() == 45 + 165);

    SplitFeedProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "UTC");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0000-0000:1234567");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux.data()),
        static_cast<int>(aux.size()), "1") == 0);

    probe.run(chart, 3, "15", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());

    // Every chart bar dispatched, the last one on the range end itself.
    assert((probe.chart_indexes == std::vector<int>{0, 1, 2}));
    assert((probe.chart_closes == std::vector<double>{100.0, 200.0, 300.0}));
    // Each chart bar's lower-tf array is exactly its own 15 aux minutes;
    // the last bar's slice is the 15 minutes from the range end, and the
    // 165 trailing aux bars reach no chart bar.
    assert(probe.lower_tf_at_chart_close.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        assert(probe.lower_tf_at_chart_close[i].size() == 15);
        assert(near(probe.lower_tf_at_chart_close[i].front(),
                    static_cast<double>(15 * i)));
        assert(near(probe.lower_tf_at_chart_close[i].back(),
                    static_cast<double>(15 * i + 14)));
    }
    // The security value the last chart bar reads is the last aux minute
    // INSIDE it (44), not anything from the trailing coverage (45..209).
    assert((probe.security_at_chart_close
            == std::vector<double>{14.0, 29.0, 44.0}));
    assert(probe.security_closes.size() == 45);
    assert(near(probe.security_closes.back(), 44.0));
}


void test_intraday_aux_label_inside_native_span_without_chart_bar_fails() {
    constexpr int64_t bar1 = 1704205800000;  // 2024-01-02 09:30 New York
    constexpr int64_t hour = 3600000;
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.0, 10.0, bar1},
        {300.0, 301.0, 299.0, 300.0, 10.0, bar1 + 2 * hour},
    };
    const Bar aux[] = {
        {1.0, 1.0, 1.0, 1.0, 1.0, bar1},
        {2.0, 2.0, 2.0, 2.0, 1.0, bar1 + hour},
        {3.0, 3.0, 3.0, 3.0, 1.0, bar1 + 2 * hour},
    };

    RoutingOnlyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600:23456");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 3, "1") == 0);

    probe.run(chart, 2, "60", "60", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().find(
        "does not map to a native chart bar") != std::string::npos);
}


void test_nifty_muhurat_shifted_open_maps_by_trading_date() {
    constexpr int64_t regular_day = 1635911100000;  // 2021-11-03 09:15 IST
    constexpr int64_t muhurat_native = 1636029000000;  // 2021-11-04 18:00 IST
    constexpr int64_t next_regular_day = 1636343100000;  // 2021-11-08 09:15 IST
    constexpr int64_t minute = 60000;
    const Bar chart[] = {
        {100.0, 160.0, 90.0, 150.0, 1000.0, regular_day},
        {200.0, 260.0, 190.0, 250.0, 2000.0, muhurat_native},
        {300.0, 360.0, 290.0, 350.0, 3000.0, next_regular_day},
    };
    const Bar aux[] = {
        {10.0, 11.5, 9.5, 11.0, 10.0, regular_day},
        {11.0, 12.5, 10.5, 12.0, 11.0, regular_day + minute},
        // The immutable NSE tape starts seven minutes after the native
        // Muhurat chart label.  Both belong to the same trading date even
        // though neither timestamp is the configured 09:15 session open.
        {20.0, 21.5, 19.5, 21.0, 20.0, 1636029420000},
        {21.0, 22.5, 20.5, 22.0, 21.0, 1636033620000},
        {30.0, 31.5, 29.5, 31.0, 30.0, next_regular_day},
        {31.0, 32.5, 30.5, 32.0, 31.0, next_regular_day + minute},
    };

    SplitFeedProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "Asia/Kolkata");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0915-1530:23456");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 6, "1") == 0);

    probe.run(chart, 3, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.chart_closes == std::vector<double>{150.0, 250.0, 350.0}));
    assert((probe.security_at_chart_close
            == std::vector<double>{12.0, 22.0, 32.0}));
    assert((probe.lower_tf_at_chart_close
            == std::vector<std::vector<double>>{
                {11.0, 12.0}, {21.0, 22.0}, {31.0, 32.0}}));
}


void test_nq_labor_day_sessions_coalesce_into_native_interval() {
    constexpr int64_t sunday_native = 1693778400000;  // 2023-09-03 17:00 CDT
    constexpr int64_t labor_reopen = 1693864800000;  // 2023-09-04 17:00 CDT
    constexpr int64_t tuesday_native = 1693951200000;  // 2023-09-05 17:00 CDT
    constexpr int64_t minute = 60000;
    const Bar chart[] = {
        {100.0, 160.0, 90.0, 150.0, 1000.0, sunday_native},
        {300.0, 360.0, 290.0, 350.0, 3000.0, tuesday_native},
    };
    const Bar aux[] = {
        {10.0, 11.5, 9.5, 11.0, 10.0, sunday_native},
        {11.0, 12.5, 10.5, 12.0, 11.0, sunday_native + minute},
        // TradingView's native Labor-Day candle legitimately coalesces the
        // Sunday session and Monday-evening reopen under sunday_native.
        {20.0, 21.5, 19.5, 21.0, 20.0, labor_reopen},
        {21.0, 22.5, 20.5, 22.0, 21.0, labor_reopen + minute},
        {30.0, 31.5, 29.5, 31.0, 30.0, tuesday_native},
        {31.0, 32.5, 30.5, 32.0, 31.0, tuesday_native + minute},
    };

    SplitFeedProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/Chicago");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "1700-1600:23456");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 6, "1") == 0);

    probe.run(chart, 2, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.chart_closes == std::vector<double>{150.0, 350.0}));
    assert((probe.security_at_chart_close == std::vector<double>{22.0, 32.0}));
    assert((probe.lower_tf_at_chart_close
            == std::vector<std::vector<double>>{
                {11.0, 12.0, 21.0, 22.0}, {31.0, 32.0}}));
}


void test_oanda_break_stamped_daily_bars_route_by_covered_session() {
    // OANDA XAUUSD: session 1800-1700 ET, but the immutable daily tape stamps
    // every bar at 17:00 ET -- inside the inter-session break, one hour BEFORE
    // the session the bar covers.  Keying the stamp by session-day floor maps
    // it to the PREVIOUS session, so the last chart bar's content rows read as
    // beyond last_chart_key and the tail prefilter empties the final bar.
    constexpr int64_t minute = 60000;
    constexpr int64_t stamp_a = 1704751200000;  // Mon 2024-01-08 17:00 EST
    constexpr int64_t open_a = 1704754800000;   // Mon 18:00 EST
    constexpr int64_t stamp_b = 1704837600000;  // Tue 17:00 EST
    constexpr int64_t open_b = 1704841200000;   // Tue 18:00 EST
    constexpr int64_t stamp_c = 1704924000000;  // Wed 17:00 EST
    constexpr int64_t open_c = 1704927600000;   // Wed 18:00 EST
    constexpr int64_t lead = 1704733200000;     // Mon 12:00 EST (prior session)
    constexpr int64_t trail = 1705014000000;    // Thu 18:00 EST (next session)
    const Bar chart[] = {
        {100.0, 160.0, 90.0, 150.0, 1000.0, stamp_a},
        {200.0, 260.0, 190.0, 250.0, 2000.0, stamp_b},
        {300.0, 360.0, 290.0, 350.0, 3000.0, stamp_c},
    };
    const Bar aux[] = {
        {90.0, 90.0, 90.0, 90.0, 1.0, lead},
        {10.0, 11.5, 9.5, 11.0, 10.0, open_a},
        {11.0, 12.5, 10.5, 12.0, 11.0, open_a + minute},
        {20.0, 21.5, 19.5, 21.0, 20.0, open_b},
        {21.0, 22.5, 20.5, 22.0, 21.0, open_b + minute},
        {30.0, 31.5, 29.5, 31.0, 30.0, open_c},
        {31.0, 32.5, 30.5, 32.0, 31.0, open_c + minute},
        {80.0, 80.0, 80.0, 80.0, 1.0, trail},
    };

    SplitFeedProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "1800-1700");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 8, "1") == 0);

    probe.run(chart, 3, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.chart_closes == std::vector<double>{150.0, 250.0, 350.0}));
    assert((probe.security_at_chart_close
            == std::vector<double>{12.0, 22.0, 32.0}));
    assert((probe.lower_tf_at_chart_close
            == std::vector<std::vector<double>>{
                {11.0, 12.0}, {21.0, 22.0}, {31.0, 32.0}}));
}


void test_overnight_daily_lower_tf_array_does_not_split_at_utc_midnight() {
    constexpr int64_t session_open = 1704232800000;  // 2024-01-02 17:00 NY
    constexpr int64_t minute = 60000;
    const Bar chart[] = {
        {100.0, 105.0, 95.0, 102.0, 1000.0, session_open},
    };
    const Bar aux[] = {
        {1.0, 1.0, 1.0, 1.0, 1.0, session_open},
        {2.0, 2.0, 2.0, 2.0, 1.0, session_open + 119 * minute},
        {3.0, 3.0, 3.0, 3.0, 1.0, session_open + 120 * minute},
        {4.0, 4.0, 4.0, 4.0, 1.0, session_open + 1439 * minute},
    };

    OvernightLowerTfProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "1700-1700:23456");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 4, "1") == 0);

    probe.run(chart, 1, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
    assert((probe.chart_array == std::vector<double>{1.0, 2.0, 3.0, 4.0}));
}

}  // namespace

int main() {
    test_native_chart_and_auxiliary_security_are_isolated();
    test_intraday_aux_feed_running_past_the_chart_range_end_is_inert();
    test_intraday_aux_label_inside_native_span_without_chart_bar_fails();
    test_nifty_muhurat_shifted_open_maps_by_trading_date();
    test_nq_labor_day_sessions_coalesce_into_native_interval();
    test_oanda_break_stamped_daily_bars_route_by_covered_session();
    test_overnight_daily_lower_tf_array_does_not_split_at_utc_midnight();
    return 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
