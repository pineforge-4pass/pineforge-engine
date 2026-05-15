// Tests for the bucket-aware dispatch in
// ``feed_security_eval_state``'s ``lower_tf_use_input`` (input
// passthrough) branch. The original count-based dispatch flushed the
// LTF buffer when ``buffer.size() == chunk_size``, which can leak one
// or more bars across script-TF chart-bar boundaries when the input
// feed has gaps, warmup misalignment, or sparse data. The fix dispatches
// the buffer (possibly partial) when the next bar's wall-clock script-TF
// bucket differs from the buffered window's bucket, so each chart-bar
// window contains exactly the input bars belonging to that bucket.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

struct LtfBufferHarness : public BacktestEngine {
    std::vector<std::vector<double>> per_bar_closes;
    std::vector<std::vector<bool>> per_bar_bulls;
    std::vector<double> _ltf_close{};
    std::vector<bool> _ltf_bull{};

    explicit LtfBufferHarness(const char* requested_tf) {
        register_security_lower_tf_eval(0, requested_tf, "");
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        if (security_lower_tf_sub_bar_index(0) == 0) {
            _ltf_close.clear();
            _ltf_bull.clear();
        }
        _ltf_close.push_back(bar.close);
        _ltf_bull.push_back(bar.close > bar.open);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        per_bar_closes.push_back(_ltf_close);
        per_bar_bulls.push_back(_ltf_bull);
    }
};

std::vector<Bar> make_minute_bars(int n, int64_t ts0_ms = 0) {
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double v = static_cast<double>(i + 1);
        out.push_back({v, v, v, v, 10.0,
                       ts0_ms + static_cast<int64_t>(i) * 60000});
    }
    return out;
}

std::vector<Bar> make_alternating_bull_bear_bars(int n, int64_t ts0_ms = 0) {
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double base = 100.0 + static_cast<double>(i);
        Bar b;
        if (i % 2 == 0) {
            b = {base, base + 1.0, base - 0.5, base + 0.8, 10.0,
                 ts0_ms + static_cast<int64_t>(i) * 60000};
        } else {
            b = {base, base + 0.5, base - 1.0, base - 0.8, 10.0,
                 ts0_ms + static_cast<int64_t>(i) * 60000};
        }
        out.push_back(b);
    }
    return out;
}

void test_gap_free_float_passthrough_no_leak() {
    LtfBufferHarness strat("1");
    auto bars = make_minute_bars(45);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_closes.size() == 3);
    for (int b = 0; b < 3; ++b) {
        const auto& arr = strat.per_bar_closes[static_cast<std::size_t>(b)];
        assert(arr.size() == 15);
        for (int i = 0; i < 15; ++i) {
            double expected = static_cast<double>(b * 15 + i + 1);
            assert(std::abs(arr[static_cast<std::size_t>(i)] - expected) < 1e-9);
        }
    }
    std::cout << "test_gap_free_float_passthrough_no_leak passed.\n";
}

void test_bool_passthrough_alternating_no_leak() {
    LtfBufferHarness strat("1");
    auto bars = make_alternating_bull_bear_bars(30);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_bulls.size() == 2);

    auto count_true = [](const std::vector<bool>& v) {
        int n = 0;
        for (bool x : v) if (x) ++n;
        return n;
    };

    const auto& b0 = strat.per_bar_bulls[0];
    const auto& b1 = strat.per_bar_bulls[1];
    assert(b0.size() == 15);
    assert(b1.size() == 15);
    int bulls0 = count_true(b0);
    int bulls1 = count_true(b1);
    assert(bulls0 == 8);
    assert(bulls1 == 7);
    assert(bulls1 != 8);
    std::cout << "test_bool_passthrough_alternating_no_leak passed (bulls0="
              << bulls0 << ", bulls1=" << bulls1 << ").\n";
}

void test_aggregated_passthrough_no_leak() {
    LtfBufferHarness strat("5");
    auto bars = make_minute_bars(30);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());
    assert(strat.per_bar_closes.size() == 2);
    for (const auto& arr : strat.per_bar_closes) assert(arr.size() == 3);
    assert(std::abs(strat.per_bar_closes[0][0] - 5.0) < 1e-9);
    assert(std::abs(strat.per_bar_closes[0][1] - 10.0) < 1e-9);
    assert(std::abs(strat.per_bar_closes[0][2] - 15.0) < 1e-9);
    assert(std::abs(strat.per_bar_closes[1][0] - 20.0) < 1e-9);
    assert(std::abs(strat.per_bar_closes[1][1] - 25.0) < 1e-9);
    assert(std::abs(strat.per_bar_closes[1][2] - 30.0) < 1e-9);
    std::cout << "test_aggregated_passthrough_no_leak passed.\n";
}

}  // namespace

int main() {
    test_gap_free_float_passthrough_no_leak();
    test_bool_passthrough_alternating_no_leak();
    test_aggregated_passthrough_no_leak();
    std::cout << "All test_ltf_buffer_no_leak tests passed.\n";
    return 0;
}
