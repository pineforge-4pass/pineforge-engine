// Tests for BacktestEngine::validate_security_timeframes — the matrix
// of accept/reject across HTF, LTF, same-TF, invalid-literal cases.
//
// Style: cassert + plain int main() to match other engine tests.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

// Keep these assertions live in Release, where the repository's CMake flags
// define NDEBUG for test executables.
#undef NDEBUG
#include <cassert>

using namespace pineforge;

namespace {

// Subclass exposing register_security{,_lower_tf}_eval (protected in
// the base) so each test can wire up exactly one security with the
// timeframe combo under test.
struct ValidationHarness : public pineforge::source::PineStrategyHost {
    void on_source_bar(const Bar&) override {}
    void evaluate_security(int, const Bar&, bool) override {}

    void add_security(const std::string& requested_tf,
                      const std::string& input_tf) {
        register_security_eval(0, requested_tf, input_tf, false, false);
    }
    void add_security_lower_tf(const std::string& requested_tf,
                               const std::string& input_tf) {
        register_security_lower_tf_eval(0, requested_tf, input_tf);
    }
    std::size_t evaluator_count() const { return security_eval_states_.size(); }
    std::size_t pine_state_count() const { return pine_security_states_.size(); }
    bool lower_array_requested() const {
        return pine_security_states_.at(0).lower_tf_array_requested;
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

struct RegistrationFailure {
    std::string message;
    RunFailureCode code = RunFailureCode::none;
    std::string args;
};

RegistrationFailure capture_registration_failure(
        ValidationHarness& strat, bool lower_tf_array,
        const std::string& requested_tf, const std::string& input_tf) {
    try {
        if (lower_tf_array) {
            strat.add_security_lower_tf(requested_tf, input_tf);
        } else {
            strat.add_security(requested_tf, input_tf);
        }
    } catch (const std::exception& error) {
        const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
        assert(info != nullptr && "registration failure must carry a run code");
        const auto& value = info->run_failure();
        return {error.what(), info->run_failure_code(),
                value.args ? *value.args : std::string()};
    }
    assert(false && "registration was expected to reject the timeframe");
    return {};
}

void expect_contains(const std::string& haystack, const std::string& needle,
                     const char* label) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << label << ": expected error to contain '" << needle
                  << "' but got: '" << haystack << "'\n";
        assert(false);
    }
}

void expect_request_timeframe_failure(
        const std::string& message, RunFailureCode code,
        const std::string& args, const char* api, const char* literal,
        const char* label) {
    if (code != RunFailureCode::request_timeframe_invalid) {
        std::cerr << label << ": expected request_timeframe_invalid, got "
                  << run_failure_code_name(code) << "\n";
        assert(false);
    }
    const std::string expected =
        std::string(api) + ": invalid timeframe literal '" + literal + "'";
    expect_contains(message, expected, label);
    expect_contains(args, std::string("\"api\":\"") + api + "\"", label);
}

// 1. request.security HTF — accept (60 from 15, ratio 4)
void test_security_htf_accept() {
    ValidationHarness strat;
    strat.add_security("60", "15");
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
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
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    assert(strat.lower_array_requested());
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
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    assert(strat.lower_array_requested());
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
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    auto err = run_with(strat, "15");
    const auto value = run_failure_value_of(strat);
    expect_request_timeframe_failure(
        err, value.code, value.args ? *value.args : std::string(),
        "request.security", "abc", "test_security_invalid_literal_rejected");
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

// 8. A known input timeframe owns malformed request literals at registration.
void test_security_invalid_literal_rejected_at_registration() {
    ValidationHarness strat;
    const auto failure =
        capture_registration_failure(strat, false, "abc", "15");
    expect_request_timeframe_failure(
        failure.message, failure.code, failure.args,
        "request.security", "abc",
        "test_security_invalid_literal_rejected_at_registration");
    assert(strat.evaluator_count() == 0);
    assert(strat.pine_state_count() == 0);
    std::cout << "test_security_invalid_literal_rejected_at_registration passed.\n";
}

// 9. The same registration door maps the numeric parser's out_of_range class.
void test_security_overflow_literal_rejected_at_registration() {
    ValidationHarness strat;
    const char* literal = "999999999999999999999999999999999999999";
    const auto failure =
        capture_registration_failure(strat, false, literal, "15");
    expect_request_timeframe_failure(
        failure.message, failure.code, failure.args,
        "request.security", literal,
        "test_security_overflow_literal_rejected_at_registration");
    assert(strat.evaluator_count() == 0);
    assert(strat.pine_state_count() == 0);
    std::cout << "test_security_overflow_literal_rejected_at_registration passed.\n";
}

// 10. The lower-TF door owns its API identity before scalar delegation.
void test_security_lower_tf_invalid_literal_rejected_at_registration() {
    ValidationHarness strat;
    const auto failure =
        capture_registration_failure(strat, true, "abc", "15");
    expect_request_timeframe_failure(
        failure.message, failure.code, failure.args,
        "request.security_lower_tf", "abc",
        "test_security_lower_tf_invalid_literal_rejected_at_registration");
    assert(strat.evaluator_count() == 0);
    assert(strat.pine_state_count() == 0);
    std::cout << "test_security_lower_tf_invalid_literal_rejected_at_registration passed.\n";
}

// 11. A lower-TF request whose input is not known defers the same check until
// validate_security_timeframes, before lower-TF capability parsing.
void test_security_lower_tf_invalid_literal_rejected_deferred() {
    ValidationHarness strat;
    strat.add_security_lower_tf("abc", "");
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    assert(strat.lower_array_requested());
    const auto err = run_with(strat, "15");
    const auto value = run_failure_value_of(strat);
    expect_request_timeframe_failure(
        err, value.code, value.args ? *value.args : std::string(),
        "request.security_lower_tf", "abc",
        "test_security_lower_tf_invalid_literal_rejected_deferred");
    std::cout << "test_security_lower_tf_invalid_literal_rejected_deferred passed.\n";
}

// 12. Empty requests retain their passthrough behavior.
void test_security_empty_request_passthrough() {
    ValidationHarness strat;
    strat.add_security("", "15");
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    const auto err = run_with(strat, "15");
    assert(err.empty() && "Empty request.security timeframe should pass through");
    std::cout << "test_security_empty_request_passthrough passed.\n";
}

// 13. Calendar months remain valid for scalar request.security.
void test_security_calendar_month_accept() {
    ValidationHarness strat;
    strat.add_security("M", "15");
    assert(strat.evaluator_count() == 1);
    assert(strat.pine_state_count() == 1);
    const auto err = run_with(strat, "15");
    assert(err.empty() && "Scalar calendar-month request should be accepted");
    std::cout << "test_security_calendar_month_accept passed.\n";
}

// 14. Calendar months remain invalid for request.security_lower_tf.
void test_security_lower_tf_calendar_month_rejected() {
    ValidationHarness strat;
    const auto failure =
        capture_registration_failure(strat, true, "M", "15");
    expect_request_timeframe_failure(
        failure.message, failure.code, failure.args,
        "request.security_lower_tf", "M",
        "test_security_lower_tf_calendar_month_rejected");
    assert(strat.evaluator_count() == 0);
    assert(strat.pine_state_count() == 0);
    std::cout << "test_security_lower_tf_calendar_month_rejected passed.\n";
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
    test_security_invalid_literal_rejected_at_registration();
    test_security_overflow_literal_rejected_at_registration();
    test_security_lower_tf_invalid_literal_rejected_at_registration();
    test_security_lower_tf_invalid_literal_rejected_deferred();
    test_security_empty_request_passthrough();
    test_security_calendar_month_accept();
    test_security_lower_tf_calendar_month_rejected();
    std::cout << "All test_security_tf_validation tests passed.\n";
    return 0;
}
