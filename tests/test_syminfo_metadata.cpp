// Tests for symbol-metadata injection (#19): set_syminfo_metadata /
// get_syminfo_metadata and the syminfo timezone/session setters.
//
// Fundamental fields (shares_outstanding_*, recommendations_*,
// target_price_*, …) have no OHLCV source, so they read na<double>()
// until a data feed injects a value. This pins: absent -> na, injected ->
// exact value, overwrite, and that the tz/session setters land on syminfo_.
#include <cmath>
#include <cstdio>
#include <string>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

struct MetaHarness : public BacktestEngine {
    void on_bar(const Bar& /*bar*/) override {}
    double meta(const std::string& key) const { return get_syminfo_metadata(key); }
    const SymInfo& sym() const { return syminfo_; }
};

int tests_run = 0;
int tests_passed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (cond) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); } \
} while(0)

void test_absent_returns_na() {
    MetaHarness h;
    CHECK(std::isnan(h.meta("shares_outstanding_total")),
          "un-injected metadata reads na");
    CHECK(std::isnan(h.meta("target_price_average")),
          "un-injected target_price_average reads na");
}

void test_injected_value() {
    MetaHarness h;
    h.set_syminfo_metadata("shares_outstanding_total", 1.6e9);
    CHECK(h.meta("shares_outstanding_total") == 1.6e9,
          "injected metadata reads back exactly");
    CHECK(std::isnan(h.meta("recommendations_buy")),
          "sibling un-injected field still na");
}

void test_overwrite() {
    MetaHarness h;
    h.set_syminfo_metadata("target_price_high", 200.0);
    h.set_syminfo_metadata("target_price_high", 250.0);
    CHECK(h.meta("target_price_high") == 250.0, "metadata overwrite wins");
}

void test_tz_session_setters() {
    MetaHarness h;
    CHECK(h.sym().timezone == "UTC", "default syminfo timezone is UTC");
    CHECK(h.sym().session == "24x7", "default syminfo session is 24x7");
    h.set_syminfo_timezone("America/New_York");
    h.set_syminfo_session("0930-1600:23456");
    CHECK(h.sym().timezone == "America/New_York", "timezone setter lands on syminfo_");
    CHECK(h.sym().session == "0930-1600:23456", "session setter lands on syminfo_");
}

}  // namespace

int main() {
    printf("--- syminfo_metadata ---\n");
    test_absent_returns_na();
    test_injected_value();
    test_overwrite();
    test_tz_session_setters();
    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
