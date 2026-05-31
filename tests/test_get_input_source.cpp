// Tests for BacktestEngine::get_input_source + the native source-series
// backing store (_src_<field>_ / _push_source_series).
//
// Pine v6 `input.source(defval)` returns a `series float` and supports
// RUNTIME override of which native price series feeds an indicator. The
// engine resolves the override string ("open"/"high"/.../"hlcc4") to a
// canonical base-class series; codegen passes the defval field's series as
// the fallback. This test pins: resolution (missing key -> default,
// override -> matching series, non-native -> default), history cadence
// (push on first tick, update intrabar), and the active-gate.
#include <cstdio>
#include <string>

#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

struct SourceHarness : public BacktestEngine {
    explicit SourceHarness(bool active = true) { _src_series_active_ = active; }
    void on_bar(const Bar& /*bar*/) override {}

    const Series<double>& resolve(const std::string& key) {
        return get_input_source(key, _src_close_);
    }

    // Drive one bar through the source-series push exactly as dispatch_bar
    // would (first tick => push). Bar fields: open, high, low, close, volume.
    void feed(double o, double h, double l, double c, double v) {
        current_bar_ = Bar{o, h, l, c, v, 0};
        is_first_tick_ = true;
        _push_source_series();
    }
    // Simulate a magnifier intrabar refinement of the current bar (no push).
    void feed_intrabar(double o, double h, double l, double c, double v) {
        current_bar_ = Bar{o, h, l, c, v, 0};
        is_first_tick_ = false;
        _push_source_series();
    }

    const Series<double>& close_s() const { return _src_close_; }
    const Series<double>& high_s()  const { return _src_high_; }
    const Series<double>& hl2_s()   const { return _src_hl2_; }
};

int tests_run = 0;
int tests_passed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (cond) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); } \
} while(0)

#define CHECK_DBL(actual, expected, msg) do { \
    ++tests_run; \
    double _a = (actual); double _e = (expected); \
    double _d = _a - _e; if (_d < 0) _d = -_d; \
    if (_d < 1e-9) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s (got %.10g, expected %.10g)\n", msg, _a, _e); } \
} while(0)

void test_resolution_default_when_absent() {
    SourceHarness h;
    // No "Src" input set -> returns the default series (identity).
    CHECK(&h.resolve("Src") == &h.close_s(),
          "missing key returns default series (identity)");
}

void test_resolution_override() {
    SourceHarness h;
    h.set_input("Src", "high");
    CHECK(&h.resolve("Src") == &h.high_s(),
          "override 'high' resolves to high series");
    h.set_input("Src", "hl2");
    CHECK(&h.resolve("Src") == &h.hl2_s(),
          "override 'hl2' resolves to hl2 series");
}

void test_resolution_nonnative_falls_back() {
    SourceHarness h;
    h.set_input("Src", "ta.sma(close,14)");  // non-native: impossible post-analyzer
    CHECK(&h.resolve("Src") == &h.close_s(),
          "non-native override falls back to default series");
    h.set_input("Src", "");
    CHECK(&h.resolve("Src") == &h.close_s(),
          "empty override falls back to default series");
}

void test_history_cadence() {
    SourceHarness h;
    h.feed(10, 12, 9, 11, 100);   // bar 0
    h.feed(11, 15, 10, 14, 200);  // bar 1
    h.feed(14, 16, 13, 13, 300);  // bar 2
    const Series<double>& c = h.close_s();
    CHECK_DBL(c[0], 13, "close[0] == latest close");
    CHECK_DBL(c[1], 14, "close[1] == prev close");
    CHECK_DBL(c[2], 11, "close[2] == first close");
    // Derived source: hl2 = (high+low)/2 of the latest bar = (16+13)/2.
    CHECK_DBL(h.hl2_s()[0], 14.5, "hl2[0] == (high+low)/2 of latest bar");
}

void test_intrabar_update_no_advance() {
    SourceHarness h;
    h.feed(10, 12, 9, 11, 100);          // bar 0, close=11
    h.feed(11, 15, 10, 14, 200);         // bar 1, close=14
    h.feed_intrabar(11, 18, 10, 17, 250); // refine bar 1 -> close=17 (no push)
    const Series<double>& c = h.close_s();
    CHECK_DBL(c[0], 17, "intrabar update overwrites current close, no advance");
    CHECK_DBL(c[1], 11, "intrabar update leaves prior bar intact");
}

void test_inactive_gate_no_push() {
    SourceHarness h(/*active=*/false);
    h.feed(10, 12, 9, 11, 100);
    h.feed(11, 15, 10, 14, 200);
    CHECK(h.close_s().size() == 0,
          "inactive gate: no source-series history pushed");
}

}  // namespace

int main() {
    printf("--- get_input_source ---\n");
    test_resolution_default_when_absent();
    test_resolution_override();
    test_resolution_nonnative_falls_back();
    test_history_cadence();
    test_intrabar_update_no_advance();
    test_inactive_gate_no_push();
    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
