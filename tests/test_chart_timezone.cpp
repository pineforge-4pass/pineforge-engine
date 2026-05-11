// test_chart_timezone.cpp — exercise BacktestEngine::set_chart_timezone()
// and verify the dual-semantics rule:
//
//   - bare variable form ``hour`` / ``minute`` / ``dayofweek`` returns
//     the EXCHANGE-TZ wall clock (== UTC for crypto symbols on the corpus
//     ETH-USDT data, which is the engine's storage TZ). The codegen
//     emits these as ``_bar_hour()`` etc., which call
//     ``_decompose_bar_time()`` — and that helper INTENTIONALLY ignores
//     ``syminfo_.timezone``. Per TV reference docs, the variable form is
//     exchange-TZ, NOT chart-TZ, and the corpus has dozens of probes
//     (``hour == N`` stop-cross gates, etc.) that would silently regress
//     if this changed.
//
//   - 1-arg function form ``hour(time)`` defaults the implicit tz arg
//     to ``syminfo.timezone``, which TV harnesses commonly set to the
//     chart-display TZ for cross-exchange / multi-zone work. The
//     codegen handles this branch separately
//     (codegen/visit_call.py); the engine surface needed here is
//     ``set_chart_timezone(tz)`` so harnesses can wire a value through
//     the C ABI before ``run_backtest_full``.
//
// This fixture covers BOTH halves of the contract: the engine's
// variable-form decomposition stays on UTC even after
// ``set_chart_timezone("Asia/Taipei")``, AND ``chart_timezone()``
// round-trips the value the codegen-emitted lambdas read at runtime.

#include <cstdio>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

namespace {

// Subclass the engine to (a) seed ``current_bar_`` directly without
// going through ``run()``, (b) re-export the ``protected`` time
// accessors as public so the test can poke them, and (c) supply a
// no-op ``on_bar`` so ``BacktestEngine``'s pure-virtual surface is
// satisfied (the test never drives a ``run()`` so the override is
// unused but required for instantiation).
class TimeProbeEngine : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}
    void set_bar_timestamp(int64_t ts_ms) {
        current_bar_.timestamp = ts_ms;
    }
    using BacktestEngine::_bar_hour;
    using BacktestEngine::_bar_minute;
    using BacktestEngine::_bar_dayofweek;
    using BacktestEngine::_bar_dayofmonth;
    using BacktestEngine::_bar_year;
};

// 2025-03-31 03:30 UTC == 11:30 Asia/Taipei (UTC+8). Picked because the
// real-world divergence in
// corpus/validation/62-same-id-stop-cross-before-modify pivots on
// exactly this offset (TV trade @ 11:30, engine trade @ 03:30, gate
// fires at hour==3, exchange TZ = UTC).
constexpr int64_t kTaipeiCross = 1743391800000LL;

void test_default_is_utc() {
    std::printf("test_default_is_utc\n");
    TimeProbeEngine eng;
    eng.set_bar_timestamp(kTaipeiCross);
    CHECK(eng._bar_hour() == 3);
    CHECK(eng._bar_minute() == 30);
    // Pine dayofweek: Sunday=1 .. Saturday=7. 2025-03-31 is a Monday → 2.
    CHECK(eng._bar_dayofweek() == 2);
}

void test_variable_form_stays_utc_after_chart_tz_set() {
    // Regression guard. The bare-name ``hour`` / ``minute`` /
    // ``dayofweek`` builtins go through ``_bar_hour()`` etc., which MUST
    // stay on UTC even after ``set_chart_timezone`` is wired up — that
    // setter feeds ``syminfo_.timezone`` for the function form
    // (``hour(time)`` 1-arg overload) ONLY. Mixing them would silently
    // break the dozens of ``hour == N`` stop-cross probes in
    // corpus/validation/.
    std::printf("test_variable_form_stays_utc_after_chart_tz_set\n");
    TimeProbeEngine eng;
    eng.set_bar_timestamp(kTaipeiCross);
    eng.set_chart_timezone("Asia/Taipei");
    CHECK(eng._bar_hour() == 3);
    CHECK(eng._bar_minute() == 30);
    CHECK(eng._bar_dayofweek() == 2);
}

void test_chart_timezone_round_trip() {
    // The 1-arg ``hour(time)`` codegen path reads the value back out via
    // ``syminfo_.timezone``; this test pins down the public-surface
    // round-trip the codegen depends on. Empty / "UTC" / "Etc/UTC" all
    // mean "engine fast path", anything else is forwarded verbatim to
    // the runtime's ``ScopedTimezone`` setenv guard.
    std::printf("test_chart_timezone_round_trip\n");
    TimeProbeEngine eng;
    eng.set_chart_timezone("Asia/Taipei");
    CHECK(eng.chart_timezone() == "Asia/Taipei");
    eng.set_chart_timezone("America/New_York");
    CHECK(eng.chart_timezone() == "America/New_York");
    eng.set_chart_timezone("UTC");
    CHECK(eng.chart_timezone() == "UTC");
    eng.set_chart_timezone("");
    CHECK(eng.chart_timezone() == "");
}

}  // namespace

int main() {
    test_default_is_utc();
    test_variable_form_stays_utc_after_chart_tz_set();
    test_chart_timezone_round_trip();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
