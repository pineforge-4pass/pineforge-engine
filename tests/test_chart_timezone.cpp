// test_chart_timezone.cpp — exercise BacktestEngine::set_chart_timezone()
// and verify the three-way TZ contract:
//
//   - bare variable form ``hour`` / ``minute`` / ``dayofweek`` returns
//     the EXCHANGE-TZ wall clock (== UTC for crypto symbols on the corpus
//     ETH-USDT data, which is the engine's storage TZ). The codegen
//     emits these as ``_bar_hour()`` etc., which call
//     ``_decompose_bar_time()`` — and that helper INTENTIONALLY ignores
//     both ``syminfo_.timezone`` and ``chart_timezone_``. Per TV
//     reference docs, the variable form is exchange-TZ, NOT chart-TZ,
//     and the corpus has dozens of probes (``hour == N`` stop-cross
//     gates, etc.) that would silently regress if this changed.
//
//   - 1-arg function form ``hour(time)`` defaults the implicit tz arg
//     to ``syminfo.timezone`` (the EXCHANGE timezone, NOT the chart
//     display TZ). The codegen handles this branch separately
//     (codegen/visit_call.py); ``set_chart_timezone`` MUST NOT mutate
//     ``syminfo_.timezone`` because that conflated the two TV concepts
//     and shifted ``hour(time)``-bucketed accumulators in
//     ``validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask``
//     by the chart-vs-exchange offset.
//
//   - The chart's display timezone is stored in a dedicated
//     ``chart_timezone_`` slot and surfaced via ``chart_timezone()``,
//     available for harnesses (and any future Pine ``chart.timezone``
//     wiring) without poisoning the bar-time decomposition.
//
// This fixture pins down all three semantics: variable-form stays on
// UTC even after ``set_chart_timezone("Asia/Taipei")``,
// ``chart_timezone()`` round-trips the value harnesses pass in, AND
// ``set_chart_timezone`` LEAVES ``syminfo_.timezone`` UNCHANGED.

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
    // Accessor used by ``test_chart_tz_setter_does_not_mutate_syminfo``
    // to prove the regression fix: pre-fix this returned whatever the
    // last ``set_chart_timezone`` call passed in (because the setter
    // wrote into ``syminfo_.timezone``); post-fix it stays at the
    // ``SymInfo`` constructor default of "UTC".
    const std::string& syminfo_timezone() const { return syminfo_.timezone; }
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
    // ``set_chart_timezone`` / ``chart_timezone()`` is the public C ABI
    // surface harnesses (validator, py wrapper) wire up before
    // ``run_backtest_full``. Empty / "UTC" / "Etc/UTC" all mean
    // "engine fast path" downstream; anything else gets forwarded
    // verbatim into a ``ScopedTimezone`` setenv guard at runtime —
    // none of which we can exercise in a unit test, so just pin the
    // round-trip.
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

void test_chart_tz_setter_does_not_mutate_syminfo() {
    // Regression guard for the
    // validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask
    // bug: pre-fix ``set_chart_timezone`` overwrote
    // ``syminfo_.timezone``, which the codegen reads as the default
    // tz argument of the 1-arg ``hour(time)`` form. The harness's
    // chart-display TZ (Asia/Taipei) thereby leaked into Pine's
    // exchange-TZ-default semantics and shifted ``hour``-bucketed
    // accumulators by 8h vs TV. Post-fix the chart TZ lives in a
    // separate ``chart_timezone_`` slot and this test guarantees
    // that contract holds.
    std::printf("test_chart_tz_setter_does_not_mutate_syminfo\n");
    TimeProbeEngine eng;
    // Default ``SymInfo::timezone`` is "UTC".
    CHECK(eng.syminfo_timezone() == "UTC");
    eng.set_chart_timezone("Asia/Taipei");
    CHECK(eng.syminfo_timezone() == "UTC");
    eng.set_chart_timezone("America/New_York");
    CHECK(eng.syminfo_timezone() == "UTC");
    // Even an empty chart TZ must NOT clobber the SymInfo default.
    eng.set_chart_timezone("");
    CHECK(eng.syminfo_timezone() == "UTC");
}

}  // namespace

int main() {
    test_default_is_utc();
    test_variable_form_stays_utc_after_chart_tz_set();
    test_chart_timezone_round_trip();
    test_chart_tz_setter_does_not_mutate_syminfo();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
