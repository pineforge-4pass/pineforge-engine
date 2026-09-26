// test_ta_series_length_extremes -- ta.highest / ta.lowest / ta.highestbars /
// ta.lowestbars with a SERIES length (pineforge::source::SeriesHighest,
// SeriesLowest, SeriesHighestBars, SeriesLowestBars), replayed call by call
// against TradingView's answers.
//
// Every site of test_ta_series_length_data.hpp is a lab tv export of a
// synthetic probe (K-TA-DYNLEN, 2026-09-26) whose Signal strings carry the
// bar's source, the call's length, whether the call ran, and TradingView's
// value and *Bars offset: 35 sites, 5,647 calls, BINANCE:BTCUSDT 15 and
// NYSE:F 15 from 2025-04-01 (bar 0 = the range's first bar) plus two
// synthetic sources. The rows below replay them bar by bar under the bar
// context the engine installs, and check the refusals TradingView raises.

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

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

static bool same(double engine, double tv) {
    if (std::isnan(tv)) return std::isnan(engine);
    return engine == tv;
}

struct Replay {
    int calls = 0;
    int matched = 0;
};

// One site: a value object and an offset object, both fed the site's calls
// under the bar context of the tape's bar.
static Replay replay_site(const SeriesExtremeSite& site) {
    Replay r;
    source::SeriesHighest hi;
    source::SeriesLowest lo;
    source::SeriesHighestBars hib;
    source::SeriesLowestBars lob;
    int shown = 0;
    for (int b = 0; b < site.bars; ++b) {
        if (site.call && !site.call[b]) continue;
        ta::BarContextScope scope(b, 0);
        const double len = static_cast<double>(site.length[b]);
        const double v = site.want_max ? hi.compute(site.src[b], len) : lo.compute(site.src[b], len);
        const double o = site.tv_offset
            ? (site.want_max ? hib.compute(site.src[b], len) : lob.compute(site.src[b], len))
            : na<double>();
        ++r.calls;
        const bool ok = same(v, site.tv_value[b]) && (!site.tv_offset || same(o, site.tv_offset[b]));
        if (ok) {
            ++r.matched;
        } else if (shown++ < 4) {
            std::printf("  %s: bar %d length %d engine (%.12g, %g) TradingView (%.12g, %g)\n",
                        site.name, b, site.length[b], v, o, site.tv_value[b],
                        site.tv_offset ? site.tv_offset[b] : na<double>());
        }
    }
    return r;
}

// Row 1..3: every recorded call of every site equals TradingView's answer.
static void rows_tapes() {
    int calls = 0;
    int matched = 0;
    for (const SeriesExtremeSite& site : kSeriesExtremeSites) {
        const Replay r = replay_site(site);
        CHECK(r.calls > 0 && r.matched == r.calls, "%s: %d/%d calls equal TradingView", site.name,
              r.matched, r.calls);
        std::printf("row %s: %d/%d\n", site.name, r.matched, r.calls);
        calls += r.calls;
        matched += r.matched;
    }
    CHECK(calls == 5647, "5,647 recorded calls replayed (got %d)", calls);
    std::printf("all sites: %d/%d calls equal TradingView\n", matched, calls);
}

static std::string error_of(double length, bool want_max, long long bar) {
    ta::BarContextScope scope(bar, 0);
    try {
        if (want_max) {
            source::SeriesHighest h;
            h.compute(1.0, length);
        } else {
            source::SeriesLowest l;
            l.compute(1.0, length);
        }
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return std::string();
}

// Row 4: a length of 0, a negative or na length, and a window reaching more
// than 15000 bars back stop the run with TradingView's error (tv/edge:
// win-zero, win-neg, win-na, lowest-big20000; lowest-big4990 / 5001 / 9000
// answered in full).
static void row_refusals() {
    CHECK(error_of(0, false, 50) ==
              "Error on bar 50: Invalid value of the 'length' argument (0) in the 'lowest' "
              "function. It must be > 0.",
          "length 0 refused: %s", error_of(0, false, 50).c_str());
    CHECK(error_of(-3, false, 50) ==
              "Error on bar 50: Invalid value of the 'length' argument (-3) in the 'lowest' "
              "function. It must be > 0.",
          "length -3 refused: %s", error_of(-3, false, 50).c_str());
    CHECK(error_of(na<double>(), true, 50) ==
              "Error on bar 50: Invalid value of the 'length' argument (0) in the 'highest' "
              "function. It must be > 0.",
          "na length refused as 0: %s", error_of(na<double>(), true, 50).c_str());
    CHECK(error_of(15001, false, 20000).empty(), "a 15001-bar window (15000 back) is served");
    CHECK(error_of(20000, false, 20100) ==
              "Error on bar 20100: The script attempts to reference historical data that is "
              "too far from the current bar (19999 bars back). The historical buffer's limit "
              "is 15000 bars.",
          "a 20000-bar window refused: %s", error_of(20000, false, 20100).c_str());
    {
        source::SeriesLowestBars b;
        ta::BarContextScope scope(5, 0);
        bool threw = false;
        try { b.compute(1.0, 0); } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("'lowestbars' function") != std::string::npos;
        }
        CHECK(threw, "lowestbars names its own function");
    }
}

// Row 5: long windows read history as far as 15000 bars back, across the
// shared chunks of the held history (tv/edge lowest-big4990: a 4990-bar
// window on bar 5000 answers the run's lowest source).
static void row_long_window() {
    source::SeriesLowest lo;
    source::SeriesLowestBars lob;
    double v = 0.0;
    double o = 0.0;
    for (int b = 0; b <= 16000; ++b) {
        ta::BarContextScope scope(b, 0);
        const double src = 1000.0 + ((b * 37) % 101) - (b == 1234 ? 900.0 : 0.0);
        const double len = b == 5000 ? 4990 : b == 16000 ? 15001 : 5;
        v = lo.compute(src, len);
        o = lob.compute(src, len);
        if (b == 5000) {
            CHECK(v == 100.0 + ((1234 * 37) % 101) && o == -(5000 - 1234),
                  "bar 5000, length 4990: %g at %g", v, o);
        }
    }
    CHECK(v == 100.0 + ((1234 * 37) % 101) && o == -(16000 - 1234),
          "bar 16000, length 15001: bar 1234, 14766 bars back, %g at %g", v, o);
}

// Row 6: a copy taken mid-run (the script-state checkpoint takes one every
// bar) continues exactly as the original, and the original is not disturbed
// by the copy's later writes.
static void row_copies() {
    source::SeriesHighestBars a;
    std::vector<double> tail_a;
    std::vector<double> tail_b;
    source::SeriesHighestBars b_copy;
    for (int b = 0; b < 5000; ++b) {
        ta::BarContextScope scope(b, 0);
        const double src = std::sin(b * 0.37) * 50.0 + b * 0.01;
        const double len = 1 + (b * 7) % 1400;
        if (b == 3000) b_copy = a;
        const double x = a.compute(src, len);
        if (b >= 3000) {
            tail_a.push_back(x);
            tail_b.push_back(b_copy.compute(src, len));
        }
    }
    CHECK(tail_a == tail_b, "a checkpoint copy replays identically (%zu calls)", tail_a.size());
    source::SeriesHighestBars fresh;
    std::vector<double> tail_c;
    for (int b = 0; b < 5000; ++b) {
        ta::BarContextScope scope(b, 0);
        const double src = std::sin(b * 0.37) * 50.0 + b * 0.01;
        const double x = fresh.compute(src, 1 + (b * 7) % 1400);
        if (b >= 3000) tail_c.push_back(x);
    }
    CHECK(tail_c == tail_a, "and equals an uncopied run");
}

// Row 7: recompute() re-applies the current bar from the state the bar found
// (an intrabar tick), so compute(x1) + recompute(x2) answers compute(x2).
static void row_recompute() {
    source::SeriesLowest ticks;
    source::SeriesLowest closes;
    bool all_equal = true;
    for (int b = 0; b < 400; ++b) {
        ta::BarContextScope scope(b, 0);
        const double final_src = 100.0 + ((b * 13) % 17);
        const double len = 1 + (b * 5) % 9;
        ticks.compute(final_src - 50.0, len);
        ticks.recompute(final_src + 7.0, len);
        const double t = ticks.recompute(final_src, len);
        const double c = closes.compute(final_src, len);
        if (!same(t, c)) all_equal = false;
    }
    CHECK(all_equal, "compute + recompute on one bar answers the bar's last source");
}

// Row 8: with no bar context installed each compute() is its own bar (unit
// use), exactly as ta::ExtremeRing counts.
static void row_no_context() {
    source::SeriesHighest h;
    const double a = h.compute(5.0, 2);   // bar 0: warm-up
    const double b = h.compute(7.0, 2);   // bar 1: max(5, 7)
    const double c = h.recompute(3.0, 2); // bar 1 again: max(5, 3)
    const double d = h.compute(4.0, 2);   // bar 2: max(3, 4)
    CHECK(std::isnan(a) && b == 7.0 && c == 5.0 && d == 4.0, "no-context cadence %g %g %g %g", a,
          b, c, d);
}

int main() {
    rows_tapes();
    row_refusals();
    row_long_window();
    row_copies();
    row_recompute();
    row_no_context();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
