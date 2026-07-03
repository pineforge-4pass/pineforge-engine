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
    void set_internal_indices(int bar_idx, int last_idx) {
        bar_index_ = bar_idx;
        last_bar_index_ = last_idx;
    }
    int public_bar_index() const { return pine_bar_index(); }
    int public_last_bar_index() const { return pine_last_bar_index(); }
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

void test_bar_index_offset_metadata() {
    MetaHarness h;
    h.set_internal_indices(5, 99);
    CHECK(h.public_bar_index() == 5, "default public bar_index is internal index");
    CHECK(h.public_last_bar_index() == 99, "default public last_bar_index is internal last index");
    h.set_syminfo_metadata("bar_index_offset", 70.0);
    CHECK(h.public_bar_index() == 75, "bar_index_offset shifts public bar_index");
    CHECK(h.public_last_bar_index() == 169, "bar_index_offset shifts public last_bar_index");
    h.set_syminfo_metadata("bar_index_offset", std::nan(""));
    CHECK(h.public_bar_index() == 5, "non-finite bar_index_offset resets to zero");
}

}  // namespace

int main() {
    printf("--- syminfo_metadata ---\n");
    test_absent_returns_na();
    test_injected_value();
    test_overwrite();
    test_tz_session_setters();
    test_bar_index_offset_metadata();
    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
