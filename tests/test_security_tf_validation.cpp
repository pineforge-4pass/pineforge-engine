// Tests for BacktestEngine::validate_security_timeframes — the matrix
// of accept/reject across HTF, LTF, same-TF, invalid-literal cases.
//
// Style: cassert + plain int main() to match other engine tests.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

// Subclass exposing register_security{,_lower_tf}_eval (protected in
// the base) so each test can wire up exactly one security with the
// timeframe combo under test.
struct ValidationHarness : public BacktestEngine {
    void on_bar(const Bar&) override {}
    void evaluate_security(int, const Bar&, bool) override {}

    void add_security(const std::string& requested_tf,
                      const std::string& input_tf) {
        register_security_eval(0, requested_tf, input_tf, false, false);
    }
    void add_security_lower_tf(const std::string& requested_tf,
                               const std::string& input_tf) {
        register_security_lower_tf_eval(0, requested_tf, input_tf);
    }
};

// Drive a tiny run so validate_security_timeframes is invoked through
// the normal engine path and any thrown error is captured as
// last_error(). Returns last_error() (empty on success).
std::string run_with(ValidationHarness& strat,
                     const std::string& input_tf) {
    std::vector<Bar> bars = {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {11.0, 11.0, 11.0, 11.0, 100.0, 900000},
    };
    strat.run(bars.data(), static_cast<int>(bars.size()),
              input_tf, input_tf, false, 4,
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

// 1. request.security HTF — accept (60 from 15, ratio 4)
void test_security_htf_accept() {
    ValidationHarness strat;
    strat.add_security("60", "15");
    auto err = run_with(strat, "15");
    assert(err.empty() && "HTF request.security should be accepted");
    std::cout << "test_security_htf_accept passed.\n";
}

// 2. request.security finer-than-input — reject with hint
void test_security_finer_rejected_with_hint() {
    ValidationHarness strat;
    strat.add_security("5", "15");
    auto err = run_with(strat, "15");
    assert(!err.empty());
    expect_contains(err, "Use request.security_lower_tf for sub-input timeframes",
                    "test_security_finer_rejected_with_hint");
    std::cout << "test_security_finer_rejected_with_hint passed.\n";
}

// 3. request.security_lower_tf with non-integer divisor — reject
void test_security_lower_tf_non_divisor_rejected() {
    ValidationHarness strat;
    strat.add_security_lower_tf("4", "15");
    auto err = run_with(strat, "15");
    assert(!err.empty());
    expect_contains(err, "is not an integer divisor of input",
                    "test_security_lower_tf_non_divisor_rejected");
    std::cout << "test_security_lower_tf_non_divisor_rejected passed.\n";
}

// 4. request.security_lower_tf with integer divisor — accept (5 from 15)
void test_security_lower_tf_divisor_accept() {
    ValidationHarness strat;
    strat.add_security_lower_tf("5", "15");
    auto err = run_with(strat, "15");
    assert(err.empty() && "Integer-divisor LTF should be accepted");
    std::cout << "test_security_lower_tf_divisor_accept passed.\n";
}

// 5. request.security_lower_tf with coarser-than-input — reject
void test_security_lower_tf_not_finer_rejected() {
    ValidationHarness strat;
    strat.add_security_lower_tf("60", "15");
    auto err = run_with(strat, "15");
    assert(!err.empty());
    expect_contains(err, "Lower-TF API requires a strictly finer timeframe",
                    "test_security_lower_tf_not_finer_rejected");
    std::cout << "test_security_lower_tf_not_finer_rejected passed.\n";
}

// 6. request.security with garbage TF literal — reject with literal.
// We register with input_tf="" so the constructor-time tf_ratio path is
// skipped; validate_security_timeframes (driven from run()) is the
// layer under test here.
void test_security_invalid_literal_rejected() {
    ValidationHarness strat;
    strat.add_security("abc", "");
    auto err = run_with(strat, "15");
    assert(!err.empty());
    expect_contains(err, "invalid timeframe literal 'abc'",
                    "test_security_invalid_literal_rejected");
    std::cout << "test_security_invalid_literal_rejected passed.\n";
}

// 7. request.security with same TF as input — accept
void test_security_same_tf_accept() {
    ValidationHarness strat;
    strat.add_security("15", "15");
    auto err = run_with(strat, "15");
    assert(err.empty() && "Same-TF request.security should be accepted");
    std::cout << "test_security_same_tf_accept passed.\n";
}

}  // namespace

int main() {
    test_security_htf_accept();
    test_security_finer_rejected_with_hint();
    test_security_lower_tf_non_divisor_rejected();
    test_security_lower_tf_divisor_accept();
    test_security_lower_tf_not_finer_rejected();
    test_security_invalid_literal_rejected();
    test_security_same_tf_accept();
    std::cout << "All test_security_tf_validation tests passed.\n";
    return 0;
}
