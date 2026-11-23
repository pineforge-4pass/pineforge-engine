// Tests for BacktestEngine::get_input_int64.
//
// Pine v6 `input.time(defval)` returns a `series int` Unix timestamp in
// MILLISECONDS. int32 overflows for any date past 2038 (and ms-epoch
// values for modern dates are already ~1.7e12). The codegen routes
// `input.time` to `get_input_int64`; this test pins the getter's
// behavior (missing key, valid int64, invalid string, negative value).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

struct GetterHarness : public pineforge::source::PineStrategyHost {
    void on_source_bar(const Bar& /*bar*/) override {}
    int64_t call(const std::string& key, int64_t default_val) const {
        return get_input_int64(key, default_val);
    }

    bool rejects(const std::string& key, int64_t default_val) const {
        try {
            call(key, default_val);
        } catch (const std::invalid_argument& error) {
            return classify_run_failure(error).code == RunFailureCode::setting_rejected
                && std::string(error.what()).find("'" + key + "'") != std::string::npos;
        }
        return false;
    }
};

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK_EQ(actual, expected, msg) do { \
    ++tests_run; \
    auto _a = (actual); \
    auto _e = (expected); \
    if (_a == _e) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s (got %lld, expected %lld)\n", msg, \
                  static_cast<long long>(_a), static_cast<long long>(_e)); } \
} while(0)

void test_missing_key_returns_default() {
    GetterHarness h;
    CHECK_EQ(h.call("absent", 42), 42, "missing key returns default");
    CHECK_EQ(h.call("absent", -1), -1, "missing key returns negative default");
    CHECK_EQ(h.call("absent", 0), 0, "missing key returns zero default");
}

void test_valid_int64_epoch_ms() {
    GetterHarness h;
    // 2023-11-14T22:13:20Z -- well past int32 max (2.147e9) when expressed
    // as ms-epoch (1.7e12). This is the failure mode the int32 helper had
    // when input.time was routed there.
    h.set_input("ts", "1700000000000");
    CHECK_EQ(h.call("ts", 0), 1700000000000LL, "valid ms epoch parses to int64");

    // Far-future date past int32 seconds-epoch overflow (2038-01-19T03:14:07Z).
    h.set_input("future", "4102444800000");  // 2100-01-01T00:00:00Z in ms
    CHECK_EQ(h.call("future", 0), 4102444800000LL,
             "2100-01-01 ms epoch survives int64 round-trip");
}

void test_invalid_string_is_rejected() {
    GetterHarness h;
    // expectation corrected: 7 and 99 -> rejection, because the overrides are present but malformed (#918).
    h.set_input("garbage", "not-a-number");
    CHECK_EQ(h.rejects("garbage", 7), true, "unparseable input is rejected");

    h.set_input("empty", "");
    CHECK_EQ(h.rejects("empty", 99), true, "empty input is rejected");
}

void test_negative_value() {
    GetterHarness h;
    h.set_input("neg", "-1700000000000");
    CHECK_EQ(h.call("neg", 0), -1700000000000LL,
             "negative int64 parses correctly");
}

}  // namespace

int main() {
    printf("--- get_input_int64 ---\n");
    test_missing_key_returns_default();
    test_valid_int64_epoch_ms();
    test_invalid_string_is_rejected();
    test_negative_value();
    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
