#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/ta.hpp>

using namespace pineforge;

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) < tol;
}

struct SarBar {
    double high;
    double low;
    double close;
};

static std::vector<double> pine_sar_reference(const std::vector<SarBar>& bars,
                                              double start,
                                              double inc,
                                              double maxv) {
    std::vector<double> out;
    out.reserve(bars.size());

    double result = na<double>();
    double max_min = na<double>();
    double acceleration = na<double>();
    bool is_below = false;

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const SarBar& bar = bars[i];
        bool is_first_trend_bar = false;

        if (i == 0) {
            out.push_back(na<double>());
            continue;
        }

        if (i == 1) {
            if (bar.close > bars[i - 1].close) {
                is_below = true;
                max_min = bar.high;
                result = bars[i - 1].low;
            } else {
                is_below = false;
                max_min = bar.low;
                result = bars[i - 1].high;
            }
            acceleration = start;
            is_first_trend_bar = true;
        }

        result = result + acceleration * (max_min - result);

        if (is_below) {
            if (result > bar.low) {
                is_first_trend_bar = true;
                is_below = false;
                result = std::max(bar.high, max_min);
                max_min = bar.low;
                acceleration = start;
            }
        } else {
            if (result < bar.high) {
                is_first_trend_bar = true;
                is_below = true;
                result = std::min(bar.low, max_min);
                max_min = bar.high;
                acceleration = start;
            }
        }

        if (!is_first_trend_bar) {
            if (is_below) {
                if (bar.high > max_min) {
                    max_min = bar.high;
                    acceleration = std::min(acceleration + inc, maxv);
                }
            } else {
                if (bar.low < max_min) {
                    max_min = bar.low;
                    acceleration = std::min(acceleration + inc, maxv);
                }
            }
        }

        if (is_below) {
            result = std::min(result, bars[i - 1].low);
            if (i > 1) {
                result = std::min(result, bars[i - 2].low);
            }
        } else {
            result = std::max(result, bars[i - 1].high);
            if (i > 1) {
                result = std::max(result, bars[i - 2].high);
            }
        }

        out.push_back(result);
    }

    return out;
}

static int test_linreg_na_in_window() {
    int fails = 0;
    ta::Linreg lr(3);
    double nanv = std::numeric_limits<double>::quiet_NaN();
    lr.compute(1.0, 0.0);
    lr.compute(2.0, 0.0);
    double a = lr.compute(nanv, 0.0);
    if (!is_na(a)) {
        std::printf("FAIL linreg: expected na when window contains na\n");
        fails++;
    }
    lr.compute(4.0, 0.0);
    lr.compute(5.0, 0.0);
    double c = lr.compute(6.0, 0.0);
    if (is_na(c)) {
        std::printf("FAIL linreg: expected finite when window is all finite\n");
        fails++;
    }
    return fails;
}

static int test_percentrank_pine_semantics() {
    int fails = 0;
    {
        ta::PercentRank pr(3);
        double s[] = {1, 2, 3, 4};
        pr.compute(s[0]);
        pr.compute(s[1]);
        double o2 = pr.compute(s[2]);
        if (!is_na(o2)) {
            std::printf("FAIL percentrank: expected na at warmup\n");
            fails++;
        }
        double o3 = pr.compute(s[3]);
        if (!near(o3, 100.0)) {
            std::printf("FAIL percentrank basic: got %g want 100\n", o3);
            fails++;
        }
    }
    {
        // Two na in lookback: valid priors 20 and 30; current 40; both <= 40 -> 100%
        ta::PercentRank pr(4);
        double nanv = std::numeric_limits<double>::quiet_NaN();
        double seq[] = {10, 20, nanv, nanv, 30, 40};
        double last = na<double>();
        for (double v : seq) {
            last = pr.compute(v);
        }
        if (!near(last, 100.0)) {
            std::printf("FAIL percentrank na history: got %g want 100\n", last);
            fails++;
        }
    }
    {
        ta::PercentRank pr(3);
        double seq[] = {10, 20, 30, 25};
        double last = na<double>();
        for (double v : seq) {
            last = pr.compute(v);
        }
        if (!near(last, 200.0 / 3.0)) {
            std::printf("FAIL percentrank denom: got %g want %g\n", last, 200.0 / 3.0);
            fails++;
        }
    }
    return fails;
}

static int test_ema_seeds_from_first_value() {
    int fails = 0;
    ta::EMA ema(5);
    const double alpha = 2.0 / 6.0;
    const double src[] = {10.0, 11.0, 13.0, 12.0, 14.0};

    double expected = na<double>();
    for (int i = 0; i < 5; ++i) {
        double got = ema.compute(src[i]);
        if (i == 0) {
            expected = src[i];
        } else {
            expected = alpha * src[i] + (1.0 - alpha) * expected;
        }
        if (!near(got, expected, 1e-12)) {
            std::printf("FAIL ema seed: i=%d got=%g want=%g\n", i, got, expected);
            fails++;
        }
    }
    return fails;
}

static int test_ema_ignores_na_inputs() {
    int fails = 0;
    ta::EMA ema(3);  // alpha = 0.5

    double a = ema.compute(10.0);
    if (!near(a, 10.0, 1e-12)) {
        std::printf("FAIL ema na ignore: first value got=%g want=10\n", a);
        fails++;
    }

    // KI-66: ta.ema RETURNS NA on the na-input bar. The na neither updates nor
    // resets the recursion — the prior value survives as STATE (not as this
    // bar's output), which `c` below confirms by resuming the recursion from 10.
    double b = ema.compute(na<double>());
    if (!is_na(b)) {
        std::printf("FAIL ema na rule: na input should return na, got=%g want=na\n", b);
        fails++;
    }

    double c = ema.compute(14.0);
    if (!near(c, 12.0, 1e-12)) {
        std::printf("FAIL ema na ignore: post-na update got=%g want=12\n", c);
        fails++;
    }

    ta::EMA ema2(3);
    double l0 = ema2.compute(na<double>());
    if (!is_na(l0)) {
        std::printf("FAIL ema na ignore: leading na should return na\n");
        fails++;
    }
    double l1 = ema2.compute(5.0);
    if (!near(l1, 5.0, 1e-12)) {
        std::printf("FAIL ema na ignore: first non-na after leading na got=%g want=5\n", l1);
        fails++;
    }

    return fails;
}

static int test_sar_matches_pine_reference_initialization() {
    int fails = 0;
    ta::SAR sar(0.02, 0.02, 0.2);

    const std::vector<SarBar> bars = {
        {11.0,  9.0, 10.0},
        {12.0, 10.0, 12.0},
        {13.0, 11.0, 11.0},
        {11.5,  9.5, 10.0},
        {10.5,  8.5,  9.0},
        { 9.5,  8.8,  9.3},
    };
    const std::vector<double> expected = pine_sar_reference(bars, 0.02, 0.02, 0.2);

    for (std::size_t i = 0; i < bars.size(); ++i) {
        double got = sar.compute(bars[i].high, bars[i].low, bars[i].close);
        if (!near(got, expected[i], 1e-12)) {
            std::printf(
                "FAIL sar reference: i=%zu got=%g want=%g\n",
                i,
                got,
                expected[i]
            );
            fails++;
        }
    }

    return fails;
}

// TR class: handle_na default (false) returns na on the first bar, matching
// TradingView v6's documented ta.tr(handle_na) behaviour. handle_na = true
// preserves the legacy v4-style (high - low) first-bar fallback.
static int test_tr_handle_na_default_returns_na_on_first_bar() {
    int fails = 0;
    {
        ta::TR tr;  // default-constructed: handle_na = false
        double v = tr.compute(100.0, 90.0, 95.0);
        if (!is_na(v)) {
            std::printf("FAIL tr default: first bar got %g, want na\n", v);
            fails++;
        }
    }
    {
        ta::TR tr(false);  // explicit handle_na = false
        double v = tr.compute(100.0, 90.0, 95.0);
        if (!is_na(v)) {
            std::printf("FAIL tr explicit false: first bar got %g, want na\n", v);
            fails++;
        }
    }
    {
        ta::TR tr(true);  // legacy handle_na = true
        double v = tr.compute(100.0, 90.0, 95.0);
        if (!near(v, 10.0)) {
            std::printf("FAIL tr handle_na=true: first bar got %g, want 10.0\n", v);
            fails++;
        }
    }
    return fails;
}

// Both forms must compute identical TR values once a previous close exists
// (handle_na only affects the very first bar).
static int test_tr_subsequent_bars_match_regardless_of_handle_na() {
    int fails = 0;
    ta::TR tr_default;
    ta::TR tr_legacy(true);

    const double bars[][3] = {
        // {high, low, close}
        {100.0,  90.0,  95.0},
        {103.0,  97.0, 102.0},
        {101.0,  98.0,  99.0},
        {105.0, 100.0, 104.0},
    };
    const int n = sizeof(bars) / sizeof(bars[0]);

    for (int i = 0; i < n; ++i) {
        double a = tr_default.compute(bars[i][0], bars[i][1], bars[i][2]);
        double b = tr_legacy.compute(bars[i][0], bars[i][1], bars[i][2]);
        if (i == 0) {
            // Documented divergence on bar 0; values asserted in the dedicated
            // first-bar test above.
            continue;
        }
        if (!near(a, b)) {
            std::printf(
                "FAIL tr parity bar=%d: default=%g legacy=%g\n", i, a, b
            );
            fails++;
        }
    }
    return fails;
}

int main() {
    int fails = test_linreg_na_in_window()
              + test_percentrank_pine_semantics()
              + test_ema_seeds_from_first_value()
              + test_ema_ignores_na_inputs()
              + test_sar_matches_pine_reference_initialization()
              + test_tr_handle_na_default_returns_na_on_first_bar()
              + test_tr_subsequent_bars_match_regardless_of_handle_na();
    if (fails > 0) {
        std::printf("test_ta: %d TA checks failed\n", fails);
        return 1;
    }

    ta::RSI rsi(14);

    double prices[] = {
        44.0,  44.34, 44.09, 43.61, 44.33,
        44.83, 45.10, 45.42, 45.84, 46.08,
        45.89, 46.03, 45.61, 46.28, 46.28,
        46.00, 46.03, 46.41, 46.22, 45.64,
        46.21, 46.25, 45.71, 46.45, 45.78,
        45.35, 44.03, 44.18, 44.22, 44.57
    };

    int n = sizeof(prices) / sizeof(prices[0]);

    printf("Bar | Close  | RSI\n");
    printf("----+--------+--------\n");

    for (int i = 0; i < n; i++) {
        double val = rsi.compute(prices[i]);
        if (is_na(val)) {
            printf("%3d | %6.2f | na\n", i, prices[i]);
        } else {
            printf("%3d | %6.2f | %6.2f\n", i, prices[i], val);
        }
    }

    return 0;
}
