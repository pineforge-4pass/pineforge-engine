// Tests for the LTF script_tf upper bound enforced by
// validate_security_timeframes: request.security_lower_tf must reject
// requested timeframes that are >= script_tf or that don't evenly
// divide script_tf, even when the requested TF is a valid integer
// multiple of input_tf (input-passthrough path added in PR #4).
//
// Matrix exhaustively exercises the four boundary cases enumerated
// in the fix description for "fix(security): reject LTF when req >=
// script_tf or script not divisible by req":
//   1. input=1, script=15, req=30 -> reject ("must be finer than script timeframe '15'")
//   2. input=1, script=15, req=15 -> reject (req == script also fails the strict-finer rule)
//   3. input=1, script=15, req=7  -> reject ("must evenly divide script timeframe '15'")
//   4. input=1, script=15, req=5  -> accept (5<15, 15%5==0, 5%1==0)
//   5. input=1, script=15, req=1  -> accept (input passthrough; req==input)
//
// Style: cassert + plain int main() to match the rest of tests/.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

// Minimal harness exposing register_security_lower_tf_eval (protected
// in BacktestEngine) so each case wires exactly one LTF security with
// the timeframe combo under test.
struct LtfBoundHarness : public BacktestEngine {
    void on_bar(const Bar&) override {}
    void evaluate_security(int, const Bar&, bool) override {}

    explicit LtfBoundHarness(const std::string& requested_tf) {
        // Pass empty input_tf at register time; validate_security_timeframes
        // (driven from run()) is the layer under test and re-runs the full
        // matrix once script_tf_ + input_tf_ are known.
        register_security_lower_tf_eval(0, requested_tf, "");
    }
};

// Run a tiny synthetic feed so validate_security_timeframes is invoked
// through the normal engine path and any thrown error is captured as
// last_error(). Returns last_error() (empty on success).
std::string run_with(LtfBoundHarness& strat,
                     const std::string& input_tf,
                     const std::string& script_tf) {
    // Two bars 15 minutes apart so detect_timeframe doesn't kick in
    // (input_tf is supplied explicitly anyway). The exact OHLCV is
    // irrelevant -- validation runs before any feed.
    std::vector<Bar> bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {11.0, 11.0, 11.0, 11.0, 100.0, 900000},
    };
    strat.run(bars.data(), static_cast<int>(bars.size()),
              input_tf, script_tf, false, 4,
              MagnifierDistribution::ENDPOINTS);
    return strat.last_error();
}

void expect_contains(const std::string& haystack, const std::string& needle,
                     const char* label) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << label << ": expected error to contain '" << needle
                  << "' but got: '" << haystack << "'\n";
        assert(false);
    }
}

// 1. req=30 > script=15 -> reject (must be finer than script TF)
void test_req_above_script_rejected() {
    LtfBoundHarness strat("30");
    auto err = run_with(strat, "1", "15");
    assert(!err.empty());
    expect_contains(err, "must be finer than script timeframe '15'",
                    "test_req_above_script_rejected");
    std::cout << "test_req_above_script_rejected passed.\n";
}

// 2. req=15 == script=15 -> reject (must be STRICTLY finer)
void test_req_equals_script_rejected() {
    LtfBoundHarness strat("15");
    auto err = run_with(strat, "1", "15");
    assert(!err.empty());
    expect_contains(err, "must be finer than script timeframe '15'",
                    "test_req_equals_script_rejected");
    std::cout << "test_req_equals_script_rejected passed.\n";
}

// 3. req=7 < script=15 but 15%7!=0 -> reject (must evenly divide)
void test_req_non_divisor_of_script_rejected() {
    LtfBoundHarness strat("7");
    auto err = run_with(strat, "1", "15");
    assert(!err.empty());
    expect_contains(err, "must evenly divide script timeframe '15'",
                    "test_req_non_divisor_of_script_rejected");
    std::cout << "test_req_non_divisor_of_script_rejected passed.\n";
}

// 4. req=5 < script=15, 15%5==0, 5%1==0 -> accept
void test_req_divisor_of_script_accept() {
    LtfBoundHarness strat("5");
    auto err = run_with(strat, "1", "15");
    assert(err.empty() && "5m LTF on script=15 input=1 must validate");
    std::cout << "test_req_divisor_of_script_accept passed.\n";
}

// 5. req=1 == input=1 < script=15 -> accept (raw input passthrough)
void test_req_equals_input_passthrough_accept() {
    LtfBoundHarness strat("1");
    auto err = run_with(strat, "1", "15");
    assert(err.empty() && "1m LTF on script=15 input=1 must validate (raw passthrough)");
    std::cout << "test_req_equals_input_passthrough_accept passed.\n";
}

}  // namespace

int main() {
    test_req_above_script_rejected();
    test_req_equals_script_rejected();
    test_req_non_divisor_of_script_rejected();
    test_req_divisor_of_script_accept();
    test_req_equals_input_passthrough_accept();
    std::cout << "All test_security_lower_tf_script_bound tests passed.\n";
    return 0;
}
