#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>

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

class SplitFeedProbe final : public BacktestEngine {
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

    void on_bar(const Bar& bar) override {
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


class OvernightLowerTfProbe final : public BacktestEngine {
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

    void on_bar(const Bar&) override { chart_array = current; }
};


class RoutingOnlyProbe final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
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
    // Completion-aware security publication receives exactly the final
    // auxiliary event of each native chart slice through the neutral bridge.
    assert((probe.completion_publishes_at_chart_close
            == std::vector<int>{1, 2, 3}));

    // Orders created on chart bars fill at the next native chart opens. If
    // the auxiliary feed contaminated the broker, these would be 20/30.
    assert(probe.trade_count() == 1);
    assert(near(probe.get_trade(0).entry_price, 200.0));
    assert(near(probe.get_trade(0).exit_price, 300.0));
    assert(probe.get_trade(0).entry_bar_index == 1);
    assert(probe.get_trade(0).exit_bar_index == 2);
}


void test_aux_label_inside_native_span_without_chart_bar_fails() {
    constexpr int64_t day1 = 1704205800000;
    constexpr int64_t day2 = 1704292200000;
    constexpr int64_t day3 = 1704378600000;
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.0, 10.0, day1},
        {300.0, 301.0, 299.0, 300.0, 10.0, day3},
    };
    const Bar aux[] = {
        {1.0, 1.0, 1.0, 1.0, 1.0, day1},
        {2.0, 2.0, 2.0, 2.0, 1.0, day2},
        {3.0, 3.0, 3.0, 3.0, 1.0, day3},
    };

    RoutingOnlyProbe probe;
    strategy_set_syminfo_timezone(
        static_cast<pf_strategy_t>(&probe), "America/New_York");
    strategy_set_syminfo_session(
        static_cast<pf_strategy_t>(&probe), "0930-1600:23456");
    assert(strategy_set_aux_security_feed(
        static_cast<pf_strategy_t>(&probe),
        reinterpret_cast<const pf_bar_t*>(aux), 3, "1") == 0);

    probe.run(chart, 2, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().find(
        "does not map to a native chart bar") != std::string::npos);
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
    test_aux_label_inside_native_span_without_chart_bar_fails();
    test_overnight_daily_lower_tf_array_does_not_split_at_utc_midnight();
    return 0;
}
