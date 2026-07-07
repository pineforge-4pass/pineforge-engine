#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <pineforge/engine.hpp>
#include <pineforge/series.hpp>
#include <pineforge/ta.hpp>

using namespace pineforge;

static void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MockSecurityStrategy : public BacktestEngine {
public:
    Series<double> _s_close;
    double _req_sec_0 = na<double>();
    double last_htf_val = na<double>();

    MockSecurityStrategy() {
        register_security_eval(0, "60", "15", false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        _req_sec_0 = bar.close;
        last_htf_val = _req_sec_0;
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        if (is_first_tick_) {
            _s_close.push(current_bar_.close);
        } else {
            _s_close.update(current_bar_.close);
        }
        if (!is_na(_req_sec_0)) {
            last_htf_val = _req_sec_0;
        }
    }
};

class LowerTimeframeSecurityHarness : public BacktestEngine {
public:
    LowerTimeframeSecurityHarness() {
        register_security_eval(0, "7", "", false, false);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

class LowerTimeframeEmulationHarness : public BacktestEngine {
public:
    double _req_sec_0 = na<double>();
    std::vector<int> completed_counts_per_input_bar;
    std::vector<double> completed_closes;

    LowerTimeframeEmulationHarness() {
        // Use the lower-TF API: regular request.security with a
        // finer-than-input TF is now rejected by the validator;
        // callers must opt in via register_security_lower_tf_eval.
        register_security_lower_tf_eval(0, "5", "");
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }

        if (static_cast<int>(completed_counts_per_input_bar.size()) <= bar_index_) {
            completed_counts_per_input_bar.resize(static_cast<std::size_t>(bar_index_) + 1, 0);
        }
        completed_counts_per_input_bar[static_cast<std::size_t>(bar_index_)]++;
        completed_closes.push_back(bar.close);
        _req_sec_0 = bar.close;
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

class LowerTimeframeUnsupportedFlagHarness : public BacktestEngine {
public:
    explicit LowerTimeframeUnsupportedFlagHarness(bool lookahead_on, bool gaps_on) {
        // Register the LTF-array path then override the flags so we
        // can prove LTF emulation rejects non-default lookahead/gaps.
        register_security_lower_tf_eval(0, "5", "");
        // The lower-TF-array helper pins flags off; flip them back on
        // here to drive the unsupported-flag rejection path.
        if (!security_eval_states_.empty()) {
            security_eval_states_.back().lookahead_on = lookahead_on;
            security_eval_states_.back().gaps_on = gaps_on;
        }
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

class HigherTimeframeUnknownInputHarness : public BacktestEngine {
public:
    HigherTimeframeUnknownInputHarness() {
        register_security_eval(0, "60", "", false, false);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

class HelperSecurityTaHarness : public BacktestEngine {
public:
    ta::EMA _ta_ema_1{3};
    ta::EMA _sec0__ta_ema_1{3};
    double _req_sec_0 = na<double>();
    double last_htf_ema = na<double>();
    double last_main_ema = na<double>();

    HelperSecurityTaHarness() {
        register_security_eval(0, "60", "15", false, false);
    }

    double f() {
        return _ta_ema_1.compute(current_bar_.close);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        _req_sec_0 = _sec0__ta_ema_1.compute(bar.close);
        last_htf_ema = _req_sec_0;
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        last_main_ema = f();
    }
};

void test_request_security_hook_dispatches_completed_values() {
    MockSecurityStrategy strat;

    std::vector<Bar> input_bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {20.0, 20.0, 20.0, 20.0, 100.0, 900000},
        {30.0, 30.0, 30.0, 30.0, 100.0, 1800000},
        {40.0, 40.0, 40.0, 40.0, 100.0, 2700000},
        {50.0, 50.0, 50.0, 50.0, 100.0, 3600000}
    };

    strat.run(input_bars.data(), input_bars.size(), "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);

    require(!is_na(strat.last_htf_val),
            "request.security hook should receive at least one completed value");
    require(std::abs(strat.last_htf_val - 40.0) < 1e-9,
            "request.security hook should receive the last completed higher-timeframe close");

    std::cout << "test_request_security_hook_dispatches_completed_values passed.\n";
}

void test_request_security_lower_tf_requires_finer_input_bars() {
    LowerTimeframeSecurityHarness strat;

    std::vector<Bar> input_bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {11.0, 11.0, 11.0, 11.0, 100.0, 900000},
        {12.0, 12.0, 12.0, 12.0, 100.0, 1800000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "15",
        "15",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );
    require(!strat.last_error().empty(),
            "Lower-TF request.security should fail without finer input bars");
    require(
        strat.last_error().find("Use request.security_lower_tf for sub-input timeframes")
            != std::string::npos,
        std::string("Unexpected lower-TF error: ") + strat.last_error()
    );
    std::cout << "test_request_security_lower_tf_requires_finer_input_bars passed.\n";
}

void test_request_security_emulates_ratio_divisible_lower_tf() {
    LowerTimeframeEmulationHarness strat;

    std::vector<Bar> input_bars = {
        {100.0, 110.0, 90.0, 105.0, 90.0, 0},
        {105.0, 120.0, 100.0, 115.0, 120.0, 900000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "15",
        "15",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );

    require(strat.completed_counts_per_input_bar.size() == input_bars.size(),
            "Lower-TF emulation should record one completed-count bucket per input bar");
    require(strat.completed_counts_per_input_bar[0] == 3,
            "5-from-15 lower-TF emulation should produce 3 completed evaluations on the first input bar");
    require(strat.completed_counts_per_input_bar[1] == 3,
            "5-from-15 lower-TF emulation should produce 3 completed evaluations on the second input bar");
    require(strat.completed_closes.size() == 6,
            "5-from-15 lower-TF emulation should emit one completed close per synthetic sub-bar");
    require(std::abs(strat.completed_closes.front() - 90.0) < 1e-9,
            "Lower-TF emulation should follow the sampled path ordering for the first synthetic close");
    require(std::abs(strat.completed_closes.back() - 115.0) < 1e-9,
            "Lower-TF emulation should end on the parent input bar close");
    require(std::abs(strat._req_sec_0 - 115.0) < 1e-9,
            "Lower-TF request.security value should reflect the last completed synthetic sub-bar");

    ReportC report{};
    strat.fill_report(&report);
    require(report.security_diag_len == 1,
            "Lower-TF emulation should expose diagnostics for the security evaluator");
    require(report.security_diag[0].feed_count == 6,
            "Lower-TF emulation should feed one synthetic bar per 5-minute slice");
    require(report.security_diag[0].eval_complete_count == 6,
            "Lower-TF emulation should evaluate every synthetic bar as complete");
    require(report.security_diag[0].eval_partial_count == 0,
            "Lower-TF emulation should not emit partial lower-TF evaluations");
    BacktestEngine::free_report(&report);

    std::cout << "test_request_security_emulates_ratio_divisible_lower_tf passed.\n";
}

void test_request_security_lower_tf_emulation_rejects_unsupported_flags() {
    struct TestCase {
        bool lookahead_on;
        bool gaps_on;
        const char* label;
    };
    std::vector<TestCase> cases = {
        {true, false, "lookahead_on"},
        {false, true, "gaps_on"},
    };

    std::vector<Bar> input_bars = {
        {100.0, 110.0, 90.0, 105.0, 90.0, 0},
        {105.0, 120.0, 100.0, 115.0, 120.0, 900000},
    };

    for (const auto& test_case : cases) {
        LowerTimeframeUnsupportedFlagHarness strat(
            test_case.lookahead_on,
            test_case.gaps_on
        );

        strat.run(
            input_bars.data(),
            static_cast<int>(input_bars.size()),
            "15",
            "15",
            false,
            4,
            MagnifierDistribution::ENDPOINTS
        );
        require(
            !strat.last_error().empty(),
            std::string("Lower-TF request.security should reject unsupported emulation flags for ")
                + test_case.label
        );
        require(
            strat.last_error().find(
                "request.security lower TF emulation only supports lookahead=barmerge.lookahead_off and gaps=barmerge.gaps_off"
            ) != std::string::npos,
            std::string("Unexpected lower-TF unsupported-flag error for ")
                + test_case.label + ": " + strat.last_error()
        );
    }

    std::cout << "test_request_security_lower_tf_emulation_rejects_unsupported_flags passed.\n";
}

void test_request_security_higher_tf_requires_inferable_input_tf() {
    HigherTimeframeUnknownInputHarness strat;

    std::vector<Bar> input_bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "",
        "",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );
    require(
        !strat.last_error().empty(),
        "Higher-TF request.security should fail with an inference diagnostic when input_tf is unknown"
    );
    require(
        strat.last_error().find("request.security cannot infer input timeframe")
            != std::string::npos,
        std::string("Unexpected unknown-input-TF error: ") + strat.last_error()
    );
    std::cout << "test_request_security_higher_tf_requires_inferable_input_tf passed.\n";
}

// --- request.security_lower_tf harnesses ---
//
// These exercise the runtime contract used by the codegen lowering of
// ``request.security_lower_tf``: the engine resets
// ``lower_tf_sub_bar_index`` at the start of each chart bar's
// synthesis loop and increments it after every per-sub-bar dispatch
// so the codegen can detect index 0 and clear its accumulator vector.
class LowerTfArraySecurityHarness : public BacktestEngine {
public:
    std::vector<double> _req_sec_lower_tf_0{};
    std::vector<std::vector<double>> per_bar_arrays;
    std::vector<int> per_dispatch_indices;

    LowerTfArraySecurityHarness(const char* requested_tf) {
        register_security_lower_tf_eval(0, requested_tf, "");
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        per_dispatch_indices.push_back(security_lower_tf_sub_bar_index(0));
        if (security_lower_tf_sub_bar_index(0) == 0) {
            _req_sec_lower_tf_0.clear();
        }
        _req_sec_lower_tf_0.push_back(bar.close);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        per_bar_arrays.push_back(_req_sec_lower_tf_0);
    }
};

class LowerTfArrayUnsupportedTfHarness : public BacktestEngine {
public:
    LowerTfArrayUnsupportedTfHarness(const char* requested_tf) {
        register_security_lower_tf_eval(0, requested_tf, "");
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

void test_request_security_lower_tf_array_5m_chart_with_1m_emulation() {
    LowerTfArraySecurityHarness strat("1");

    std::vector<Bar> input_bars = {
        {100.0, 110.0, 90.0, 105.0, 100.0, 0},
        {105.0, 120.0, 100.0, 115.0, 100.0, 300000},
        {115.0, 130.0, 110.0, 125.0, 100.0, 600000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "5",
        "5",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );

    require(strat.per_bar_arrays.size() == 3,
            "Expected one captured array per chart bar");
    for (std::size_t i = 0; i < strat.per_bar_arrays.size(); ++i) {
        require(strat.per_bar_arrays[i].size() == 5,
                "5m chart with 1m lower-TF must produce 5 elements per chart bar");
    }

    require(strat.per_dispatch_indices.size() == 15,
            "Three chart bars * five sub-bars must produce 15 dispatches");
    for (int b = 0; b < 3; ++b) {
        for (int s = 0; s < 5; ++s) {
            int observed = strat.per_dispatch_indices[static_cast<std::size_t>(b * 5 + s)];
            require(observed == s,
                    "lower_tf_sub_bar_index must walk 0..ratio-1 per chart bar");
        }
    }

    // Bar 0: synthetic close path ends on the parent close (105) per
    // ``synthesize_lower_tf_bars`` semantics. Verifying the last element
    // matches the parent close gives a cheap chronological-ordering
    // sanity check (earliest -> latest within the chart bar).
    require(std::abs(strat.per_bar_arrays[0].back() - 105.0) < 1e-9,
            "Last lower-TF close of chart bar 0 must equal parent close");
    require(std::abs(strat.per_bar_arrays[1].back() - 115.0) < 1e-9,
            "Last lower-TF close of chart bar 1 must equal parent close");
    require(std::abs(strat.per_bar_arrays[2].back() - 125.0) < 1e-9,
            "Last lower-TF close of chart bar 2 must equal parent close");

    std::cout << "test_request_security_lower_tf_array_5m_chart_with_1m_emulation passed.\n";
}

void test_request_security_lower_tf_array_60m_chart_with_1m_emulation() {
    LowerTfArraySecurityHarness strat("1");

    std::vector<Bar> input_bars;
    for (int i = 0; i < 2; ++i) {
        input_bars.push_back({
            100.0 + i,
            110.0 + i,
            90.0 + i,
            105.0 + i,
            100.0,
            static_cast<int64_t>(i) * 3600000
        });
    }

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "60",
        "60",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );

    require(strat.per_bar_arrays.size() == 2,
            "Expected one captured array per 60m chart bar");
    for (std::size_t i = 0; i < strat.per_bar_arrays.size(); ++i) {
        require(strat.per_bar_arrays[i].size() == 60,
                "60m chart with 1m lower-TF must produce 60 elements per chart bar");
    }

    std::cout << "test_request_security_lower_tf_array_60m_chart_with_1m_emulation passed.\n";
}

void test_request_security_lower_tf_array_rejects_higher_timeframe() {
    LowerTfArrayUnsupportedTfHarness strat("60");

    std::vector<Bar> input_bars = {
        {100.0, 110.0, 90.0, 105.0, 100.0, 0},
        {105.0, 120.0, 100.0, 115.0, 100.0, 300000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "5",
        "5",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );
    require(
        !strat.last_error().empty(),
        "request.security_lower_tf with a coarser timeframe than the chart should raise"
    );
    require(
        strat.last_error().find(
            "Lower-TF API requires a strictly finer timeframe"
        ) != std::string::npos,
        std::string("Unexpected higher-TF lower-TF-array error: ") + strat.last_error()
    );
    std::cout << "test_request_security_lower_tf_array_rejects_higher_timeframe passed.\n";
}

void test_request_security_lower_tf_array_rejects_non_divisible_timeframe() {
    LowerTfArrayUnsupportedTfHarness strat("7");

    std::vector<Bar> input_bars = {
        {100.0, 110.0, 90.0, 105.0, 100.0, 0},
        {105.0, 120.0, 100.0, 115.0, 100.0, 900000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "15",
        "15",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );
    require(
        !strat.last_error().empty(),
        "request.security_lower_tf with a non-divisor timeframe should raise"
    );
    require(
        strat.last_error().find(
            "is not an integer divisor of input"
        ) != std::string::npos,
        std::string("Unexpected non-divisible lower-TF-array error: ")
            + strat.last_error()
    );
    std::cout << "test_request_security_lower_tf_array_rejects_non_divisible_timeframe passed.\n";
}

void test_request_security_helper_ta_uses_security_local_state() {
    HelperSecurityTaHarness strat;

    std::vector<Bar> input_bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {20.0, 20.0, 20.0, 20.0, 100.0, 900000},
        {30.0, 30.0, 30.0, 30.0, 100.0, 1800000},
        {40.0, 40.0, 40.0, 40.0, 100.0, 2700000},
        {50.0, 50.0, 50.0, 50.0, 100.0, 3600000},
        {60.0, 60.0, 60.0, 60.0, 100.0, 4500000},
        {70.0, 70.0, 70.0, 70.0, 100.0, 5400000},
        {80.0, 80.0, 80.0, 80.0, 100.0, 6300000},
    };

    strat.run(
        input_bars.data(),
        static_cast<int>(input_bars.size()),
        "15",
        "15",
        false,
        4,
        MagnifierDistribution::ENDPOINTS
    );

    ta::EMA expected_htf(3);
    double expected = expected_htf.compute(40.0);
    expected = expected_htf.compute(80.0);

    ta::EMA wrong_main(3);
    double wrong = na<double>();
    for (const auto& bar : input_bars) {
        wrong = wrong_main.compute(bar.close);
    }

    require(!is_na(strat.last_htf_ema),
            "TA-bearing request.security helper should produce a concrete higher-timeframe result");
    require(std::abs(strat.last_htf_ema - expected) < 1e-9,
            "TA-bearing request.security helper should follow security-local TA state");
    require(std::abs(strat.last_htf_ema - wrong) > 1e-6,
            "Higher-timeframe helper TA should not collapse to the main-context TA state");

    std::cout << "test_request_security_helper_ta_uses_security_local_state passed.\n";
}

// Plain request.security with a target TF strictly finer than script_tf
// (e.g. "5" on a 15m chart, fed from 1m input bars) completes its own
// aggregation R = script/requested times per calling bar. The publish gate
// (SecurityEvalState::publish_gate_tf_seconds) must latch the is_complete
// flag to calling-bar boundaries ONLY under lookahead_on (TV merges the
// FIRST intrabar of each calling bar there); under lookahead_off TV merges
// the LAST intrabar, so every finer-period completion must publish
// unchanged. Regression coverage for both sides:
//   - gated lookahead_off broke masayanfx-multi-time-score-strategy
//     (ta.highest(high,20)[1], lookahead_off, "5" on 15m): 100.0% -> 93.7%
//   - ungated lookahead_on broke 3commas triple-RSI DCA
//     (ta.rsi(close,7)[1], lookahead_on, "5" on 15m): 100.0% -> 50.5%
class FinerTfPublishGateHarness : public BacktestEngine {
public:
    std::vector<int64_t> published_ts;  // bucket-start ts of is_complete evals

    explicit FinerTfPublishGateHarness(bool lookahead_on) {
        register_security_eval(0, "5", "1", lookahead_on, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        published_ts.push_back(bar.timestamp);
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
    }
};

void test_request_security_finer_tf_publish_gate_is_lookahead_only() {
    // 30 one-minute bars = two 15m calling bars = six 5m security buckets
    // starting at 0, 300k, 600k, 900k, 1.2M, 1.5M ms. Bucket ends aligned
    // to the 15m (900s) script boundary: 600k (ends 900s) and 1.5M
    // (ends 1800s).
    std::vector<Bar> input_bars;
    for (int i = 0; i < 30; ++i) {
        double px = 100.0 + i;
        input_bars.push_back(
            {px, px + 1.0, px - 1.0, px + 0.5, 10.0,
             static_cast<int64_t>(i) * 60000});
    }

    FinerTfPublishGateHarness off(false);
    off.run(input_bars.data(), static_cast<int>(input_bars.size()),
            "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    require(off.last_error().empty(),
            "lookahead_off finer-TF security run should succeed: " + off.last_error());
    std::vector<int64_t> expected_off = {0, 300000, 600000, 900000, 1200000, 1500000};
    require(off.published_ts == expected_off,
            "lookahead_off finer-TF security must publish EVERY completed security "
            "period (TV merges the LAST intrabar of the calling bar): expected one "
            "publish per 5m bucket, got " + std::to_string(off.published_ts.size()));

    FinerTfPublishGateHarness on(true);
    on.run(input_bars.data(), static_cast<int>(input_bars.size()),
           "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    require(on.last_error().empty(),
            "lookahead_on finer-TF security run should succeed: " + on.last_error());
    std::vector<int64_t> expected_on = {600000, 1500000};
    require(on.published_ts == expected_on,
            "lookahead_on finer-TF security must latch publishes to calling-bar "
            "boundaries (TV merges the FIRST intrabar of the calling bar): expected "
            "only script-TF-aligned bucket completions, got "
            + std::to_string(on.published_ts.size()));

    std::cout << "test_request_security_finer_tf_publish_gate_is_lookahead_only passed.\n";
}

// KI-33 cadence guard: on the 1m-magnifier path (input_tf="1",
// script_tf="15", bar_magnifier on) a COARSER fixed-minute
// request.security — "60" (= 4 x 15m script bars) and "240" (= 16 x
// 15m script bars) — must latch/publish its aggregated value ONLY on
// the coarser-TF wall-clock boundary and hold it CONSTANT within the
// period. The suspected bug (KI-33) was that the security aggregator,
// being fed once per 1m sub-bar inside run_magnified_bar, would
// complete at requested_tf/4 (a "60" security updating every 15m
// instead of every 60m). The correct cadence is pure Pine-timeframe
// arithmetic — 60m = 4x15m, 240m = 16x15m — so this needs no TV data.
//
// The harness registers two coarser HTF securities reading `close`,
// captures the latched value once per SCRIPT bar (on_bar fires once
// per script bar on the magnifier path, after the per-sub-bar security
// feed), and asserts the observed per-script-bar cadence against the
// arithmetic expectation.
class MagnifierCoarserSecurityCadenceHarness : public BacktestEngine {
public:
    double _req_sec_60 = na<double>();
    double _req_sec_240 = na<double>();
    std::vector<double> sec60_per_script_bar;
    std::vector<double> sec240_per_script_bar;
    std::vector<double> script_close_per_script_bar;

    MagnifierCoarserSecurityCadenceHarness() {
        // Coarser fixed-minute HTFs, fed from a 1m input feed. sec 0 =
        // "60" (4x the 15m script bar), sec 1 = "240" (16x).
        register_security_eval(0, "60", "1", false, false);
        register_security_eval(1, "240", "1", false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (!is_complete) {
            return;
        }
        if (sec_id == 0) {
            _req_sec_60 = bar.close;
        } else if (sec_id == 1) {
            _req_sec_240 = bar.close;
        }
    }

    void on_bar(const Bar& bar) override {
        // One capture per script bar (magnifier on_bar fires only on the
        // last tick of the last sub-bar, after that sub-bar's security
        // feed has already published any boundary completion).
        sec60_per_script_bar.push_back(_req_sec_60);
        sec240_per_script_bar.push_back(_req_sec_240);
        script_close_per_script_bar.push_back(bar.close);
    }
};

void test_request_security_magnifier_coarser_tf_cadence() {
    MagnifierCoarserSecurityCadenceHarness strat;

    // 480 gap-free 1m bars = 32 x 15m script bars = 8 x 60m periods =
    // 2 x 240m periods. Distinct, monotonic closes (100 + i) make every
    // bar individually identifiable, so a value that tracked the 15m
    // close every bar is trivially distinguishable from one latched to
    // the coarser boundary. Timestamps start at epoch 0 so 60m/240m
    // UTC-epoch buckets align cleanly.
    const int kInputBars = 480;
    const int kScriptTfMin = 15;      // 15m script bars
    const int kSubPerScript = kScriptTfMin;   // 15 x 1m per script bar
    std::vector<Bar> input_bars;
    input_bars.reserve(kInputBars);
    for (int i = 0; i < kInputBars; ++i) {
        double px = 100.0 + i;   // distinct monotonic close per 1m bar
        input_bars.push_back(
            {px, px + 0.5, px - 0.5, px, 10.0,
             static_cast<int64_t>(i) * 60000});
    }

    strat.run(input_bars.data(), static_cast<int>(input_bars.size()),
              "1", "15", /*bar_magnifier=*/true, 4,
              MagnifierDistribution::ENDPOINTS);
    require(strat.last_error().empty(),
            "magnifier coarser-TF security run should succeed: " + strat.last_error());

    const int expected_script_bars = kInputBars / kSubPerScript;  // 32
    require(static_cast<int>(strat.sec60_per_script_bar.size())
                == expected_script_bars,
            "expected one latched sec value capture per script bar, got "
                + std::to_string(strat.sec60_per_script_bar.size()));

    // --- Arithmetic expectation (pure Pine TF cadence, no TV data) ---
    // Script bar k covers 1m bars [15k, 15k+14]; its last 1m bar is
    // 15k+14. A "60" bucket completes at 1m bar index 59, 119, 179, ...
    // (every 60 bars); a "240" bucket at 239, 479, ... For close(i) =
    // 100 + i the completed HTF close is 100 + (last 1m bar of bucket).
    const int sub60 = 60;    // 60m spans 60 x 1m bars
    const int sub240 = 240;  // 240m spans 240 x 1m bars
    std::vector<double> expected_sec60(expected_script_bars, na<double>());
    std::vector<double> expected_sec240(expected_script_bars, na<double>());
    double latched60 = na<double>();
    double latched240 = na<double>();
    for (int k = 0; k < expected_script_bars; ++k) {
        int last_1m = k * kSubPerScript + (kSubPerScript - 1);  // 15k+14
        // Has a 60m / 240m bucket boundary completed at or before this
        // script bar's final 1m bar? Boundary completes at 1m index
        // (n*period - 1). i.e. (last_1m + 1) % period == 0 marks a fresh
        // completion landing exactly on this script bar.
        if ((last_1m + 1) % sub60 == 0) {
            latched60 = 100.0 + last_1m;
        }
        if ((last_1m + 1) % sub240 == 0) {
            latched240 = 100.0 + last_1m;
        }
        expected_sec60[k] = latched60;
        expected_sec240[k] = latched240;
    }

    // --- Emit the observed per-script-bar table (report evidence) ---
    std::cout << "  [KI-33 cadence] per-script-bar latched security values:\n";
    std::cout << "  bar | script_close |   sec60(obs/exp)   |  sec240(obs/exp)\n";
    for (int k = 0; k < expected_script_bars; ++k) {
        auto fmt = [](double v) {
            return is_na(v) ? std::string("na") : std::to_string(v);
        };
        std::cout << "  " << (k < 10 ? " " : "") << k
                  << "  | " << strat.script_close_per_script_bar[k]
                  << "     | " << fmt(strat.sec60_per_script_bar[k])
                  << " / " << fmt(expected_sec60[k])
                  << "   | " << fmt(strat.sec240_per_script_bar[k])
                  << " / " << fmt(expected_sec240[k]) << "\n";
    }

    // --- Assert cadence: value latches only on the coarser boundary
    //     and holds constant within the period. ---
    for (int k = 0; k < expected_script_bars; ++k) {
        double obs60 = strat.sec60_per_script_bar[k];
        double exp60 = expected_sec60[k];
        require(is_na(obs60) == is_na(exp60),
                "sec60 na-ness mismatch at script bar " + std::to_string(k)
                    + " (obs na=" + std::to_string(is_na(obs60))
                    + ", exp na=" + std::to_string(is_na(exp60)) + ")");
        if (!is_na(exp60)) {
            require(std::abs(obs60 - exp60) < 1e-9,
                    "sec60 latched value wrong at script bar "
                        + std::to_string(k) + ": obs " + std::to_string(obs60)
                        + " vs exp " + std::to_string(exp60)
                        + " (a value tracking the 15m close would be "
                        + std::to_string(strat.script_close_per_script_bar[k])
                        + ")");
        }

        double obs240 = strat.sec240_per_script_bar[k];
        double exp240 = expected_sec240[k];
        require(is_na(obs240) == is_na(exp240),
                "sec240 na-ness mismatch at script bar " + std::to_string(k));
        if (!is_na(exp240)) {
            require(std::abs(obs240 - exp240) < 1e-9,
                    "sec240 latched value wrong at script bar "
                        + std::to_string(k) + ": obs " + std::to_string(obs240)
                        + " vs exp " + std::to_string(exp240));
        }
    }

    // --- Direct anti-bug checks: the value MUST NOT track the 15m close
    //     every bar, and MUST hold constant strictly within a period. ---
    // Script bar 4 sits one script bar past the first 60m completion
    // (bar 3). Under KI-33 the "60" security would re-complete every 15m
    // and read the script-bar-4 15m close (174); the correct latch holds
    // the 60m close from bar 3 (159).
    require(!is_na(strat.sec60_per_script_bar[4]),
            "sec60 should be latched (non-na) by script bar 4");
    require(std::abs(strat.sec60_per_script_bar[4] - 159.0) < 1e-9,
            "sec60 at script bar 4 must hold the prior 60m close (159), not re-latch");
    require(std::abs(strat.sec60_per_script_bar[4]
                     - strat.script_close_per_script_bar[4]) > 1e-6,
            "sec60 must NOT track the 15m script close every bar (KI-33 symptom)");
    // Constant across the whole 60m period bars 4,5,6 (all latch 159).
    require(std::abs(strat.sec60_per_script_bar[5] - 159.0) < 1e-9
                && std::abs(strat.sec60_per_script_bar[6] - 159.0) < 1e-9,
            "sec60 must hold CONSTANT within the 60m period (bars 4-6 == 159)");
    // sec60 changes exactly at the boundary (bar 7 -> 219).
    require(std::abs(strat.sec60_per_script_bar[7] - 219.0) < 1e-9,
            "sec60 must advance to the next 60m close (219) at the bar-7 boundary");

    std::cout << "test_request_security_magnifier_coarser_tf_cadence passed.\n";
}

int main() {
    test_request_security_hook_dispatches_completed_values();
    test_request_security_magnifier_coarser_tf_cadence();
    test_request_security_finer_tf_publish_gate_is_lookahead_only();
    test_request_security_lower_tf_requires_finer_input_bars();
    test_request_security_emulates_ratio_divisible_lower_tf();
    test_request_security_lower_tf_emulation_rejects_unsupported_flags();
    test_request_security_higher_tf_requires_inferable_input_tf();
    test_request_security_helper_ta_uses_security_local_state();
    test_request_security_lower_tf_array_5m_chart_with_1m_emulation();
    test_request_security_lower_tf_array_60m_chart_with_1m_emulation();
    test_request_security_lower_tf_array_rejects_higher_timeframe();
    test_request_security_lower_tf_array_rejects_non_divisible_timeframe();
    return 0;
}
