// test_ta_series_length_supertrend -- ta.supertrend with a factor that is not
// a constant (pineforge::source::PineSupertrend), replayed bar by bar against
// TradingView's answers.
//
// TradingView compiles a series factor but reads it once: the line and the
// direction are the Pine reference implementation with the factor of the
// call's first execution. The sites of test_ta_series_length_data.hpp are lab
// tv exports of synthetic probes (K-TA-DYNLEN, 2026-09-26) on
// BINANCE:BTCUSDT 15 and NYSE:F 15 from 2025-04-01; each Signal carries the
// bar's high, low, close and factor and TradingView's line and direction.

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "test_ta_series_length_data.hpp"

using namespace pineforge;
using namespace ta_series_length_data;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, ...) do {                                                   \
    if (cond) { tests_passed++; }                                               \
    else { tests_failed++; std::printf("FAIL "); std::printf(__VA_ARGS__);      \
           std::printf("\n"); }                                                 \
} while (0)

// TradingView prints 12 decimals of its double; the engine's double must
// round to the same print (checked as 1e-9 relative, far below a price tick).
static bool near(double engine, double tv) {
    if (std::isnan(tv)) return std::isnan(engine);
    if (std::isnan(engine)) return false;
    return std::fabs(engine - tv) <= 1e-9 * std::fmax(1.0, std::fabs(tv));
}

// Rows 1..3: series factor (first bar 3, then 1 .. 5.5 and a regime switch),
// a factor of 2 on bar 0 only, an na factor on bar 0, and a later 0 / -2
// factor that is never read: line and direction equal TradingView's on every
// bar, the warm-up (line 0 on bar 0, na until the ATR is formed, direction 1)
// included.
static void rows_tapes() {
    int bars = 0;
    for (const SupertrendSite& site : kSupertrendSites) {
        source::PineSupertrend st;
        int matched = 0;
        int shown = 0;
        for (int b = 0; b < site.bars; ++b) {
            const ta::SupertrendResult r =
                st.compute(site.factor[b], 10, site.high[b], site.low[b], site.close[b]);
            const bool ok = near(r.value, site.tv_line[b]) && near(r.direction, site.tv_direction[b]);
            if (ok) {
                ++matched;
            } else if (shown++ < 4) {
                std::printf("  %s: bar %d engine (%.12g, %g) TradingView (%.12g, %g)\n", site.name,
                            b, r.value, r.direction, site.tv_line[b], site.tv_direction[b]);
            }
        }
        CHECK(matched == site.bars, "%s: %d/%d bars", site.name, matched, site.bars);
        std::printf("row %s: %d/%d\n", site.name, matched, site.bars);
        bars += site.bars;
    }
    CHECK(bars == 1200, "1,200 recorded bars replayed (got %d)", bars);
}

// Row 4: the latched factor is the one TradingView checks. 0 or a negative
// first factor stops the run on its first execution (tv/edge st-factor-zero
// and st-factor-neg: RE10001 on bar 0); so does an atrPeriod that is not a
// positive int.
static std::string first_call_error(double factor, double atr_period) {
    ta::BarContextScope scope(0, 0);
    try {
        source::PineSupertrend st;
        st.compute(factor, atr_period, 2.0, 1.0, 1.5);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return std::string();
}

static void row_refusals() {
    CHECK(first_call_error(0.0, 10) ==
              "Error on bar 0: Invalid value of the 'factor' argument (0) in the 'supertrend' "
              "function. It must be > 0.",
          "factor 0: %s", first_call_error(0.0, 10).c_str());
    CHECK(first_call_error(-2.0, 10) ==
              "Error on bar 0: Invalid value of the 'factor' argument (-2) in the 'supertrend' "
              "function. It must be > 0.",
          "factor -2: %s", first_call_error(-2.0, 10).c_str());
    CHECK(first_call_error(na<double>(), 10).empty(), "an na factor is accepted");
    CHECK(first_call_error(3.0, 0) ==
              "Error on bar 0: Invalid value of the 'atrPeriod' argument (0) in the 'supertrend' "
              "function. It must be > 0.",
          "atrPeriod 0: %s", first_call_error(3.0, 0).c_str());
}

// Row 5: recompute() re-applies the bar from the state it found, so ticks of
// one bar end where a single compute of the bar's last values ends.
static void row_recompute() {
    const SupertrendSite& site = kSupertrendSites[0];
    source::PineSupertrend ticks;
    source::PineSupertrend closes;
    bool equal = true;
    for (int b = 0; b < site.bars; ++b) {
        ticks.compute(site.factor[b], 10, site.high[b] + 5.0, site.low[b] - 5.0, site.close[b] - 3.0);
        const ta::SupertrendResult t =
            ticks.recompute(site.factor[b], 10, site.high[b], site.low[b], site.close[b]);
        const ta::SupertrendResult c =
            closes.compute(site.factor[b], 10, site.high[b], site.low[b], site.close[b]);
        const bool same_value = (std::isnan(t.value) && std::isnan(c.value)) || t.value == c.value;
        if (!same_value || t.direction != c.direction) equal = false;
    }
    CHECK(equal, "compute + recompute on one bar answers the bar's last values");
}

int main() {
    rows_tapes();
    row_refusals();
    row_recompute();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
