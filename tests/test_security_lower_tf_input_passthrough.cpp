// Tests for request.security_lower_tf input-passthrough path: when the
// requested TF is >= input_tf and < script_tf the engine returns the
// real input bars (raw or aggregated) instead of synthesising sub-bars
// from the parent chart bar. Covers the four cases enumerated in the
// implementation plan.
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

struct InputPassthroughHarness : public BacktestEngine {
    std::vector<double> _ltf_close{};
    std::vector<std::vector<double>> per_bar_arrays;
    std::vector<int> per_dispatch_indices;

    explicit InputPassthroughHarness(const char* requested_tf) {
        register_security_lower_tf_eval(0, requested_tf, "");
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        per_dispatch_indices.push_back(security_lower_tf_sub_bar_index(0));
        if (security_lower_tf_sub_bar_index(0) == 0) {
            _ltf_close.clear();
        }
        _ltf_close.push_back(bar.close);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        per_bar_arrays.push_back(_ltf_close);
    }
};

std::vector<Bar> make_minute_bars(int n) {
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double v = static_cast<double>(i + 1);
        out.push_back({v, v, v, v, 10.0, static_cast<int64_t>(i) * 60000});
    }
    return out;
}

void test_req_equals_input_raw_passthrough() {
    InputPassthroughHarness strat("1");
    auto bars = make_minute_bars(30);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_arrays.size() == 2);
    for (const auto& arr : strat.per_bar_arrays) assert(arr.size() == 15);
    for (int i = 0; i < 15; ++i)
        assert(std::abs(strat.per_bar_arrays[0][static_cast<std::size_t>(i)]
                        - static_cast<double>(i + 1)) < 1e-9);
    for (int i = 0; i < 15; ++i)
        assert(std::abs(strat.per_bar_arrays[1][static_cast<std::size_t>(i)]
                        - static_cast<double>(i + 16)) < 1e-9);
    std::cout << "test_req_equals_input_raw_passthrough passed.\n";
}

void test_req_between_input_and_script_aggregated() {
    InputPassthroughHarness strat("5");
    auto bars = make_minute_bars(30);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_arrays.size() == 2);
    for (const auto& arr : strat.per_bar_arrays) assert(arr.size() == 3);
    assert(std::abs(strat.per_bar_arrays[0][0] - 5.0) < 1e-9);
    assert(std::abs(strat.per_bar_arrays[0][1] - 10.0) < 1e-9);
    assert(std::abs(strat.per_bar_arrays[0][2] - 15.0) < 1e-9);
    assert(std::abs(strat.per_bar_arrays[1][0] - 20.0) < 1e-9);
    assert(std::abs(strat.per_bar_arrays[1][1] - 25.0) < 1e-9);
    assert(std::abs(strat.per_bar_arrays[1][2] - 30.0) < 1e-9);
    assert(strat.per_dispatch_indices.size() == 6);
    for (int b = 0; b < 2; ++b)
        for (int s = 0; s < 3; ++s)
            assert(strat.per_dispatch_indices[
                static_cast<std::size_t>(b * 3 + s)] == s);
    std::cout << "test_req_between_input_and_script_aggregated passed.\n";
}

void test_req_equals_script_rejected() {
    InputPassthroughHarness strat("15");
    auto bars = make_minute_bars(30);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(!strat.last_error().empty());
    assert(strat.last_error().find(
        "not finer than script timeframe") != std::string::npos);
    std::cout << "test_req_equals_script_rejected passed.\n";
}

void test_req_below_input_uses_synthesis() {
    InputPassthroughHarness strat("1");
    std::vector<Bar> bars;
    for (int i = 0; i < 2; ++i) {
        double v = 100.0 + i;
        bars.push_back({v, v + 5, v - 5, v + 2, 100.0,
                        static_cast<int64_t>(i) * 900000});
    }
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_arrays.size() == 2);
    for (const auto& arr : strat.per_bar_arrays) assert(arr.size() == 15);
    std::cout << "test_req_below_input_uses_synthesis passed.\n";
}

void test_req_non_divisor_of_script_rejected() {
    InputPassthroughHarness strat("7");
    auto bars = make_minute_bars(120);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "60", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(!strat.last_error().empty());
    assert(strat.last_error().find(
        "not an integer divisor of script timeframe") != std::string::npos);
    std::cout << "test_req_non_divisor_of_script_rejected passed.\n";
}

}  // namespace

int main() {
    test_req_equals_input_raw_passthrough();
    test_req_between_input_and_script_aggregated();
    test_req_equals_script_rejected();
    test_req_below_input_uses_synthesis();
    test_req_non_divisor_of_script_rejected();
    std::cout << "All test_security_lower_tf_input_passthrough tests passed.\n";
    return 0;
}
