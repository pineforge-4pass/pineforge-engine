// test_ta_simple_length -- ta.* calls whose length is SIMPLE (fixed for the
// run, yet neither a constant nor an input: a syminfo-dependent preset, an
// input-string ternary) and the checked simple length
// (pineforge::source::FirstCallBound, simple_ta_length).
//
// TradingView answers a simple length exactly as the constant: the rows below
// replay the K-TA-DYNLEN lab tv tapes (synthetic probes, BINANCE:BTCUSDT 15
// with length 9 and NYSE:F 15 with length 14, from 2025-04-01) through the
// fixed-length classes built by FirstCallBound, and the sparse const /
// simple / series probe through the ring of ta::Lowest / ta::HighestBars /
// ta::SMA and through the series classes.

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdint>
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

// TradingView prints 12 decimals of its double.
static bool near(double engine, double tv) {
    if (std::isnan(tv)) return std::isnan(engine);
    if (std::isnan(engine)) return false;
    return std::fabs(engine - tv) <= 1e-9 * std::fmax(1.0, std::fabs(tv));
}

static bool same_bits(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Row 1: ta.rsi / ta.rma / ta.atr with a simple length. The object is built
// from the first call's length and never rebuilt; its answers equal the
// constant-length class bit for bit and TradingView's on every bar (the chart
// ta.atr's high and low come from the same chart's win-every tape, whose
// closes equal this tape's).
// Row 2: ta.ema with a simple length: FirstCallBound builds ta::EMA exactly
// as a constant length does (bit for bit, either seeding); TradingView's
// ta.ema is the SMA-seeded one (na until `length` closes).
static void rows_recursive() {
    for (const SimpleLengthSite& site : kSimpleLengthSites) {
        source::FirstCallBound<ta::RSI> rsi;
        source::FirstCallBound<ta::RMA> rma;
        source::FirstCallBound<ta::ATR> atr;
        source::FirstCallBound<ta::EMA> ema;
        source::FirstCallBound<ta::EMA> ema_sma;
        ta::RSI rsi_const(site.length);
        ta::RMA rma_const(site.length);
        ta::ATR atr_const(site.length);
        ta::EMA ema_const(site.length);
        int built = 0;
        int rsi_ok = 0, rma_ok = 0, atr_ok = 0, ema_tv = 0, same_const = 0;
        for (int b = 0; b < site.bars; ++b) {
            // The length expression runs on the first call only.
            const auto length_of = [&]() {
                ++built;
                return source::simple_ta_length(site.length, "rsi", "length");
            };
            const double r = rsi.bind([&] { return ta::RSI(length_of()); }).compute(site.close[b]);
            const double m = rma.bind([&] { return ta::RMA(site.length); }).compute(site.close[b]);
            const double a = atr.bind([&] { return ta::ATR(site.length); })
                                 .compute(site.high[b], site.low[b], site.close[b]);
            const double e = ema.bind([&] { return ta::EMA(site.length); }).compute(site.close[b]);
            const double es = ema_sma
                                  .bind([&] {
                                      return ta::EMA(site.length, ta::EmaSeeding::SimpleAverage);
                                  })
                                  .compute(site.close[b]);
            rsi_ok += near(r, site.tv_rsi[b]);
            rma_ok += near(m, site.tv_rma[b]);
            atr_ok += near(a, site.tv_atr[b]);
            ema_tv += near(es, site.tv_ema[b]);
            same_const += same_bits(r, rsi_const.compute(site.close[b])) &&
                          same_bits(m, rma_const.compute(site.close[b])) &&
                          same_bits(a, atr_const.compute(site.high[b], site.low[b], site.close[b])) &&
                          same_bits(e, ema_const.compute(site.close[b]));
        }
        CHECK(built == 1, "%s: the length is read once (%d)", site.name, built);
        CHECK(rsi_ok == site.bars && rma_ok == site.bars && atr_ok == site.bars,
              "%s: rsi %d, rma %d, atr %d of %d bars equal TradingView", site.name, rsi_ok, rma_ok,
              atr_ok, site.bars);
        CHECK(ema_tv == site.bars, "%s: SMA-seeded ema %d/%d bars equal TradingView", site.name,
              ema_tv, site.bars);
        CHECK(same_const == site.bars, "%s: %d/%d bars equal the constant-length classes",
              site.name, same_const, site.bars);
        std::printf("row simple ta.rsi/ta.rma/ta.atr %s: %d/%d/%d of %d\n", site.name, rsi_ok,
                    rma_ok, atr_ok, site.bars);
        std::printf("row simple ta.ema %s: %d/%d (SMA-seeded), constant-identical %d/%d\n",
                    site.name, ema_tv, site.bars, same_const, site.bars);
    }
}

// Row 3: a sparse call (2 of every 9 bars) with a simple length reads the
// const-length ring -- never-written slots read 0 -- exactly as the constant
// (TradingView: const == simple on 46/46 calls per chart), while the series
// length reads the held history (SeriesLowest / SeriesHighestBars equal the
// series column). ta.sma is the same per-execution window for all three.
static void rows_qualifier() {
    for (const QualifierSite& site : kQualifierSites) {
        source::FirstCallBound<ta::Lowest> lo_simple;
        source::FirstCallBound<ta::HighestBars> hb_simple;
        source::FirstCallBound<ta::SMA> sma_simple;
        source::SeriesLowest lo_series;
        source::SeriesHighestBars hb_series;
        int calls = 0, simple_ok = 0, series_ok = 0, const_eq_simple = 0;
        for (int b = 0; b < site.bars; ++b) {
            if (!site.call[b]) continue;
            ta::BarContextScope scope(b, 0);
            ++calls;
            const double l = lo_simple.bind([] { return ta::Lowest(5); }).compute(site.low[b]);
            const double h = hb_simple.bind([] { return ta::HighestBars(5); }).compute(site.high[b]);
            const double m = sma_simple.bind([] { return ta::SMA(5); }).compute(site.close[b]);
            const double ls = lo_series.compute(site.low[b], 5);
            const double hs = hb_series.compute(site.high[b], 5);
            simple_ok += near(l, site.tv_lowest_simple[b]) && near(h, site.tv_hbars_simple[b]) &&
                         near(m, site.tv_sma_simple[b]);
            series_ok += near(ls, site.tv_lowest_series[b]) && near(hs, site.tv_hbars_series[b]);
            const_eq_simple += same_bits(site.tv_lowest_const[b], site.tv_lowest_simple[b]) &&
                               same_bits(site.tv_hbars_const[b], site.tv_hbars_simple[b]) &&
                               same_bits(site.tv_sma_const[b], site.tv_sma_simple[b]);
        }
        CHECK(calls == 46 && const_eq_simple == calls, "%s: TradingView const == simple %d/%d",
              site.name, const_eq_simple, calls);
        CHECK(simple_ok == calls, "%s: simple ring %d/%d", site.name, simple_ok, calls);
        CHECK(series_ok == calls, "%s: series held history %d/%d", site.name, series_ok, calls);
        std::printf("row simple window (ring) %s: %d/%d, series %d/%d\n", site.name, simple_ok,
                    calls, series_ok, calls);
    }
}

// Row 4: a simple length that is not a positive int stops the run on the
// call's first execution with TradingView's text (tv/edge rsi-simple-* /
// ema-simple-*: RE10001 for 0 and -3, RE10003 for na).
static std::string error_of(double value) {
    ta::BarContextScope scope(0, 0);
    try {
        source::simple_ta_length(value, "rsi", "length");
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return std::string();
}

static void row_refusals() {
    CHECK(error_of(0) ==
              "Error on bar 0: Invalid value of the 'length' argument (0) in the 'rsi' function. "
              "It must be > 0.",
          "0: %s", error_of(0).c_str());
    CHECK(error_of(-3) ==
              "Error on bar 0: Invalid value of the 'length' argument (-3) in the 'rsi' function. "
              "It must be > 0.",
          "-3: %s", error_of(-3).c_str());
    CHECK(error_of(na<double>()) ==
              "Error on bar 0: Invalid value of the 'length' argument in the 'rsi' function. It "
              "must not be na",
          "na: %s", error_of(na<double>()).c_str());
    bool int_na = false;
    try {
        source::simple_ta_length(na<int>(), "ema", "length");
    } catch (const std::runtime_error& e) {
        int_na = std::string(e.what()).find("must not be na") != std::string::npos;
    }
    CHECK(int_na, "an int na is na");
    bool i64_na = false;
    try {
        source::simple_ta_length(na<std::int64_t>(), "ema", "length");
    } catch (const std::runtime_error& e) {
        i64_na = std::string(e.what()).find("must not be na") != std::string::npos;
    }
    CHECK(i64_na, "an int64 na is na");
    CHECK(source::simple_ta_length(14.9, "rsi", "length") == 14, "a float length truncates");
    CHECK(source::simple_ta_length(7, "rsi", "length") == 7, "an int length passes");
}

// Row 5: the generated call spelling -- compute(make, args...) binds on the
// first call and forwards -- and ta_number, which carries an integer na into
// the double the lowered classes take (the int / int64 na sentinels are not
// NaN).
static void row_generated_spelling() {
    const SimpleLengthSite& site = kSimpleLengthSites[0];
    source::FirstCallBound<ta::RSI> spelled;
    source::FirstCallBound<ta::RSI> bound;
    int built = 0;
    bool equal = true;
    for (int b = 0; b < site.bars; ++b) {
        const auto make = [&]() {
            ++built;
            return ta::RSI(source::simple_ta_length(site.length, "rsi", "length"));
        };
        const double x = b % 3 == 2 ? spelled.recompute(make, site.close[b])
                                     : spelled.compute(make, site.close[b]);
        if (b % 3 == 2) {
            // recompute rewrites the bar the previous compute opened
            bound.bind(make).recompute(site.close[b]);
        }
        const double y = b % 3 == 2 ? bound.bind(make).recompute(site.close[b])
                                     : bound.bind(make).compute(site.close[b]);
        if (!same_bits(x, y)) equal = false;
    }
    CHECK(equal && built == 2, "compute(make, ...) == bind(make).compute(...) (built %d)", built);
    CHECK(std::isnan(source::ta_number(na<int>())) && std::isnan(source::ta_number(na<std::int64_t>())) &&
              source::ta_number(5) == 5.0 && source::ta_number(std::int64_t{7}) == 7.0 &&
              source::ta_number(2.5) == 2.5,
          "ta_number keeps an integer na");
    std::string text;
    try {
        ta::BarContextScope scope(3, 0);
        source::SeriesLowest lo;
        lo.compute(1.0, source::ta_number(na<int>()));
    } catch (const std::runtime_error& e) {
        text = e.what();
    }
    CHECK(text.find("argument (0) in the 'lowest' function") != std::string::npos,
          "an int na series length is refused as 0: %s", text.c_str());
}

int main() {
    rows_recursive();
    rows_qualifier();
    row_refusals();
    row_generated_spelling();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
