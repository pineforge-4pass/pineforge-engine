#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
#include <limits>

#include <pineforge/ta.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/timeframe.hpp>
#include <pineforge/magnifier.hpp>
#include <pineforge/na.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/series.hpp>

using namespace pineforge;

// ---- helpers ----------------------------------------------------------------

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

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) < tol;
}

// ---- 1. Composed TA indicators: EMA of SMA ---------------------------------

static void test_ema_of_sma() {
    std::printf("test_ema_of_sma\n");
    ta::SMA sma(3);
    ta::EMA ema(5);

    double prices[] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    double results[11];

    for (int i = 0; i < 11; i++) {
        double sma_val = sma.compute(prices[i]);
        double ema_val = ema.compute(sma_val);
        results[i] = ema_val;
    }

    // SMA(3) needs 3 bars to produce first value (index 2).
    // Pine EMA seeds from the first non-na input, so EMA(SMA) is valid from index 2.
    for (int i = 2; i < 11; i++) {
        CHECK(!is_na(results[i]));
        CHECK(std::isfinite(results[i]));
    }

    // EMA(SMA) is a lagging indicator - should lag behind the linearly rising price
    CHECK(results[10] < prices[10]);
}

// ---- 2. Composed recompute --------------------------------------------------

static void test_composed_recompute() {
    std::printf("test_composed_recompute\n");
    ta::SMA sma1(3), sma2(3);
    ta::EMA ema1(3), ema2(3);

    double data[] = {10, 11, 12, 13, 14};

    // Feed 4 bars identically
    for (int i = 0; i < 4; i++) {
        double s1 = sma1.compute(data[i]);
        ema1.compute(s1);
        double s2 = sma2.compute(data[i]);
        ema2.compute(s2);
    }

    // Bar 5: instance 1 computes with 14, then recomputes with 16
    double s1 = sma1.compute(data[4]);
    ema1.compute(s1);
    double s1r = sma1.recompute(16);
    double recomp = ema1.recompute(s1r);

    // Instance 2 computes directly with 16
    double s2 = sma2.compute(16);
    double direct = ema2.compute(s2);

    CHECK(near(recomp, direct));
}

// ---- 3. RSI of custom source (hl2) -----------------------------------------

static void test_rsi_of_hl2() {
    std::printf("test_rsi_of_hl2\n");
    ta::RSI rsi(14);
    double results[20];

    for (int i = 0; i < 20; i++) {
        double high = 100 + i + (i % 3);
        double low  = 100 + i - (i % 3);
        double hl2  = (high + low) / 2.0;
        results[i] = rsi.compute(hl2);
    }

    // After warmup (14 bars), RSI should be in [0, 100]
    for (int i = 14; i < 20; i++) {
        CHECK(!is_na(results[i]));
        CHECK(results[i] >= 0.0 && results[i] <= 100.0);
    }
}

// ---- 4. Bollinger Bands of ATR output ---------------------------------------

static void test_bb_of_atr() {
    std::printf("test_bb_of_atr\n");
    ta::ATR atr(14);
    ta::BB bb(20, 2.0);

    for (int i = 0; i < 40; i++) {
        double h = 100 + i * 0.5 + (i % 5);
        double l = 100 + i * 0.5 - (i % 5);
        double c = (h + l) / 2.0;
        double atr_val = atr.compute(h, l, c);
        auto bb_result = bb.compute(atr_val);

        if (i >= 33) { // both fully warmed up
            CHECK(!is_na(bb_result.middle));
            CHECK(bb_result.upper > bb_result.middle);
            CHECK(bb_result.lower < bb_result.middle);
        }
    }
}

// ---- 5. TimeframeAggregator edge cases --------------------------------------

static void test_aggregator_single_bar() {
    std::printf("test_aggregator_single_bar\n");
    TimeframeAggregator agg(3);
    Bar b{100, 105, 95, 102, 50, 1000};
    auto r = agg.feed(b);
    CHECK(!r.is_complete);
}

static void test_aggregator_exact_ratio() {
    std::printf("test_aggregator_exact_ratio\n");
    TimeframeAggregator agg(2);
    Bar b1{100, 105, 95,  102, 50, 1000};
    Bar b2{102, 108, 100, 106, 60, 2000};
    Bar b3{106, 110, 104, 109, 70, 3000};

    auto r1 = agg.feed(b1);
    CHECK(!r1.is_complete);
    auto r2 = agg.feed(b2);
    CHECK(r2.is_complete);
    CHECK(near(r2.bar.open, 100));
    CHECK(near(r2.bar.close, 106));

    auto r3 = agg.feed(b3);
    CHECK(!r3.is_complete); // new group started
}

static void test_aggregator_volume_accumulation() {
    std::printf("test_aggregator_volume_accumulation\n");
    TimeframeAggregator agg(3);
    Bar b1{100, 105, 95,  102, 100, 1000};
    Bar b2{102, 108, 100, 106, 200, 2000};
    Bar b3{106, 110, 104, 109, 300, 3000};

    agg.feed(b1);
    agg.feed(b2);
    auto r = agg.feed(b3);
    CHECK(r.is_complete);
    CHECK(near(r.bar.volume, 600)); // 100+200+300
}

// ---- 6. Price path sampling edge cases --------------------------------------

static void test_flat_bar_sampling() {
    std::printf("test_flat_bar_sampling\n");
    Bar bar{100, 100, 100, 100, 500, 0};
    auto prices = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(prices.size() == 4);
    for (auto p : prices) CHECK(near(p, 100));
}

static void test_high_sample_count() {
    std::printf("test_high_sample_count\n");
    Bar bar{100, 110, 90, 105, 500, 0};
    auto prices = sample_price_path(bar, 100, MagnifierDistribution::UNIFORM);
    CHECK(prices.size() == 100);
    CHECK(near(prices[0], 100));
    CHECK(near(prices[99], 105));
    // All samples should be within [low, high]
    for (auto p : prices) {
        CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
    }
}

// ---- 7. Strategy engine - basic subclass ------------------------------------

class TestStrategy : public BacktestEngine {
public:
    int bar_count = 0;
    std::vector<double> close_history;

    TestStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar& bar) override {
        close_history.push_back(bar.close);
        bar_count++;
    }
};

static void test_engine_empty_bars() {
    std::printf("test_engine_empty_bars\n");
    TestStrategy strat;
    strat.run(nullptr, 0);
    CHECK(strat.bar_count == 0);
    CHECK(strat.trade_count() == 0);
}

static void test_engine_single_bar() {
    std::printf("test_engine_single_bar\n");
    TestStrategy strat;
    Bar bars[] = {{100, 105, 95, 102, 50, 1000000}};
    strat.run(bars, 1);
    CHECK(strat.bar_count == 1);
    CHECK(strat.close_history.size() == 1);
    CHECK(near(strat.close_history[0], 102));
}

// request.security with gaps_on + lookahead_off should return na on
// non-complete higher-timeframe bars.
static void test_request_security_gaps_on_emits_na_between_completions() {
    std::printf("test_request_security_gaps_on_emits_na_between_completions\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            register_security_eval(0, "60", "15", false, true);
        }

        void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
            if (sec_id == 0) {
                sec_val_ = bar.close;
            }
        }

        void clear_security(int sec_id) override {
            if (sec_id == 0) {
                sec_val_ = std::numeric_limits<double>::quiet_NaN();
            }
        }

        void on_bar(const Bar& bar) override {
            seen_.push_back(sec_val_);
        }

        const std::vector<double>& seen() const { return seen_; }

    private:
        double sec_val_ = std::numeric_limits<double>::quiet_NaN();
        std::vector<double> seen_;
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50,  900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000},
        {102.0, 103.0, 101.0, 102.0, 50, 2'700'000},
        {103.0, 104.0, 102.0, 103.0, 50, 3'600'000},  // first 60m completion
        {104.0, 105.0, 103.0, 104.0, 50, 4'500'000},
        {105.0, 106.0, 104.0, 105.0, 50, 3900'000},
        {106.0, 107.0, 105.0, 106.0, 50, 6'300'000},
        {107.0, 108.0, 106.0, 107.0, 50, 7'200'000},  // second 60m completion
    };
    strat.run(bars, 8, "15", "15", false, 4, MagnifierDistribution::ENDPOINTS);

    const auto& seen = strat.seen();
    CHECK(seen.size() == 8);
    CHECK(std::isnan(seen[0]));

    bool saw_non_nan = false;
    bool saw_reset_to_nan = false;
    for (double v : seen) {
        if (std::isnan(v)) {
            if (saw_non_nan) {
                saw_reset_to_nan = true;
            }
        } else {
            saw_non_nan = true;
        }
    }

    // gaps_on + lookahead_off should clear the cached value back to na
    // on non-complete bars between higher-timeframe completions.
    CHECK(saw_non_nan);
    CHECK(saw_reset_to_nan);
}

// With process_orders_on_close=false, priced orders created on a bar should be
// eligible only from the next bar (no retroactive same-bar fills).
static void test_priced_entry_not_filled_same_bar_when_pooc_false() {
    std::printf("test_priced_entry_not_filled_same_bar_when_pooc_false\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, na<double>(), 101.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 110.0, 90.0, 100.0, 50, 900'000},   // stop touched on creation bar
        {100.0, 100.0, 100.0, 100.0, 50, 1'800'000}, // not touched later
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}


static void test_priced_entry_fill_rounds_to_mintick() {
    std::printf("test_priced_entry_fill_rounds_to_mintick\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, na<double>(), 100.006);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 100.0, 99.0, 100.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).entry_price, 100.01, 1e-9));
}



static void test_barstate_flags_simple_run() {
    std::printf("test_barstate_flags_simple_run\n");

    class Strat : public BacktestEngine {
    public:
        std::vector<bool> isnew_values;
        std::vector<bool> isconfirmed_values;
        std::vector<bool> islast_values;

        void on_bar(const Bar& bar) override {
            (void)bar;
            isnew_values.push_back(is_first_tick_);
            isconfirmed_values.push_back(is_last_tick_);
            islast_values.push_back(barstate_islast_);
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000},
        {102.0, 103.0, 101.0, 102.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.isnew_values.size() == 3);
    CHECK(strat.isconfirmed_values.size() == 3);
    CHECK(strat.islast_values.size() == 3);
    CHECK(strat.isnew_values[0] && strat.isnew_values[1] && strat.isnew_values[2]);
    CHECK(strat.isconfirmed_values[0] && strat.isconfirmed_values[1] && strat.isconfirmed_values[2]);
    CHECK(!strat.islast_values[0]);
    CHECK(!strat.islast_values[1]);
    CHECK(strat.islast_values[2]);
}

static void test_barstate_flags_magnifier_run() {
    std::printf("test_barstate_flags_magnifier_run\n");

    class Strat : public BacktestEngine {
    public:
        std::vector<bool> isnew_values;
        std::vector<bool> isconfirmed_values;
        std::vector<bool> islast_values;

        void on_bar(const Bar& bar) override {
            (void)bar;
            isnew_values.push_back(is_first_tick_);
            isconfirmed_values.push_back(is_last_tick_);
            islast_values.push_back(barstate_islast_);
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 60'000},
        {101.0, 102.0, 100.0, 101.0, 50, 120'000},
        {102.0, 103.0, 101.0, 102.0, 50, 180'000},
        {103.0, 104.0, 102.0, 103.0, 50, 240'000},
    };
    strat.run(bars, 4, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);

    CHECK(strat.isnew_values.size() == 2);
    CHECK(strat.isconfirmed_values.size() == 2);
    CHECK(strat.islast_values.size() == 2);
    CHECK(strat.isnew_values[0] && strat.isnew_values[1]);
    CHECK(strat.isconfirmed_values[0] && strat.isconfirmed_values[1]);
    CHECK(!strat.islast_values[0]);
    CHECK(strat.islast_values[1]);
}

static void test_buy_stop_limit_requires_stop_before_limit_on_path() {
    std::printf("test_buy_stop_limit_requires_stop_before_limit_on_path\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                // Buy stop-limit: stop activates at 105, limit fills at 95 only after activation.
                strategy_entry("L", true, 95.0, 105.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        // Low is touched before high on the open-proximity path O->L->H->C.
        // The limit price exists in the bar before activation, so no fill should occur.
        {100.0, 110.0, 90.0, 108.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_buy_stop_limit_fills_when_limit_seen_after_activation() {
    std::printf("test_buy_stop_limit_fills_when_limit_seen_after_activation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, 95.0, 105.0);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("L");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        // Open is nearer high, so path O->H->L->C. Since open is already
        // above the stop, activation occurs at the open, then the later low
        // reaches the buy limit 95.
        {108.0, 110.0, 90.0, 100.0, 50, 1'800'000},
        {100.0, 101.0, 99.0, 100.0, 50, 2'700'000},
        {100.0, 101.0, 99.0, 100.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).entry_price, 95.0, 1e-9));
}


static void test_sell_stop_limit_requires_stop_before_limit_on_path() {
    std::printf("test_sell_stop_limit_requires_stop_before_limit_on_path\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                // Sell stop-limit: stop activates at 95, limit fills at 105 only after activation.
                strategy_entry("S", false, 105.0, 95.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        // High is touched before low on the open-proximity path O->H->L->C.
        // |110 - 101| = 9, |101 - 90| = 11.
        // The limit price exists in the bar before activation, so no fill should occur.
        {101.0, 110.0, 90.0, 92.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_sell_stop_limit_fills_when_limit_seen_after_activation() {
    std::printf("test_sell_stop_limit_fills_when_limit_seen_after_activation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("S", false, 105.0, 95.0);
            } else if (bar_index_ == 2 && signed_position_size() < 0.0) {
                strategy_close("S");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        // Open is nearer low, so path O->L->H->C. Since open is already below
        // the stop, activation occurs at open, then the later high reaches the sell limit 105.
        {92.0, 110.0, 90.0, 100.0, 50, 1'800'000},
        {100.0, 101.0, 99.0, 100.0, 50, 2'700'000},
        {100.0, 101.0, 99.0, 100.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).entry_price, 105.0, 1e-9));
}

// ---- 8. Strategy with risk limits -------------------------------------------

class RiskTestStrategy : public BacktestEngine {
public:
    RiskTestStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        risk_max_position_size_ = 2.0; // max 2 units
        pyramiding_ = 5;
    }

    void on_bar(const Bar& bar) override {
        // Try to pyramid every bar
        strategy_entry("Long", true);
    }

    double get_signed_position_size() const { return signed_position_size(); }
};

static void test_risk_max_position_size() {
    std::printf("test_risk_max_position_size\n");
    RiskTestStrategy strat;
    Bar bars[10];
    for (int i = 0; i < 10; i++)
        bars[i] = {100.0+i, 105.0+i, 95.0+i, 102.0+i, 50, (int64_t)(i+1)*60000LL};
    strat.run(bars, 10);
    // With max_position_size=2 and qty=1, should only have 2 entries
    CHECK(strat.get_signed_position_size() <= 2.0);
}

static void test_allow_entry_in_opposite_entry_closes_without_reversing() {
    std::printf("test_allow_entry_in_opposite_entry_closes_without_reversing\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            risk_direction_ = RiskDirection::LONG_ONLY;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("S_BLOCKED", false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50, 0},
        {101, 102, 100, 101, 50, 900'000},
        {102, 103, 101, 102, 50, 1'800'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).is_long);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_blocked_entry_does_not_consume_intraday_fill_quota() {
    std::printf("test_blocked_entry_does_not_consume_intraday_fill_quota\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 10;
            risk_direction_ = RiskDirection::LONG_ONLY;
            risk_max_position_size_ = 2.0;
            max_intraday_filled_orders_ = 3;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) strategy_entry("L1", true);
            if (bar_index_ == 1) strategy_entry("L2", true);
            if (bar_index_ == 2) strategy_entry("L3_BLOCKED", true);
            if (bar_index_ == 3) strategy_entry("S_BLOCKED", false);
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50, 0},
        {101, 102, 100, 101, 50, 900'000},
        {102, 103, 101, 102, 50, 1'800'000},
        {103, 104, 102, 103, 50, 2'700'000},
        {104, 105, 103, 104, 50, 3'600'000},
        {105, 106, 104, 105, 50, 4'500'000},
    };
    strat.run(bars, 6);

    CHECK(strat.trade_count() == 2);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

// Dual pending stop entries placed while flat: same bar, path touches both stops.
// Second touch must flatten (bracket-style), not reverse into a new position.
// This is the exact-zero-remainder case (both legs FIXED qty=1): confirmed by
// real corpus probe 80 (order-dual-stop-both-touch-priority-01, Trade 1 —
// entry long + exit long at the identical timestamp). When the opposite leg's
// qty is NOT equal (equity/price-based sizing, e.g. equity/priceA vs
// equity/priceB), TV instead defers the whole order to a later bar rather
// than flattening-with-remainder — see
// waranyutrkm-inside-day-breakout-strategy and classify_order_eligibility's
// flat_armed_opposite_close remainder check.
static void test_flat_bracket_dual_stop_closes_on_opposite_touch() {
    std::printf("test_flat_bracket_dual_stop_closes_on_opposite_touch\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("LE", true, na<double>(), 102.0);
                strategy_entry("SE", false, na<double>(), 98.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    // Wide range touches both bracket stops; path picks a first fill then the
    // opposite stop must close, not open a reverse position.
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        {101.0, 103.0, 97.0, 100.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

// Cross-bar bracket close: a flat-issued long+short stop pair where one leg
// fires earlier and the opposite leg fires several bars later must close the
// position, not reverse into a fresh opposite position. Regression for
// validation probes 80/81/86/87.
static void test_flat_bracket_dual_stop_cross_bar_closes_on_opposite_touch() {
    std::printf("test_flat_bracket_dual_stop_cross_bar_closes_on_opposite_touch\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("LE", true, na<double>(), 105.0);
                strategy_entry("SE", false, na<double>(), 95.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        {100.0, 102.0, 94.0, 99.0, 50, 1'800'000},
        {99.0, 100.0, 96.0, 98.0, 50, 2'700'000},
        {98.0, 99.0, 96.0, 97.0, 50, 3'600'000},
        {97.0, 106.0, 96.0, 105.0, 50, 4'500'000},
    };
    strat.run(bars, 5);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).is_long == false);
        CHECK(near(strat.get_trade(0).entry_price, 95.0, 1e-9));
        CHECK(near(strat.get_trade(0).exit_price, 105.0, 1e-9));
    }
}

// Open-tie dual-stop: when bar.open equals the stop level for both a long and
// a short flat-armed entry, both legs are "armed" at the open and TV's broker
// emulator picks the long leg as the entry (the short leg becomes the bracket
// exit). Regression for validation probe 83 (76% match -> 100%).
static void test_flat_bracket_dual_stop_open_equals_stop_prefers_long() {
    std::printf("test_flat_bracket_dual_stop_open_equals_stop_prefers_long\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                // Source order intentionally puts the short leg first to prove
                // it does not affect arbitration when both legs tie at open.
                strategy_entry("SE", false, na<double>(), 100.0);
                strategy_entry("LE", true, na<double>(), 100.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        // Bullish bar with open exactly at the stop level. Path uses low first
        // (open closer to low). Without the open-tie fix the engine would pick
        // the short leg via the path; TV picks long.
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    // Expected: long entry at 100 + bracket close by SE on the same bar, also
    // at the stop level (round trip with no remaining position).
    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).is_long == true);
        CHECK(near(strat.get_trade(0).entry_price, 100.0, 1e-9));
        CHECK(near(strat.get_trade(0).exit_price, 100.0, 1e-9));
    }
}

// Flat-armed priced entries firing on the same bar in the same direction must
// both fill — TradingView does not throttle pre-armed bracket legs the way it
// throttles fresh in-position priced entries. Regression for probe 80 where
// an older flat-armed short stop and a newer flat-armed short stop both fire
// as distinct trades despite pyramiding=1.
static void test_flat_armed_priced_entries_pyramid_within_one_bar() {
    std::printf("test_flat_armed_priced_entries_pyramid_within_one_bar\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("S_NEAR", false, na<double>(), 99.0);
                strategy_entry("S_FAR", false, na<double>(), 96.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        {100.0, 100.0, 95.0, 97.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(near(strat.get_signed_position_size(), -2.0, 1e-9));
}

// ---- 9. Magnifier sub-bar processing ---------------------------------------

class MagnifierTestStrategy : public BacktestEngine {
public:
    int on_bar_calls = 0;
    int first_tick_count = 0;
    int last_tick_count = 0;

    MagnifierTestStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar& bar) override {
        on_bar_calls++;
        if (is_first_tick_) first_tick_count++;
        if (is_last_tick_) last_tick_count++;
    }
};

static void test_magnifier_sub_bar_count() {
    std::printf("test_magnifier_sub_bar_count\n");
    MagnifierTestStrategy strat;
    // 6 bars of 1m data, script TF = 3m, magnifier with 4 samples
    Bar bars[6];
    for (int i = 0; i < 6; i++)
        bars[i] = {100.0+i, 105.0+i, 95.0+i, 102.0+i, 50, (int64_t)(i)*60000LL};

    strat.run(bars, 6, "1", "3", true, 4, MagnifierDistribution::ENDPOINTS);

    // 6 input bars / 3 ratio = 2 script bars.
    // run_magnified_bar() calls on_bar() once per script bar (on the last tick),
    // while process_pending_orders() runs on every sub-tick for order fill accuracy.
    // Total: 2 on_bar calls (one per completed script bar)
    CHECK(strat.on_bar_calls == 2);
    // first_tick should fire twice (once per script bar, forced true on last tick)
    CHECK(strat.first_tick_count == 2);
    // last_tick should fire twice
    CHECK(strat.last_tick_count == 2);
}

// ---- 10. NaN propagation through TA chain -----------------------------------

static void test_nan_propagation() {
    std::printf("test_nan_propagation\n");
    ta::SMA sma(3);
    ta::EMA ema(3);

    // First bar: SMA fed NaN, should return NaN; EMA should return NaN
    double s1 = sma.compute(na<double>());
    double e1 = ema.compute(s1);
    CHECK(is_na(e1));

    // Bar 2: SMA still warming up
    double s2 = sma.compute(10);
    double e2 = ema.compute(s2);
    // SMA has only 2 values (one was NaN), still warming up
    // Both should still be NaN
    CHECK(is_na(s2) || is_na(e2) || std::isfinite(e2));
    // The key invariant: if SMA returns NaN, EMA should too
    if (is_na(s2)) CHECK(is_na(e2));

    // Bar 3: SMA may have enough data now
    double s3 = sma.compute(20);
    double e3 = ema.compute(s3);
    // After 3 bars total, SMA should produce something (though first input was NaN)
    // The exact behavior depends on implementation, just verify no crash
    (void)e3;
    CHECK(true); // smoke test - no crash
}

// ---- 11. Per-trade extreme tracking -----------------------------------------

class ExtremeTrackingStrategy : public BacktestEngine {
public:
    ExtremeTrackingStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 5) {
            strategy_close("Long");
        }
    }

    double get_max_runup(int idx) const { return closed_trade_max_runup(idx); }
    double get_max_drawdown(int idx) const { return closed_trade_max_drawdown(idx); }
};

static void test_per_trade_extremes() {
    std::printf("test_per_trade_extremes\n");
    ExtremeTrackingStrategy strat;
    Bar bars[] = {
        {100, 105, 95,  100, 50, 1000},  // entry at open of next bar
        {100, 115, 98,  110, 50, 2000},  // entry fills at open=100, price goes up to 115
        {110, 120, 105, 118, 50, 3000},  // up more to 120
        {118, 119, 90,  95,  50, 4000},  // drops to 90
        {95,  100, 85,  88,  50, 5000},  // drops more to 85
        {88,  95,  86,  92,  50, 6000},  // exit fills next bar
        {92,  95,  90,  93,  50, 7000},  // after exit
    };
    strat.run(bars, 7);

    CHECK(strat.trade_count() >= 1);
    if (strat.trade_count() >= 1) {
        // max_runup should reflect the peak unrealized profit
        double max_runup = strat.get_max_runup(0);
        CHECK(max_runup > 0);
        // max_drawdown should reflect the worst unrealized loss
        double max_dd = strat.get_max_drawdown(0);
        CHECK(max_dd > 0);
    }
}

// ---- 12. MACD as composed indicator -----------------------------------------

static void test_macd_composition() {
    std::printf("test_macd_composition\n");
    ta::MACD macd(12, 26, 9);

    double prices[40];
    for (int i = 0; i < 40; i++) {
        prices[i] = 100 + 5 * std::sin(i * 0.3) + i * 0.1;
    }

    ta::MACDResult last_result;
    for (int i = 0; i < 40; i++) {
        last_result = macd.compute(prices[i]);
    }

    // After 40 bars, MACD should be fully warmed up
    CHECK(!is_na(last_result.macd_line));
    CHECK(!is_na(last_result.signal_line));
    CHECK(std::isfinite(last_result.histogram));
    // histogram = macd_line - signal_line
    CHECK(near(last_result.histogram, last_result.macd_line - last_result.signal_line));
}

// ---- 13. Multiple indicator chain: RSI -> SMA -> BB -------------------------

static void test_rsi_sma_bb_chain() {
    std::printf("test_rsi_sma_bb_chain\n");
    ta::RSI rsi(14);
    ta::SMA sma(5);
    ta::BB bb(10, 2.0);

    for (int i = 0; i < 50; i++) {
        double price = 100 + 10 * std::sin(i * 0.2) + i * 0.05;
        double rsi_val = rsi.compute(price);
        double sma_val = sma.compute(rsi_val);
        auto bb_result = bb.compute(sma_val);

        if (i >= 28) { // all indicators warmed up
            CHECK(!is_na(bb_result.middle));
            CHECK(std::isfinite(bb_result.upper));
            CHECK(std::isfinite(bb_result.lower));
            CHECK(bb_result.upper >= bb_result.middle);
            CHECK(bb_result.lower <= bb_result.middle);
        }
    }
}

// ---- 14. Strategy entry/exit roundtrip PnL ----------------------------------

class PnlTestStrategy : public BacktestEngine {
public:
    PnlTestStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 3) {
            strategy_close("Long");
        }
    }
};

// Trail activation level computation must ceil the trail_points magnitude so
// the activation always sits on a mintick boundary AWAY from entry. When a
// series float (e.g. ta.atr(...)) is passed to trail_points, TradingView's
// broker rounds the tick count up before applying — engine previously kept
// the raw float and let round_to_mintick(round-half-to-nearest) settle the
// fill. That biased the activation 1 tick toward entry on ~40% of
// community/scalping-strategy trades. Regression for the ceil rule.
static void test_trail_points_activation_ceils_to_mintick() {
    std::printf("test_trail_points_activation_ceils_to_mintick\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                // trail_points = 6.2346 (sub-tick precision). With ceil rule
                // the broker uses 7 ticks → activation = entry + $0.07.
                // Without it, the engine produced entry + $0.06 (floor of
                // raw 0.062346).
                strategy_exit("LX", "L",
                              na<double>(), na<double>(),
                              6.2346,
                              na<double>(),
                              na<double>(),
                              100.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {1635.15, 1635.15, 1635.15, 1635.15, 50, 900'000},
        // Bar 1 fills the long market, then the trail activates and fires on
        // the same bar — entry 1635.15, activation 1635.22 (7 ticks above).
        {1635.15, 1635.69, 1630.15, 1635.16, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).is_long == true);
        CHECK(near(strat.get_trade(0).entry_price, 1635.15, 1e-9));
        CHECK(near(strat.get_trade(0).exit_price, 1635.22, 1e-9));
    }
}

static void test_exit_profit_loss_materializes_after_pending_entry_fill() {
    std::printf("test_exit_profit_loss_materializes_after_pending_entry_fill\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
            pyramiding_ = 1;
        }

        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L",
                              na<double>(), na<double>(),
                              na<double>(), na<double>(), na<double>(),
                              100.0, "", na<double>(), "",
                              40.0, 20.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {101.00, 101.00, 101.00, 101.00, 50, 900'000},
        // Entry fills at 100.00. The retained profit/loss exit must price
        // from that actual fill before this bar's path is evaluated.
        {100.00, 100.10, 99.70, 99.90, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).is_long == true);
        CHECK(near(strat.get_trade(0).entry_price, 100.00, 1e-9));
        CHECK(near(strat.get_trade(0).exit_price, 99.80, 1e-9));
        CHECK(strat.get_trade(0).entry_bar_index == 1);
        CHECK(strat.get_trade(0).exit_bar_index == 1);
    }
}

static void test_strategy_pnl_roundtrip() {
    std::printf("test_strategy_pnl_roundtrip\n");
    PnlTestStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},    // bar 0: entry signal
        {100, 110, 98, 108, 50, 120000},    // bar 1: fills at open=100, qty=10
        {108, 115, 105, 112, 50, 180000},   // bar 2
        {112, 118, 110, 115, 50, 240000},   // bar 3: exit signal
        {115, 120, 112, 118, 50, 300000},   // bar 4: exit fills at open=115
    };
    strat.run(bars, 5);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        const Trade& t = strat.get_trade(0);
        CHECK(t.is_long);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 115.0));
        // PnL = (115 - 100) * 10 = 150
        CHECK(near(t.pnl, 150.0));
    }
}

// ---- 15. Aggregator high/low tracking ---------------------------------------

static void test_aggregator_high_low() {
    std::printf("test_aggregator_high_low\n");
    TimeframeAggregator agg(3);
    Bar b1{100, 105, 90,  102, 100, 1000};  // low=90
    Bar b2{102, 120, 100, 106, 200, 2000};  // high=120
    Bar b3{106, 110, 95,  109, 300, 3000};

    agg.feed(b1);
    agg.feed(b2);
    auto r = agg.feed(b3);
    CHECK(r.is_complete);
    CHECK(near(r.bar.open, 100));     // open of first bar
    CHECK(near(r.bar.close, 109));    // close of last bar
    CHECK(near(r.bar.high, 120));     // max high across all 3
    CHECK(near(r.bar.low, 90));       // min low across all 3
}

// ---- 16. Supertrend composed with EMA source --------------------------------

static void test_supertrend_basic() {
    std::printf("test_supertrend_basic\n");
    ta::Supertrend st(3.0, 10);

    for (int i = 0; i < 30; i++) {
        double h = 100 + i + 3;
        double l = 100 + i - 3;
        double c = 100 + i;
        auto result = st.compute(h, l, c);

        if (i >= 10) {
            CHECK(std::isfinite(result.value));
            CHECK(result.direction == 1.0 || result.direction == -1.0);
        }
    }
}

// ---- 17. Strategy with process_orders_on_close ------------------------------

class CloseOrderStrategy : public BacktestEngine {
public:
    CloseOrderStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }

    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 2) {
            strategy_close("Long");
        }
    }
};

static void test_process_orders_on_close() {
    std::printf("test_process_orders_on_close\n");
    CloseOrderStrategy strat;
    Bar bars[] = {
        {100, 105, 95,  102, 50, 60000},
        {102, 110, 100, 108, 50, 120000},
        {108, 115, 105, 112, 50, 180000},
        {112, 118, 110, 115, 50, 240000},
    };
    strat.run(bars, 4);

    // With process_orders_on_close, entry fills at bar.close on bar 0
    // and exit fills at bar.close on bar 2
    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        const Trade& t = strat.get_trade(0);
        CHECK(near(t.entry_price, 102.0)); // bar 0 close
        CHECK(near(t.exit_price, 112.0));  // bar 2 close
    }
}

// ---- 18. Stochastic + SMA smoothing chain -----------------------------------

static void test_stoch_sma_chain() {
    std::printf("test_stoch_sma_chain\n");
    ta::Stoch stoch(14);
    ta::SMA sma_k(3);  // %K smoothing
    ta::SMA sma_d(3);  // %D smoothing

    for (int i = 0; i < 30; i++) {
        double h = 100 + i + (i % 5) * 2;
        double l = 100 + i - (i % 5) * 2;
        double c = (h + l) / 2.0 + (i % 3 - 1);
        double raw_k = stoch.compute(c, h, l);
        double smooth_k = sma_k.compute(raw_k);
        double smooth_d = sma_d.compute(smooth_k);

        if (i >= 18) { // all warmed up
            CHECK(!is_na(smooth_d));
            CHECK(std::isfinite(smooth_d));
        }
    }
}

// ---- 19. EMA recompute through chain consistency ----------------------------

static void test_ema_chain_recompute() {
    std::printf("test_ema_chain_recompute\n");
    // Two independent EMA chains: verify recompute matches fresh compute
    ta::EMA ema1a(5), ema1b(3);
    ta::EMA ema2a(5), ema2b(3);

    double data[] = {10, 12, 11, 14, 13, 15, 16, 12, 18, 17};

    // Feed 9 bars identically
    for (int i = 0; i < 9; i++) {
        double v1 = ema1a.compute(data[i]);
        ema1b.compute(v1);
        double v2 = ema2a.compute(data[i]);
        ema2b.compute(v2);
    }

    // Bar 10: chain 1 computes with 17, then recomputes with 20
    double v1 = ema1a.compute(data[9]);
    ema1b.compute(v1);
    double v1r = ema1a.recompute(20);
    double recomp = ema1b.recompute(v1r);

    // Chain 2 computes directly with 20
    double v2 = ema2a.compute(20);
    double direct = ema2b.compute(v2);

    CHECK(near(recomp, direct));
}

// ---- 20. DMI basic smoke test -----------------------------------------------

static void test_dmi_basic() {
    std::printf("test_dmi_basic\n");
    ta::DMI dmi(14, 14);

    ta::DMIResult last;
    for (int i = 0; i < 30; i++) {
        double h = 100 + i * 0.5 + (i % 3);
        double l = 100 + i * 0.5 - (i % 3);
        double c = (h + l) / 2.0;
        last = dmi.compute(h, l, c);
    }

    // After warmup, DI+ and DI- should be non-negative
    CHECK(!is_na(last.diplus));
    CHECK(!is_na(last.diminus));
    CHECK(last.diplus >= 0.0);
    CHECK(last.diminus >= 0.0);
}

// ---- 21. Multi-indicator confluence — RSI + MACD + BB ----------------------

static void test_multi_indicator_confluence() {
    std::printf("test_multi_indicator_confluence\n");
    ta::RSI rsi(14);
    ta::MACD macd(12, 26, 9);
    ta::BB bb(20, 2.0);

    int entry_signals = 0;
    for (int i = 0; i < 100; i++) {
        double price;
        if (i < 40) price = 100 - i * 0.5;
        else if (i < 60) price = 80 + (i - 40) * 1.0;
        else price = 100 + (i - 60) * 0.3;

        double rsi_val = rsi.compute(price);
        auto macd_r = macd.compute(price);
        auto bb_r = bb.compute(price);

        if (i >= 35) {  // all indicators warmed up (MACD needs ~34 bars)
            CHECK(!is_na(rsi_val));
            CHECK(!is_na(macd_r.histogram));
            CHECK(!is_na(bb_r.lower));

            if (rsi_val < 30 && macd_r.histogram > 0 && price < bb_r.lower)
                entry_signals++;
        }
    }
    // Smoke test — ran without crashing and produced valid values
    CHECK(true);
}

// ---- 22. Position reversal (Long -> Short in one bar) ----------------------

class ReversalStrategy : public BacktestEngine {
public:
    ReversalStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 1) strategy_entry("Long", true);
        if (bar_index_ == 3) strategy_entry("Short", false);
        if (bar_index_ == 5) strategy_close("Short");
    }
};

static void test_position_reversal() {
    std::printf("test_position_reversal\n");
    ReversalStrategy strat;
    Bar bars[7];
    for (int i = 0; i < 7; i++)
        bars[i] = {100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 50, (int64_t)(i + 1) * 60000};
    strat.run(bars, 7);

    // Should have at least 2 trades: the long (closed by reversal) and the short
    CHECK(strat.trade_count() >= 2);
    if (strat.trade_count() >= 2) {
        CHECK(strat.get_trade(0).is_long == true);
        CHECK(strat.get_trade(1).is_long == false);
    }
}

// Reversal should preserve explicit qty on the new opposite position
// instead of falling back to default qty sizing.
static void test_reversal_uses_explicit_qty_for_new_side() {
    std::printf("test_reversal_uses_explicit_qty_for_new_side\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, na<double>(), na<double>(), 2.0);
            }
            if (bar_index_ == 1) {
                strategy_entry("S", false, na<double>(), na<double>(), 5.0);
            }
            if (bar_index_ == 2) {
                strategy_close("S");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},   // long entry at 100, qty=2
        {110.0, 111.0, 109.0, 110.0, 50, 1'800'000}, // reverse to short at 110, qty=5
        {100.0, 101.0, 99.0, 100.0, 50, 2'700'000},  // close short at 100
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 2);
    if (strat.trade_count() == 2) {
        CHECK(strat.get_trade(0).is_long == true);
        CHECK(near(strat.get_trade(0).qty, 2.0, 1e-9));
        CHECK(near(strat.get_trade(0).pnl, 20.0, 1e-9));

        CHECK(strat.get_trade(1).is_long == false);
        CHECK(near(strat.get_trade(1).qty, 5.0, 1e-9));
        CHECK(near(strat.get_trade(1).pnl, 50.0, 1e-9));
    }
}

// ---- 23. Pyramiding + partial exit (qty_percent=50) ------------------------

class PyramidPartialExitStrategy : public BacktestEngine {
public:
    double position_before_close_all = -1.0;
    int partial_trade_rows = -1;

    PyramidPartialExitStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 3;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 1) strategy_entry("E1", true);
        if (bar_index_ == 2) strategy_entry("E2", true);
        if (bar_index_ == 3) strategy_entry("E3", true);
        // Actionable partial exit: bar 6 reaches the 110 limit and closes
        // exactly 50% of the three-lot pyramided position.
        if (bar_index_ == 5) strategy_exit("X1", "",
            110.0,                                     // actionable limit
            std::numeric_limits<double>::quiet_NaN(),  // no stop
            std::numeric_limits<double>::quiet_NaN(),  // no trail_points
            std::numeric_limits<double>::quiet_NaN(),  // no trail_offset
            std::numeric_limits<double>::quiet_NaN(),  // no trail_price
            50.0);  // qty_percent
        if (bar_index_ == 7) {
            position_before_close_all = signed_position_size();
            partial_trade_rows = trade_count();
            strategy_close_all();
        }
    }

    double final_position() const { return signed_position_size(); }
};

static void test_pyramid_partial_exit() {
    std::printf("test_pyramid_partial_exit\n");
    PyramidPartialExitStrategy strat;
    Bar bars[9];
    for (int i = 0; i < 9; i++)
        bars[i] = {100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 50, (int64_t)(i + 1) * 60000};
    strat.run(bars, 9);

    // The partial leg must have executed before close_all: qty 3 -> 1.5.
    CHECK(near(strat.position_before_close_all, 1.5, 1e-9));
    CHECK(strat.partial_trade_rows > 0);
    double partial_qty = 0.0;
    for (int i = 0; i < strat.trade_count(); ++i) {
        if (strat.get_trade(i).exit_id == "X1") {
            partial_qty += strat.get_trade(i).qty;
        }
    }
    CHECK(near(partial_qty, 1.5, 1e-9));
    CHECK(near(strat.final_position(), 0.0, 1e-9));
}

// An actionable strategy.exit(..., qty_percent<100) should reduce, not flatten,
// a position. The limit leg is required: an all-actionable-NaN strategy.exit is
// inert under the TV-pinned high-level command contract.
static void test_exit_qty_percent_reduces_position() {
    std::printf("test_exit_qty_percent_reduces_position\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (bar_index_ == 1) {
                strategy_exit("PX", "L",
                    102.0,                                     // actionable limit, reached on bar 2
                    std::numeric_limits<double>::quiet_NaN(),  // no stop
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_points
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_offset
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_price
                    50.0);                                     // close 50%
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000},
        {102.0, 103.0, 101.0, 102.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.5, 1e-9));
}

// Re-issuing the same partial strategy.exit id each bar should not repeatedly
// re-fill after it has already executed for the current position.
static void test_partial_exit_id_fills_once_per_position() {
    std::printf("test_partial_exit_id_fills_once_per_position\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (bar_index_ >= 1) {
                strategy_exit("P", "L",
                    100.0,                                     // limit always reachable
                    std::numeric_limits<double>::quiet_NaN(),  // no stop
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_points
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_offset
                    std::numeric_limits<double>::quiet_NaN(),  // no trail_price
                    50.0);                                     // partial
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000},
        {102.0, 103.0, 101.0, 102.0, 50, 2'700'000},
        {103.0, 104.0, 102.0, 103.0, 50, 3'600'000},
        {104.0, 105.0, 103.0, 104.0, 50, 4'500'000},
    };
    strat.run(bars, 5);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.5, 1e-9));
}

// ---- 24. close_entries_rule = "ANY" ----------------------------------------

class CloseEntriesAnyStrategy : public BacktestEngine {
public:
    CloseEntriesAnyStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 3;
        close_entries_rule_any_ = true;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 1) strategy_entry("A", true);
        if (bar_index_ == 2) strategy_entry("B", true);
        if (bar_index_ == 3) strategy_entry("C", true);
        if (bar_index_ == 5) strategy_close("B");
        if (bar_index_ == 7) strategy_close_all();
    }
};

static void test_close_entries_any() {
    std::printf("test_close_entries_any\n");
    CloseEntriesAnyStrategy strat;
    Bar bars[9];
    for (int i = 0; i < 9; i++)
        bars[i] = {100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 50, (int64_t)(i + 1) * 60000};
    strat.run(bars, 9);

    // Trade for "B" closed at bar 5, trades for "A" and "C" closed at bar 7
    CHECK(strat.trade_count() >= 2);
}

// ---- 25. Trailing stop mechanics -------------------------------------------

class TrailingStopStrategy : public BacktestEngine {
public:
    TrailingStopStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        syminfo_mintick_ = 1.0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 0) {
            // trail_points=10 (activation), trail_offset=5 (stop distance from peak)
            strategy_exit("TS", "Long",
                std::numeric_limits<double>::quiet_NaN(),  // no limit
                std::numeric_limits<double>::quiet_NaN(),  // no stop
                10.0,   // trail_points
                5.0);   // trail_offset
        }
    }
};

static void test_trailing_stop() {
    std::printf("test_trailing_stop\n");
    TrailingStopStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 108, 98, 105, 50, 120000},
        {105, 115, 103, 112, 50, 180000},
        {112, 114, 109, 111, 50, 240000},
        {111, 113, 108, 109, 50, 300000},
    };
    strat.run(bars, 5);

    // Should have been stopped out by trailing stop
    CHECK(strat.trade_count() >= 1);
}

static void test_limit_exit_beats_trailing_stop_after_activation() {
    std::printf("test_limit_exit_beats_trailing_stop_after_activation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 1.0;
            process_orders_on_close_ = false;
        }

        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("X", "L",
                    113.0,
                    std::numeric_limits<double>::quiet_NaN(),
                    10.0,
                    3.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 115.0, 90.0, 111.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).exit_price, 113.0, 1e-9));
        CHECK(strat.get_trade(0).exit_bar_index == 2);
    }
}

static void test_trailing_stop_fills_at_crossing_level_after_activation() {
    std::printf("test_trailing_stop_fills_at_crossing_level_after_activation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 1.0;
            process_orders_on_close_ = false;
        }

        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("TS", "L",
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    10.0,
                    3.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 115.0, 90.0, 111.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).exit_price, 112.0, 1e-9));
        CHECK(strat.get_trade(0).exit_bar_index == 2);
    }
}

static void test_trailing_stop_does_not_lookahead_bar_high_at_open() {
    std::printf("test_trailing_stop_does_not_lookahead_bar_high_at_open\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 1.0;
            process_orders_on_close_ = false;
        }

        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("TS", "L",
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    10.0,
                    3.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        {100.0, 100.0, 100.0, 100.0, 50, 1'800'000},
        {100.0, 120.0, 95.0, 100.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).exit_price, 117.0, 1e-9));
        CHECK(strat.get_trade(0).exit_bar_index == 2);
    }
}

static void test_trailing_stop_ignores_entry_bar_extreme_before_exit_creation() {
    std::printf("test_trailing_stop_ignores_entry_bar_extreme_before_exit_creation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 1.0;
            process_orders_on_close_ = false;
        }

        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("TS", "L",
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    10.0,
                    3.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        // Entry fills at open 100. The high occurs before the trailing order is
        // created, so it must not activate the trailing stop.
        {100.0, 120.0, 100.0, 100.0, 50, 1'800'000},
        {100.0, 105.0, 95.0, 100.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 0);
}

static void test_trailing_points_without_offset_exits_at_activation() {
    std::printf("test_trailing_points_without_offset_exits_at_activation\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            syminfo_mintick_ = 0.01;
            process_orders_on_close_ = false;
        }

        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("S", false);
                strategy_exit("TS", "S",
                    1791.0,                                     // far take-profit
                    std::numeric_limits<double>::quiet_NaN(),
                    38.0,                                      // activation: entry - 0.38
                    std::numeric_limits<double>::quiet_NaN()); // no trail_offset
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {1898.89, 1903.13, 1868.37, 1880.46, 50, 900'000},
        {1880.45, 1887.29, 1837.70, 1843.53, 50, 1'800'000},
        {1843.53, 1850.63, 1818.05, 1824.95, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).exit_price, 1880.07, 1e-9));
        CHECK(strat.get_trade(0).exit_bar_index == 1);
    }
}

// ---- 26. Magnifier + limit order fill precision ----------------------------

class MagnifierLimitStrategy : public BacktestEngine {
public:
    MagnifierLimitStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true, 97.0);  // limit price
        }
        if (bar_index_ == 1 && position_side_ != PositionSide::FLAT) {
            strategy_close("Long");
        }
    }
};

static void test_magnifier_limit_fill() {
    std::printf("test_magnifier_limit_fill\n");
    MagnifierLimitStrategy strat;
    Bar bars[] = {
        {100, 102, 99, 101, 50, 60000},
        {101, 103, 95, 98, 50, 120000},
        {98, 105, 96, 103, 50, 180000},
        {103, 108, 100, 106, 50, 240000},
        {106, 110, 104, 108, 50, 300000},  // bar for close order to fill
        {108, 112, 106, 110, 50, 360000},
    };
    strat.run(bars, 6, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);

    // Limit entry at 97 should fill on script bar 1 (input bars 2-3, low=96).
    // Close order placed on bar 1 fills on bar 2 (input bars 4-5).
    CHECK(strat.trade_count() >= 1);
}

// ---- 26b. Magnifier + volume_weighted sampling ------------------------------

static void test_magnifier_volume_weighted_toggle() {
    std::printf("test_magnifier_volume_weighted_toggle\n");
    // Real-bar magnifier mode (multiple sub-bars per script bar, e.g.
    // input_tf=1m, script_tf=2m) overrides the volume-weighted toggle: each
    // sub-bar collapses to its four real OHLC corners regardless of vw,
    // because synthesizing extra ticks inside a real lower-TF bar cannot
    // recover information not in the input feed. The toggle still flows
    // through the engine's state, but the per-sub-bar tick count stays
    // pinned at 4 — so flat and vw report the same total tick count.
    Bar bars[] = {
        {100, 105, 99, 103, 5000, 60000},
        {103, 108, 102, 106,  500, 120000},
        {106, 112, 104, 110, 2000, 180000},
        {110, 115, 108, 113, 2000, 240000},
    };

    MagnifierLimitStrategy strat_flat;
    strat_flat.run(bars, 4, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);
    ReportC flat_report{};
    strat_flat.fill_report(&flat_report);
    int64_t flat_ticks = flat_report.magnifier_sample_ticks_total;
    int64_t flat_subs  = flat_report.magnifier_sub_bars_total;
    BacktestEngine::free_report(&flat_report);

    MagnifierLimitStrategy strat_vw;
    strat_vw.set_magnifier_volume_weighted(true);
    strat_vw.run(bars, 4, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);
    ReportC vw_report{};
    strat_vw.fill_report(&vw_report);
    int64_t vw_ticks = vw_report.magnifier_sample_ticks_total;
    int64_t vw_subs  = vw_report.magnifier_sub_bars_total;
    BacktestEngine::free_report(&vw_report);

    // Real-bar mode pins ticks at 4 per sub-bar — vw and flat must match.
    CHECK(flat_ticks == vw_ticks);
    CHECK(flat_subs == vw_subs);
    CHECK(flat_ticks == flat_subs * 4);
}

// ---- 27. Magnifier + TA consistency ----------------------------------------

static void test_magnifier_ta_consistency() {
    std::printf("test_magnifier_ta_consistency\n");
    ta::SMA sma_normal(3);
    ta::SMA sma_magnified(3);

    // Normal: SMA on aggregated closes
    double normal_val = 0;
    normal_val = sma_normal.compute(102);
    normal_val = sma_normal.compute(103);
    normal_val = sma_normal.compute(110);
    // SMA(3) of [102, 103, 110] = 105
    CHECK(near(normal_val, 105.0));

    // Magnified: compute + recompute through sub-bars
    sma_magnified.compute(100);
    sma_magnified.recompute(102);
    sma_magnified.compute(105);
    sma_magnified.recompute(103);
    sma_magnified.compute(108);
    double mag_val = sma_magnified.recompute(110);

    CHECK(near(mag_val, normal_val));
}

// ---- 28. Risk halt — max_drawdown stops trading ----------------------------

class RiskHaltStrategy : public BacktestEngine {
public:
    int entries_attempted = 0;
    RiskHaltStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        risk_max_drawdown_ = 500;
    }
    void on_bar(const Bar& bar) override {
        entries_attempted++;
        if (position_side_ == PositionSide::FLAT) {
            strategy_entry("Long", true);
        }
        if (bar_index_ % 3 == 2) {
            strategy_close("Long");
        }
    }
};

static void test_risk_halt_max_drawdown() {
    std::printf("test_risk_halt_max_drawdown\n");
    RiskHaltStrategy strat;
    Bar bars[20];
    for (int i = 0; i < 20; i++) {
        double base = 1000 - i * 30;
        bars[i] = {base, base + 5, base - 5, base - 3, 50, (int64_t)(i + 1) * 60000};
    }
    strat.run(bars, 20);

    // With declining prices and 500 drawdown limit, trades should be limited
    int trades_with_risk = strat.trade_count();
    // Verify it didn't crash and produced some trades
    CHECK(trades_with_risk >= 1);
}

// ---- 29. Equity extremes accuracy ------------------------------------------

class EquityTrackStrategy : public BacktestEngine {
public:
    EquityTrackStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 4) strategy_close("L");
    }

    double get_max_drawdown() const { return max_drawdown_; }
    double get_max_runup() const { return max_runup_; }
    double get_net_profit() const { return net_profit_sum_; }
};

// ---- 28b. Per-trade MAE/MFE propagated to ReportC --------------------------

class MaeMfeStrategy : public BacktestEngine {
public:
    MaeMfeStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 3) strategy_close("L");
    }
};

static void test_mae_mfe_exposed_in_report() {
    std::printf("test_mae_mfe_exposed_in_report\n");
    // Long fills on bar 1 open at 100 (market order placed bar 0 close).
    // Subsequent path before exit:
    //   bar 1 high=120 (favorable +20), low=98  (adverse  -2)
    //   bar 2 high=118, low=80   (adverse dips to  -20)
    //   bar 3 exit placed; fills on bar 4 open at 95.
    // Expected max_runup    = 120 - 100 = 20
    // Expected max_drawdown = 100 - 80  = 20
    MaeMfeStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 120, 98, 115, 50, 120000},
        {115, 118, 80, 95, 50, 180000},
        {95, 96, 80, 85, 50, 240000},
        {95, 96, 93, 95, 50, 300000},
    };
    strat.run(bars, 5);

    ReportC rep{};
    strat.fill_report(&rep);
    CHECK(rep.trades_len >= 1);
    if (rep.trades_len >= 1) {
        const TradeC& tr = rep.trades[0];
        CHECK(near(tr.max_runup, 20.0, 1e-6));
        CHECK(near(tr.max_drawdown, 20.0, 1e-6));
    }
    BacktestEngine::free_report(&rep);
}

static void test_equity_extremes_accuracy() {
    std::printf("test_equity_extremes_accuracy\n");
    EquityTrackStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 120, 98, 115, 50, 120000},
        {115, 118, 80, 85, 50, 180000},
        {85, 90, 75, 78, 50, 240000},
        {78, 82, 76, 80, 50, 300000},
        {80, 85, 78, 83, 50, 360000},
    };
    strat.run(bars, 6);

    CHECK(strat.trade_count() == 1);
    double net = strat.get_net_profit();
    CHECK(near(net, -20.0, 0.01));

    double dd = strat.get_max_drawdown();
    CHECK(dd > 20.0);
}

// ---- 30. Series history [n] correctness ------------------------------------

static void test_series_history() {
    std::printf("test_series_history\n");
    // Mirrors Pine series[k]: [0] current bar, [k] k bars ago; na if no data.
    Series<double> s(10);

    CHECK(is_na(s[-1]));

    s.push(10);
    CHECK(s[0] == 10);
    CHECK(is_na(s[1]));
    CHECK(is_na(s[3]));

    s.push(20);
    CHECK(s[0] == 20);
    CHECK(s[1] == 10);

    s.push(30);
    CHECK(s[0] == 30);
    CHECK(s[1] == 20);
    CHECK(s[2] == 10);
    CHECK(is_na(s[3]));

    // Update (magnifier) — replaces current without advancing
    s.update(35);
    CHECK(s[0] == 35);
    CHECK(s[1] == 20);
    CHECK(s[2] == 10);

    // Push after update — advances
    s.push(40);
    CHECK(s[0] == 40);
    CHECK(s[1] == 35);
    CHECK(s[2] == 20);
}

// ---- 31. process_orders_on_close with stop/limit ---------------------------

class POOCStopLimitStrategy : public BacktestEngine {
public:
    POOCStopLimitStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 0) {
            strategy_exit("SL", "Long",
                std::numeric_limits<double>::quiet_NaN(),
                95.0);
        }
    }
};

static void test_pooc_stop_deferred() {
    std::printf("test_pooc_stop_deferred\n");
    POOCStopLimitStrategy strat;
    Bar bars[] = {
        {100, 105, 90, 100, 50, 60000},
        {100, 103, 96, 101, 50, 120000},
        {101, 104, 93, 97, 50, 180000},
        {97, 100, 95, 99, 50, 240000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        // Exit should NOT be on bar 0 (same bar as entry with POOC)
        CHECK(strat.get_trade(0).exit_bar_index > 0);
    }
}

// ---- 32. OCA order groups --------------------------------------------------

class OCAStrategy : public BacktestEngine {
public:
    OCAStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        }
        if (bar_index_ == 0) {
            strategy_exit("TP", "Long", 120.0,
                std::numeric_limits<double>::quiet_NaN());
            strategy_exit("SL", "Long",
                std::numeric_limits<double>::quiet_NaN(),
                90.0);
        }
    }
};

static void test_oca_one_cancels_other() {
    std::printf("test_oca_one_cancels_other\n");
    OCAStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 110, 98, 108, 50, 120000},
        {108, 125, 105, 122, 50, 180000},
        {122, 128, 120, 125, 50, 240000},
    };
    strat.run(bars, 4);

    // Only one exit should have occurred (TP hit)
    CHECK(strat.trade_count() == 1);
}

// ---- 33. Multi-TF aggregation (disabled: register_request_tf / request_security_field not in runtime) ----

#if 0
class MultiTFStrategy : public BacktestEngine {
public:
    double hourly_close = 0;
    int hourly_updates = 0;

    MultiTFStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
    }

    void setup_tfs() {
        register_request_tf("60");
    }

    void on_bar(const Bar& bar) override {
        double hc = request_security_field("60", "close");
        if (!is_na(hc) && hc != hourly_close) {
            hourly_close = hc;
            hourly_updates++;
        }
    }
};

static void test_multi_tf_aggregation() {
    std::printf("test_multi_tf_aggregation\n");
    MultiTFStrategy strat;
    strat.setup_tfs();

    // 16 bars of 15m data = 4 hours
    Bar bars[16];
    for (int i = 0; i < 16; i++)
        bars[i] = {100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 50,
                   (int64_t)(1705363200000LL + i * 900000LL)};

    strat.run(bars, 16, "15", "15");

    // 16 bars of 15m = 4 completed hourly bars, should get several updates
    CHECK(strat.hourly_updates >= 3);
}
#endif

// ---- 34. Simple long — position_size, avg_price, equity, openprofit, netprofit

class PositionLongStrategy : public BacktestEngine {
public:
    // Snapshots at each bar
    double pos_size[5] = {};
    double pos_avg_price[5] = {};
    double equity[5] = {};
    double open_pnl[5] = {};
    double net_pnl[5] = {};
    int open_trades[5] = {};
    int closed_trades_count[5] = {};

    PositionLongStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_value_ = 0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("Long", true);
        if (bar_index_ == 3) strategy_close("Long");

        // Snapshot state AFTER strategy logic
        int i = bar_index_;
        if (i < 5) {
            pos_size[i] = signed_position_size();
            pos_avg_price[i] = position_entry_price_;
            equity[i] = current_equity() + open_profit(bar.close);
            open_pnl[i] = open_profit(bar.close);
            net_pnl[i] = net_profit();
            open_trades[i] = (int)pyramid_entries_.size();
            closed_trades_count[i] = (int)trades_.size();
        }
    }
};

static void test_position_long_lifecycle() {
    std::printf("test_position_long_lifecycle\n");
    PositionLongStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {102, 108, 100, 105, 50, 120000},  // entry fills here at open=102
        {105, 112, 103, 110, 50, 180000},
        {110, 115, 108, 112, 50, 240000},
        {112, 118, 110, 115, 50, 300000},  // exit fills here at open=112
    };
    strat.run(bars, 5);

    // Bar 0: order placed, not yet filled
    CHECK(near(strat.pos_size[0], 0.0));
    CHECK(strat.open_trades[0] == 0);

    // Bar 1: entry filled at open=102, qty=2
    CHECK(near(strat.pos_size[1], 2.0));
    CHECK(near(strat.pos_avg_price[1], 102.0));
    CHECK(near(strat.open_pnl[1], (105.0 - 102.0) * 2.0));  // open_profit at close=105
    CHECK(strat.open_trades[1] == 1);

    // Bar 2: still in position
    CHECK(near(strat.pos_size[2], 2.0));
    CHECK(near(strat.open_pnl[2], (110.0 - 102.0) * 2.0));  // at close=110

    // Bar 3: close order placed, not yet filled
    CHECK(near(strat.pos_size[3], 2.0));  // still in position

    // Bar 4: exit filled at open=112
    CHECK(near(strat.pos_size[4], 0.0));
    CHECK(near(strat.net_pnl[4], (112.0 - 102.0) * 2.0));  // = 20
    CHECK(near(strat.equity[4], 10020.0));
    CHECK(strat.closed_trades_count[4] == 1);
    CHECK(strat.open_trades[4] == 0);
}

// ---- 35. Simple short — mirror of long test

class PositionShortStrategy : public BacktestEngine {
public:
    double pos_size[5] = {};
    double pos_avg_price[5] = {};
    double net_pnl[5] = {};

    PositionShortStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_value_ = 0;
        slippage_ = 0;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("Short", false);
        if (bar_index_ == 3) strategy_close("Short");
        int i = bar_index_;
        if (i < 5) {
            pos_size[i] = signed_position_size();
            pos_avg_price[i] = position_entry_price_;
            net_pnl[i] = net_profit();
        }
    }
};

static void test_position_short_lifecycle() {
    std::printf("test_position_short_lifecycle\n");
    PositionShortStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {102, 108, 100, 105, 50, 120000},
        {105, 112, 103, 110, 50, 180000},
        {110, 115, 108, 112, 50, 240000},
        {112, 118, 110, 115, 50, 300000},
    };
    strat.run(bars, 5);

    CHECK(near(strat.pos_size[0], 0.0));
    CHECK(near(strat.pos_size[1], -2.0));
    CHECK(near(strat.pos_avg_price[1], 102.0));
    CHECK(near(strat.pos_size[4], 0.0));
    CHECK(near(strat.net_pnl[4], -20.0));  // (102-112)*2 = -20
}

// ---- 36. Pyramiding — 3 entries at different prices, verify avg_price

class PyramidStrategy : public BacktestEngine {
public:
    double pos_size[7] = {};
    double pos_avg[7] = {};
    int open_trade_count[7] = {};
    double net_pnl[7] = {};

    PyramidStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 3;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("E1", true);
        if (bar_index_ == 1) strategy_entry("E2", true);
        if (bar_index_ == 2) strategy_entry("E3", true);
        if (bar_index_ == 5) strategy_close_all();

        int i = bar_index_;
        if (i < 7) {
            pos_size[i] = signed_position_size();
            pos_avg[i] = position_entry_price_;
            open_trade_count[i] = (int)pyramid_entries_.size();
            net_pnl[i] = net_profit();
        }
    }
};

static void test_pyramid_avg_price() {
    std::printf("test_pyramid_avg_price\n");
    PyramidStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 108, 98, 105, 50, 120000},   // E1 fills at 100
        {105, 112, 103, 110, 50, 180000},  // E2 fills at 105
        {110, 115, 108, 115, 50, 240000},  // E3 fills at 110
        {115, 120, 113, 120, 50, 300000},
        {120, 125, 118, 125, 50, 360000},  // close_all placed
        {125, 130, 123, 130, 50, 420000},  // exit fills at 125
    };
    strat.run(bars, 7);

    // After all 3 entries filled (bar 3)
    CHECK(near(strat.pos_size[3], 3.0));
    CHECK(near(strat.pos_avg[3], 105.0));  // (100+105+110)/3
    CHECK(strat.open_trade_count[3] == 3);

    // After exit (bar 6)
    CHECK(near(strat.pos_size[6], 0.0));
    CHECK(near(strat.net_pnl[6], 60.0));  // 25+20+15
    CHECK(strat.trade_count() == 3);

    // Verify each trade individually
    CHECK(near(strat.get_trade(0).pnl, 25.0));  // E1: 125-100
    CHECK(near(strat.get_trade(1).pnl, 20.0));  // E2: 125-105
    CHECK(near(strat.get_trade(2).pnl, 15.0));  // E3: 125-110
}

// ---- 37. Win/loss sequence — verify wintrades, losstrades, grossprofit, grossloss

class WinLossStrategy : public BacktestEngine {
public:
    WinLossStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;  // fills at close for exact prices
    }
    void on_bar(const Bar& bar) override {
        switch(bar_index_) {
            case 0: strategy_entry("T1", true); break;   // buy at 100
            case 1: strategy_close("T1"); break;                     // sell at 120
            case 2: strategy_entry("T2", true); break;   // buy at 120
            case 3: strategy_close("T2"); break;                     // sell at 110
            case 4: strategy_entry("T3", false); break;  // short at 110
            case 5: strategy_close("T3"); break;                     // cover at 100
            case 6: strategy_entry("T4", false); break;  // short at 100
            case 7: strategy_close("T4"); break;                     // cover at 115
        }
    }
    // Public accessors for protected methods
    int get_wintrades() const { return count_wintrades(); }
    int get_losstrades() const { return count_losstrades(); }
    double get_gross_profit() const { return gross_profit(); }
    double get_gross_loss() const { return gross_loss(); }
    double get_net_profit() const { return net_profit(); }
};

static void test_win_loss_tracking() {
    std::printf("test_win_loss_tracking\n");
    WinLossStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},    // T1 entry at 100
        {100, 125, 98, 120, 50, 120000},    // T1 exit at 120 -> +20
        {120, 125, 118, 120, 50, 180000},   // T2 entry at 120
        {120, 122, 108, 110, 50, 240000},   // T2 exit at 110 -> -10
        {110, 115, 108, 110, 50, 300000},   // T3 short at 110
        {110, 112, 98, 100, 50, 360000},    // T3 cover at 100 -> +10
        {100, 105, 98, 100, 50, 420000},    // T4 short at 100
        {100, 118, 98, 115, 50, 480000},    // T4 cover at 115 -> -15
    };
    strat.run(bars, 8);

    CHECK(strat.trade_count() == 4);
    CHECK(strat.get_wintrades() == 2);
    CHECK(strat.get_losstrades() == 2);
    CHECK(near(strat.get_gross_profit(), 30.0));   // 20 + 10
    CHECK(near(strat.get_gross_loss(), -25.0));     // -10 + -15
    CHECK(near(strat.get_net_profit(), 5.0));       // 30 - 25

    // Verify individual trades
    CHECK(strat.get_trade(0).is_long == true);
    CHECK(near(strat.get_trade(0).pnl, 20.0));
    CHECK(strat.get_trade(1).is_long == true);
    CHECK(near(strat.get_trade(1).pnl, -10.0));
    CHECK(strat.get_trade(2).is_long == false);
    CHECK(near(strat.get_trade(2).pnl, 10.0));
    CHECK(strat.get_trade(3).is_long == false);
    CHECK(near(strat.get_trade(3).pnl, -15.0));
}

// ---- 38. Position reversal — long to short, verify intermediate state

class ReversalPositionStrategy : public BacktestEngine {
public:
    double pos_size[6] = {};
    double pos_avg[6] = {};
    double net_pnl[6] = {};

    ReversalPositionStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("Long", true);
        if (bar_index_ == 2) strategy_entry("Short", false); // reversal
        if (bar_index_ == 4) strategy_close("Short");

        int i = bar_index_;
        if (i < 6) {
            pos_size[i] = signed_position_size();
            pos_avg[i] = position_entry_price_;
            net_pnl[i] = net_profit();
        }
    }
};

static void test_position_reversal_state() {
    std::printf("test_position_reversal_state\n");
    ReversalPositionStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},     // long entry at 100
        {100, 110, 98, 108, 50, 120000},
        {108, 112, 105, 110, 50, 180000},    // reversal: close long at 110, open short at 110
        {110, 115, 105, 107, 50, 240000},
        {107, 110, 100, 105, 50, 300000},    // close short at 105
        {105, 108, 103, 106, 50, 360000},
    };
    strat.run(bars, 6);

    // TradingView semantics: with process_orders_on_close=true a market
    // strategy.entry fills at THIS bar's close, but the resulting position is
    // NOT visible to strategy.position_size / strategy.position_avg_price until
    // the NEXT bar's evaluation (the broker state updates between bars). So on
    // the bar that places the entry the script still sees the pre-entry state.
    //
    // Bar 0: long entry placed; position not yet visible this bar (still flat).
    CHECK(near(strat.pos_size[0], 0.0));
    CHECK(near(strat.pos_avg[0], 0.0));

    // Bar 2: reversal entry placed; the long opened on bar 0 (visible since
    // bar 1) is still the live position during this bar's script — the flip to
    // short fills at bar-2 close and only becomes visible on bar 3.
    CHECK(near(strat.pos_size[2], 1.0));
    CHECK(near(strat.pos_avg[2], 100.0));
    CHECK(near(strat.net_pnl[2], 0.0));    // long not closed yet from script POV

    // Bar 3: short now visible at avg 110, closed long realized +10.
    CHECK(near(strat.pos_size[3], -1.0));
    CHECK(near(strat.pos_avg[3], 110.0));
    CHECK(near(strat.net_pnl[3], 10.0));

    // Bar 4: short closed at 105 (pnl=+5). Like the market ENTRY on bar 0
    // (comment above), a process_orders_on_close strategy.close fills at
    // THIS bar's close but only AFTER the script's calc — TV's broker
    // state updates between bars, so the script still sees the live short
    // during bar 4; the flat state and realized +15 become visible on
    // bar 5. (Close fills moved to the end-of-bar order-processing point
    // by the same-bar multi-close single-fill batch — validated against
    // the 3commas grid-bot TV exports, xau 10.8%->81.2% / xlm
    // 22.4%->99.0% matched with pol/xrp held at 100%.)
    CHECK(near(strat.pos_size[4], -1.0));
    CHECK(near(strat.pos_avg[4], 110.0));
    CHECK(near(strat.net_pnl[4], 10.0));

    // Bar 5: flat + realized 15 now visible.
    CHECK(near(strat.pos_size[5], 0.0));
    CHECK(near(strat.net_pnl[5], 15.0));  // 10 + 5

    CHECK(strat.trade_count() == 2);
}

// ---- 38a2. TV one-close-fill-per-bar: multiple default-FIFO strategy.close
// calls on the SAME bar (the grid-bot pattern) collapse into a single
// surviving fill — the LAST nonzero-target call wins, its id-ledger is NOT
// consumed by the fill (the remainder stays closable by a later close of the
// same id). The first replaced call's unreserved logical slot is consumed;
// intermediate replaced calls keep theirs. Empirically derived from 3commas
// (xau/xlm/pol/xrp) — see fix/same-bar-multi-close-single-fill.

class SameBarMultiCloseStrategy : public BacktestEngine {
public:
    double final_pos = -1.0;  // signed position size seen on the last bar
    SameBarMultiCloseStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 10;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("A", true, na, na, 1.0);
        if (bar_index_ == 1) strategy_entry("B", true, na, na, 2.0);
        if (bar_index_ == 2) {
            strategy_close("A", "tpA");   // first replacement: ledger consumed
            strategy_close("B", "tpB");   // survivor: fills qty 2 (FIFO drain)
        }
        if (bar_index_ == 3) strategy_entry("B", true, na, na, 2.0);
        if (bar_index_ == 4) strategy_close("B", "tpB2");  // old 2 + new 2, pos-clamped
        if (bar_index_ == 5) final_pos = signed_position_size();
    }
};

static void test_same_bar_multi_close_single_fill() {
    std::printf("test_same_bar_multi_close_single_fill\n");
    SameBarMultiCloseStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50, 60000},   // bar 0: A qty1 fills @100
        {100, 101, 99, 100, 50, 120000},  // bar 1: B qty2 fills @100 (pos 3)
        {100, 106, 99, 105, 50, 180000},  // bar 2: both closes -> ONE fill qty2 @105
        {105, 106, 99, 100, 50, 240000},  // bar 3: B qty2 refills @100 (pos 3)
        {100, 111, 99, 110, 50, 300000},  // bar 4: close(B) fills min(ledger 4, pos 3) @110
        {110, 111, 99, 110, 50, 360000},
    };
    strat.run(bars, 6);

    // Bar 2 fills only the surviving close's qty (2): FIFO drains lot A (1)
    // fully + half of lot B -> two trade rows @105. The old immediate path
    // filled BOTH closes (qty 3, flat) — the one-fill rule leaves 1 open.
    // Bar 4's close(B) then targets B's UNCONSUMED ledger (2 stale + 2 new),
    // clamped to the live position (3): two rows @110, flat afterwards.
    CHECK(strat.trade_count() == 4);
    if (strat.trade_count() == 4) {
        CHECK(near(strat.get_trade(0).qty, 1.0));       // lot A drained @105
        CHECK(near(strat.get_trade(0).exit_price, 105.0));
        CHECK(strat.get_trade(0).exit_comment == "tpB");  // survivor's comment
        CHECK(near(strat.get_trade(1).qty, 1.0));       // half of lot B @105
        CHECK(near(strat.get_trade(1).exit_price, 105.0));
        CHECK(near(strat.get_trade(2).qty, 1.0));       // lot B remainder @110
        CHECK(near(strat.get_trade(2).exit_price, 110.0));
        CHECK(strat.get_trade(2).exit_comment == "tpB2");
        CHECK(near(strat.get_trade(3).qty, 2.0));       // bar-3 refill @110
        CHECK(near(strat.get_trade(3).exit_price, 110.0));
    }
    CHECK(near(strat.final_pos, 0.0));
}

class CloseReplacementProbeBase : public BacktestEngine {
public:
    CloseReplacementProbeBase() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 10;
        process_orders_on_close_ = true;
    }
    double ledger(const std::string& id) const {
        auto it = id_unclosed_qty_.find(id);
        return it == id_unclosed_qty_.end() ? 0.0 : it->second;
    }
    double reservation(const std::string& id) const {
        auto it = close_reserved_qty_.find(id);
        return it == close_reserved_qty_.end() ? 0.0 : it->second;
    }
    double two_call_first_qty(const std::string& id) const {
        auto it = close_two_call_first_qty_.find(id);
        return it == close_two_call_first_qty_.end() ? 0.0 : it->second;
    }
};

// ENA discriminator: only an exact-two-call batch may create provenance, and
// only a later exact-two-call batch may consume it. The carried quantity is
// the PRIOR batch's FIRST target (2), not B's ledger (6), reservation (3), or
// ledger-minus-reservation (3).
class TwoCallReplacementChainStrategy : public CloseReplacementProbeBase {
public:
    double prior_res_b = -1.0;
    double prior_first_b = -1.0;
    double carried_ledger_b = -1.0;
    double released_res_b = -1.0;
    double released_first_b = -1.0;
    double final_pos = -1.0;
    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("A", true, na, na, 2.0);
        if (bar_index_ == 1) strategy_entry("B", true, na, na, 3.0);
        if (bar_index_ == 2) strategy_entry("C", true, na, na, 3.0);
        if (bar_index_ == 3) {
            strategy_close("A", "priorA");
            strategy_close("B", "priorB");
        }
        if (bar_index_ == 4) {
            prior_res_b = reservation("B");
            prior_first_b = two_call_first_qty("B");
            strategy_entry("B", true, na, na, 3.0);  // ledger B: 3 -> 6
        }
        if (bar_index_ == 5) {
            strategy_close("B", "currentB");
            strategy_close("C", "currentC");
        }
        if (bar_index_ == 6) {
            carried_ledger_b = ledger("B");
            released_res_b = reservation("B");
            released_first_b = two_call_first_qty("B");
            strategy_entry("B", true, na, na, 3.0);  // ledger B: 2 -> 5
        }
        if (bar_index_ == 7) strategy_close("B", "finalB");
        if (bar_index_ == 8) final_pos = signed_position_size();
    }
};

static void test_exact_two_call_replacement_carries_prior_first_target() {
    std::printf("test_exact_two_call_replacement_carries_prior_first_target\n");
    TwoCallReplacementChainStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50,  60000},
        {100, 101, 99, 100, 50, 120000},
        {100, 101, 99, 100, 50, 180000},
        {100, 101, 99, 100, 50, 240000},
        {100, 101, 99, 100, 50, 300000},
        {100, 101, 99, 100, 50, 360000},
        {100, 101, 99, 100, 50, 420000},
        {100, 101, 99, 100, 50, 480000},
        {100, 101, 99, 100, 50, 540000},
    };
    strat.run(bars, 9);

    CHECK(near(strat.prior_res_b, 3.0));
    CHECK(near(strat.prior_first_b, 2.0));
    CHECK(near(strat.carried_ledger_b, 2.0));
    CHECK(near(strat.released_res_b, 0.0));
    CHECK(near(strat.released_first_b, 0.0));
    CHECK(near(strat.final_pos, 3.0));
    CHECK(strat.trade_count() == 6);
    if (strat.trade_count() == 6) {
        const double expected_qty[] = {2.0, 1.0, 2.0, 1.0, 2.0, 3.0};
        for (int i = 0; i < 6; ++i) {
            CHECK(near(strat.get_trade(i).qty, expected_qty[i]));
        }
    }
}

// XAU L37 control: a 3+ call batch creates a reservation but no two-call
// provenance, so a later two-call replacement keeps the legacy full erase.
// The subsequent sole close also clears the survivor's reservation/provenance,
// and close_all exercises the flat-position reset.
class ThreeToTwoReplacementStrategy : public CloseReplacementProbeBase {
public:
    double prior_res_b = -1.0;
    double prior_first_b = -1.0;
    double ledger_b = -1.0;
    double res_b = -1.0;
    double first_b = -1.0;
    double res_c_after_sole = -1.0;
    double first_c_after_sole = -1.0;
    double final_pos = -1.0;
    size_t final_reservations = 99;
    size_t final_provenance = 99;
    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("A", true, na, na, 1.0);
        if (bar_index_ == 1) strategy_entry("X", true, na, na, 1.0);
        if (bar_index_ == 2) strategy_entry("B", true, na, na, 1.0);
        if (bar_index_ == 3) strategy_entry("C", true, na, na, 1.0);
        if (bar_index_ == 4) {
            strategy_close("A", "priorA");
            strategy_close("X", "priorX");
            strategy_close("B", "priorB");
        }
        if (bar_index_ == 5) {
            prior_res_b = reservation("B");
            prior_first_b = two_call_first_qty("B");
            strategy_close("B", "currentB");
            strategy_close("C", "currentC");
        }
        if (bar_index_ == 6) {
            ledger_b = ledger("B");
            res_b = reservation("B");
            first_b = two_call_first_qty("B");
            strategy_close("C", "soleC");
        }
        if (bar_index_ == 7) {
            res_c_after_sole = reservation("C");
            first_c_after_sole = two_call_first_qty("C");
            strategy_close_all();
        }
        if (bar_index_ == 8) {
            final_pos = signed_position_size();
            final_reservations = close_reserved_qty_.size();
            final_provenance = close_two_call_first_qty_.size();
        }
    }
};

static void test_three_call_batch_does_not_create_two_call_provenance() {
    std::printf("test_three_call_batch_does_not_create_two_call_provenance\n");
    ThreeToTwoReplacementStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50,  60000},
        {100, 101, 99, 100, 50, 120000},
        {100, 101, 99, 100, 50, 180000},
        {100, 101, 99, 100, 50, 240000},
        {100, 101, 99, 100, 50, 300000},
        {100, 101, 99, 100, 50, 360000},
        {100, 101, 99, 100, 50, 420000},
        {100, 101, 99, 100, 50, 480000},
        {100, 101, 99, 100, 50, 540000},
    };
    strat.run(bars, 9);

    CHECK(near(strat.prior_res_b, 1.0));
    CHECK(near(strat.prior_first_b, 0.0));
    CHECK(near(strat.ledger_b, 0.0));
    CHECK(near(strat.res_b, 0.0));
    CHECK(near(strat.first_b, 0.0));
    CHECK(near(strat.res_c_after_sole, 0.0));
    CHECK(near(strat.first_c_after_sole, 0.0));
    CHECK(near(strat.final_pos, 0.0));
    CHECK(strat.final_reservations == 0);
    CHECK(strat.final_provenance == 0);
}

// A prior exact-two batch does create provenance, but a third call in the
// current batch invalidates its provisional carry. Intermediate ledgers remain
// intact and a 3+ survivor must not create new two-call provenance.
class TwoToThreeReplacementStrategy : public CloseReplacementProbeBase {
public:
    double prior_res_b = -1.0;
    double prior_first_b = -1.0;
    double ledger_b = -1.0;
    double ledger_c = -1.0;
    double ledger_d = -1.0;
    double res_b = -1.0;
    double res_d = -1.0;
    double first_d = -1.0;
    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("A", true, na, na, 1.0);
        if (bar_index_ == 1) strategy_entry("B", true, na, na, 1.0);
        if (bar_index_ == 2) strategy_entry("C", true, na, na, 1.0);
        if (bar_index_ == 3) strategy_entry("D", true, na, na, 1.0);
        if (bar_index_ == 4) {
            strategy_close("A", "priorA");
            strategy_close("B", "priorB");
        }
        if (bar_index_ == 5) {
            prior_res_b = reservation("B");
            prior_first_b = two_call_first_qty("B");
            strategy_close("B", "currentB");
            strategy_close("C", "currentC");
            strategy_close("D", "currentD");
        }
        if (bar_index_ == 6) {
            ledger_b = ledger("B");
            ledger_c = ledger("C");
            ledger_d = ledger("D");
            res_b = reservation("B");
            res_d = reservation("D");
            first_d = two_call_first_qty("D");
        }
    }
};

static void test_three_call_current_batch_invalidates_two_call_carry() {
    std::printf("test_three_call_current_batch_invalidates_two_call_carry\n");
    TwoToThreeReplacementStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50,  60000},
        {100, 101, 99, 100, 50, 120000},
        {100, 101, 99, 100, 50, 180000},
        {100, 101, 99, 100, 50, 240000},
        {100, 101, 99, 100, 50, 300000},
        {100, 101, 99, 100, 50, 360000},
        {100, 101, 99, 100, 50, 420000},
    };
    strat.run(bars, 7);

    CHECK(near(strat.prior_res_b, 1.0));
    CHECK(near(strat.prior_first_b, 1.0));
    CHECK(near(strat.ledger_b, 0.0));
    CHECK(near(strat.ledger_c, 1.0));
    CHECK(near(strat.ledger_d, 1.0));
    CHECK(near(strat.res_b, 0.0));
    CHECK(near(strat.res_d, 1.0));
    CHECK(near(strat.first_d, 0.0));
}

// A surviving multi-close may fill the entire position slice not already
// claimed by older reservations. In that case no physical backing remains for
// the survivor: its ledger/reservation/provenance must all clear. A later
// zero-available close also consumes its stale logical cycle, so a same-id
// re-entry starts fresh instead of accumulating an unfillable prior slice.
class ZeroBackedCloseReservationStrategy : public CloseReplacementProbeBase {
public:
    double post_pos = -1.0;
    double post_ledger_b = -1.0;
    double post_res_b = -1.0;
    double post_first_b = -1.0;
    double post_total_res = -1.0;
    double blocked_ledger_c = -1.0;
    double reentry_ledger_c = -1.0;
    double final_ledger_c = -1.0;
    double final_pos = -1.0;

    double total_reservations() const {
        double total = 0.0;
        for (const auto& kv : close_reserved_qty_) total += kv.second;
        return total;
    }

    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("seed", true, na, na, 5.0);
        if (bar_index_ == 1) {
            // Model a FIFO history where logical ids overlap the five live
            // physical units: R already owns two reserved units, while B's
            // surviving close can fill the remaining three exactly.
            id_unclosed_qty_.clear();
            close_reserved_qty_.clear();
            close_two_call_first_qty_.clear();
            id_unclosed_qty_["A"] = 1.0;
            id_unclosed_qty_["B"] = 3.0;
            id_unclosed_qty_["R"] = 2.0;
            close_reserved_qty_["R"] = 2.0;
            strategy_close("A", "first");
            strategy_close("B", "survivor");
        }
        if (bar_index_ == 2) {
            post_pos = signed_position_size();
            post_ledger_b = ledger("B");
            post_res_b = reservation("B");
            post_first_b = two_call_first_qty("B");
            post_total_res = total_reservations();

            id_unclosed_qty_["C"] = 1.0;
            strategy_close("C", "blocked");  // no unreserved physical qty
            blocked_ledger_c = ledger("C");
            strategy_entry("C", true, na, na, 1.0);
        }
        if (bar_index_ == 3) {
            reentry_ledger_c = ledger("C");
            strategy_close("C", "fresh-cycle");
        }
        if (bar_index_ == 4) {
            final_ledger_c = ledger("C");
            final_pos = signed_position_size();
        }
    }
};

static void test_zero_backed_close_reservation_clears_stale_cycle() {
    std::printf("test_zero_backed_close_reservation_clears_stale_cycle\n");
    ZeroBackedCloseReservationStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50,  60000},
        {100, 101, 99, 100, 50, 120000},
        {100, 101, 99, 100, 50, 180000},
        {100, 101, 99, 100, 50, 240000},
        {100, 101, 99, 100, 50, 300000},
    };
    strat.run(bars, 5);

    CHECK(near(strat.post_pos, 2.0));
    CHECK(near(strat.post_ledger_b, 0.0));
    CHECK(near(strat.post_res_b, 0.0));
    CHECK(near(strat.post_first_b, 0.0));
    CHECK(near(strat.post_total_res, 2.0));
    CHECK(near(strat.blocked_ledger_c, 0.0));
    CHECK(near(strat.reentry_ledger_c, 1.0));
    CHECK(near(strat.final_ledger_c, 0.0));
    CHECK(near(strat.final_pos, 2.0));
    CHECK(strat.trade_count() == 2);
}

// ENA control: a positive truncated reservation is still a live replacement
// chain. Keep the survivor's established logical ledger, but clamp the new
// physical reservation and drop exact-two provenance because the fill is not
// fully backed. This is intentionally distinct from the zero-backed ETH case.
class PositiveTruncatedCloseReservationStrategy : public CloseReplacementProbeBase {
public:
    double final_pos = -1.0;
    double ledger_b = -1.0;
    double res_b = -1.0;
    double first_b = -1.0;
    double total_res = -1.0;

    void on_bar(const Bar&) override {
        double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("seed", true, na, na, 6.0);
        if (bar_index_ == 1) {
            id_unclosed_qty_.clear();
            close_reserved_qty_.clear();
            close_two_call_first_qty_.clear();
            id_unclosed_qty_["A"] = 1.0;
            id_unclosed_qty_["B"] = 4.0;
            id_unclosed_qty_["R"] = 1.0;
            close_reserved_qty_["R"] = 1.0;
            strategy_close("A", "first");
            strategy_close("B", "survivor");
        }
        if (bar_index_ == 2) {
            final_pos = signed_position_size();
            ledger_b = ledger("B");
            res_b = reservation("B");
            first_b = two_call_first_qty("B");
            total_res = 0.0;
            for (const auto& kv : close_reserved_qty_) total_res += kv.second;
        }
    }
};

static void test_positive_truncated_close_reservation_keeps_ledger_only() {
    std::printf("test_positive_truncated_close_reservation_keeps_ledger_only\n");
    PositiveTruncatedCloseReservationStrategy strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50,  60000},
        {100, 101, 99, 100, 50, 120000},
        {100, 101, 99, 100, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(near(strat.final_pos, 2.0));
    CHECK(near(strat.ledger_b, 4.0));
    CHECK(near(strat.res_b, 1.0));
    CHECK(near(strat.first_b, 0.0));
    CHECK(near(strat.total_res, 2.0));
}

// ---- 38b. process_orders_on_close: an exit gated on position visibility must
// NOT fire on the entry bar. TradingView does not expose a just-placed market
// entry through strategy.position_size until the next bar, so a regime/bias
// style `if strategy.position_size != 0 => strategy.close()` cannot close the
// position on the bar it was opened. Regression guard for the Quant-Synthesis
// [JOAT] same-bar-close family (engine previously immediate-filled POOC market
// entries and produced spurious zero-duration trades).

class EntryBarCloseGuardStrategy : public BacktestEngine {
public:
    int close_calls_on_entry_bar = 0;
    EntryBarCloseGuardStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& bar) override {
        (void)bar;
        if (bar_index_ == 0) strategy_entry("L", true);
        if (signed_position_size() != 0.0) {
            if (bar_index_ == 0) ++close_calls_on_entry_bar;
            strategy_close("L");
        }
    }
};

static void test_pooc_exit_not_triggered_on_entry_bar() {
    std::printf("test_pooc_exit_not_triggered_on_entry_bar\n");
    EntryBarCloseGuardStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},     // long entry placed; fills at close
        {100, 110, 98, 108, 50, 120000},    // position now visible -> close here
        {108, 112, 105, 110, 50, 180000},
    };
    strat.run(bars, 3);

    // The gated close must never fire on the entry bar.
    CHECK(strat.close_calls_on_entry_bar == 0);
    // Exactly one closed trade, not a zero-duration same-bar trade: entered at
    // bar-0 close (100), closed at bar-1 close (108) once the position became
    // visible.
    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).pnl, 8.0));  // long 100 -> 108
}

// ---- 39. Commission impact on P&L

class CommissionStrategy : public BacktestEngine {
public:
    CommissionStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;  // 0.1% per trade
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 2) strategy_close("L");
    }
};

static void test_commission_deducted() {
    std::printf("test_commission_deducted\n");
    CommissionStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},    // entry at 100, qty=10
        {100, 110, 98, 108, 50, 120000},
        {108, 112, 105, 110, 50, 180000},   // exit at 110
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    // Gross PnL = (110-100)*10 = 100
    // Entry commission = 100 * 10 * 0.001 = 1.0
    // Exit commission = 110 * 10 * 0.001 = 1.1
    // Net PnL = 100 - 1.0 - 1.1 = 97.9
    double pnl = strat.get_trade(0).pnl;
    CHECK(near(pnl, 97.9, 0.01));
}

// ---- 40. Slippage impact

class SlippageStrategy : public BacktestEngine {
public:
    SlippageStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 5;         // 5 ticks
        syminfo_mintick_ = 0.1; // tick = 0.1, so 5 ticks = 0.5
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 2) strategy_close("L");
    }
};

static void test_slippage_applied() {
    std::printf("test_slippage_applied\n");
    SlippageStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 110, 98, 108, 50, 120000},   // entry fills at open=100 + slippage=0.5 = 100.5
        {108, 112, 105, 110, 50, 180000},
        {110, 115, 108, 112, 50, 240000},  // exit fills at open=110 - slippage=0.5 = 109.5
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    // Entry: 100 + 0.5 = 100.5 (buy slips up)
    // Exit: 110 - 0.5 = 109.5 (sell slips down)
    CHECK(near(strat.get_trade(0).entry_price, 100.5, 0.01));
    CHECK(near(strat.get_trade(0).exit_price, 109.5, 0.01));
    CHECK(near(strat.get_trade(0).pnl, 9.0, 0.01));  // 109.5 - 100.5
}

// ---- 41. qty_type = PERCENT_OF_EQUITY

class PercentEquityStrategy : public BacktestEngine {
public:
    PercentEquityStrategy() {
        initial_capital_ = 10000;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 50.0;  // 50% of equity
        commission_value_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& bar) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 2) strategy_close("L");
    }
};

static void test_qty_percent_of_equity() {
    std::printf("test_qty_percent_of_equity\n");
    PercentEquityStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},    // entry at 100
        {100, 110, 98, 108, 50, 120000},
        {108, 112, 105, 110, 50, 180000},   // exit at 110
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    // qty = (10000 * 0.50) / 100 = 50
    CHECK(near(strat.get_trade(0).qty, 50.0, 0.01));
    // PnL = (110-100) * 50 = 500
    CHECK(near(strat.get_trade(0).pnl, 500.0, 0.01));
}

static void test_qty_percent_of_equity_includes_open_profit_for_pyramid_add() {
    std::printf("test_qty_percent_of_equity_includes_open_profit_for_pyramid_add\n");
    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 10000;
            default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            default_qty_value_ = 50.0;
            commission_value_ = 0;
            slippage_ = 0;
            pyramiding_ = 2;
            process_orders_on_close_ = true;
        }
        double calc_add_qty() {
            current_bar_ = {110, 110, 110, 110, 50, 120000};
            position_side_ = PositionSide::LONG;
            position_qty_ = 50.0;
            position_entry_price_ = 100.0;
            return calc_qty(110.0);
        }
        void on_bar(const Bar&) override {}
    } strat;

    // The second default percent-of-equity entry sizes from live
    // strategy.equity: 10000 closed equity + 500 unrealized open profit,
    // then 50% of that at a 110 close fill.
    CHECK(near(strat.calc_add_qty(), 5250.0 / 110.0, 0.01));
}

// ---- main -------------------------------------------------------------------

// ---- Price path fill priority tests ----------------------------------------

static void test_price_path_bullish_stop_first() {
    std::printf("test_price_path_bullish_stop_first\n");
    // Bullish bar: O=100, L=90, H=115, C=108
    // Path: O(100) -> L(90) -> H(115) -> C(108)
    // Long position with stop=95, limit=110
    // Stop at 95 is hit first (on the way down to L=90)

    class StopFirstStrategy : public BacktestEngine {
    public:
        StopFirstStrategy() {
            initial_capital_ = 100000; default_qty_value_ = 1.0;
            commission_value_ = 0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", 110.0, 95.0);
            }
        }
    };

    StopFirstStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 115, 90, 108, 50, 120000},
        {108, 112, 105, 110, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 95.0, 0.5));
}

static void test_price_path_bearish_limit_first() {
    std::printf("test_price_path_bearish_limit_first\n");
    // Bearish bar: O=110, H=120, L=90, C=95
    // Path: O(110) -> H(120) -> L(90) -> C(95)
    // Long position with stop=92, limit=115
    // Limit at 115 is hit first (on the way up to H=120)

    class LimitFirstStrategy : public BacktestEngine {
    public:
        LimitFirstStrategy() {
            initial_capital_ = 100000; default_qty_value_ = 1.0;
            commission_value_ = 0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", 115.0, 92.0);
            }
        }
    };

    LimitFirstStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {110, 120, 90, 95, 50, 120000},
        {95, 100, 92, 98, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 115.0, 0.5));
}

static void test_price_path_short_stop_first() {
    std::printf("test_price_path_short_stop_first\n");
    // Short position with stop=115 (buy back above), limit=90 (buy back below)
    // Bearish bar: O=110, H=120, L=85, C=95
    // Path: O(110) -> H(120) -> L(85) -> C(95)
    // Stop at 115 hit first (on the way up to H=120)

    class ShortStopStrategy : public BacktestEngine {
    public:
        ShortStopStrategy() {
            initial_capital_ = 100000; default_qty_value_ = 1.0;
            commission_value_ = 0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("S", false);
                strategy_exit("X", "S", 90.0, 115.0);
            }
        }
    };

    ShortStopStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {110, 120, 85, 95, 50, 120000},
        {95, 100, 90, 98, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 115.0, 0.5));
}

static void test_price_path_vs_open_proximity() {
    std::printf("test_price_path_vs_open_proximity\n");
    // Case where open proximity gives WRONG answer but price path is correct
    // Bullish bar: O=105, L=90, H=120, C=112
    // stop=93, limit=108
    // Open proximity: dist to stop=|105-93|=12, dist to limit=|105-108|=3 -> limit first (WRONG)
    // Price path: O(105) -> L(90) -> H(120) -> C(112)
    //   On way to L(90): crosses 93 (stop)
    //   On way to H(120): crosses 108 (limit)
    //   -> stop hit first (CORRECT)

    class PathVsProximityStrategy : public BacktestEngine {
    public:
        PathVsProximityStrategy() {
            initial_capital_ = 100000; default_qty_value_ = 1.0;
            commission_value_ = 0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", 108.0, 93.0);
            }
        }
    };

    PathVsProximityStrategy strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {105, 120, 90, 112, 50, 120000},
        {112, 115, 110, 113, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 93.0, 0.5));
}

// TradingView's broker emulator chooses the first leg from whether open is
// closer to high or low, not from candle color. A bullish bar can still go
// O->H first when the open is near the high.
static void test_price_path_bullish_open_near_high_hits_limit_first() {
    std::printf("test_price_path_bullish_open_near_high_hits_limit_first\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", 111.0, 95.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 105.0, 95.0, 100.0, 50, 60'000},
        // Bullish bar, but open is much closer to high than low:
        // |112 - 110| = 2 vs |110 - 90| = 20, so path should be O->H->L->C.
        {110.0, 112.0, 90.0, 111.0, 50, 120'000},
        {111.0, 113.0, 109.0, 112.0, 50, 180'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 111.0, 0.5));
}

// Even on a bearish bar, the broker emulator should go O->L->H->C when the
// open is closer to the low. That means a long stop can trigger before a later
// take-profit on the rebound.
static void test_price_path_open_near_low_hits_stop_first() {
    std::printf("test_price_path_open_near_low_hits_stop_first\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
                strategy_exit("X", "L", 110.0, 95.0);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100, 105, 95, 100, 50, 60000},
        {100, 125, 80, 85, 50, 120000},
        {85, 90, 82, 88, 50, 180000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 95.0, 0.5));
}

// When opposite stop entries are both crossed in one bar while flat, the one
// touched first along the OHLC path should win (not insertion order).
static void test_opposite_stop_entries_follow_path_order() {
    std::printf("test_opposite_stop_entries_follow_path_order\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (position_side_ == PositionSide::FLAT) {
                // Insertion order intentionally long then short.
                strategy_entry("LStop", true, na<double>(), 105.0);
                strategy_entry("SStop", false, na<double>(), 95.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        double get_entry_price() const { return position_entry_price_; }
    };

    Strat strat;
    Bar bars[] = {
        // Place both stop entries on first bar.
        {100, 100, 100, 100, 50, 900'000},
        // Bullish bar path is O->L->H->C: 100 -> 90 -> 110 -> 105.
        // Short stop 95 is crossed on O->L before long stop 105 on L->H.
        {100, 110, 90, 105, 50, 1'800'000},
    };

    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);  // no exit yet
    CHECK(strat.get_signed_position_size() < 0.0);
    CHECK(near(strat.get_entry_price(), 95.0, 0.5));
}

// The opposing-stop arbitration helper also has to follow the open-proximity
// path rule, not candle color. This bearish bar is still O->L->H->C because
// the open is nearer the low.
static void test_opposite_stop_entries_use_open_proximity_path_priority() {
    std::printf("test_opposite_stop_entries_use_open_proximity_path_priority\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (position_side_ == PositionSide::FLAT) {
                strategy_entry("LStop", true, na<double>(), 105.0);
                strategy_entry("SStop", false, na<double>(), 97.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        double get_entry_price() const { return position_entry_price_; }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        // Bearish bar, but open is much closer to low than high:
        // |100 - 94| = 6 vs |110 - 100| = 10, so path should be O->L->H->C.
        // The short stop at 97 is touched before the long stop at 105.
        {100.0, 110.0, 94.0, 96.0, 50, 1'800'000},
    };

    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(strat.get_signed_position_size() < 0.0);
    CHECK(near(strat.get_entry_price(), 97.0, 0.5));
}

// strategy.close(id) must only close entries matching that id.

static void test_strategy_entry_oca_cancel_group() {
    std::printf("test_strategy_entry_oca_cancel_group\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, na<double>(), 105.0, na<double>(), "", "entry_oca", 1);
                strategy_entry("S", false, na<double>(), 95.0, na<double>(), "", "entry_oca", 1);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        // Path O->L->H->C touches short stop first; long stop would be touched
        // later, but OCA cancel must remove it after short fills.
        {100.0, 110.0, 90.0, 105.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 0);
    CHECK(strat.get_signed_position_size() < 0.0);
}


static void test_strategy_entry_qty_type_cash_overrides_default_sizing() {
    std::printf("test_strategy_entry_qty_type_cash_overrides_default_sizing\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true, na<double>(), na<double>(), 1000.0, "", "", 0, 2);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 101.0, 99.0, 100.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).qty, 10.0, 1e-9));
    }
}

static void test_strategy_close_respects_entry_id() {
    std::printf("test_strategy_close_respects_entry_id\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Long", true);
            } else if (bar_index_ == 1) {
                // Should be a no-op: there is no "Short" entry to close.
                strategy_close("Short");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50, 900'000},
        {100, 102, 98, 101, 50, 1'800'000},
        {101, 103, 100, 102, 50, 2'700'000},
    };

    strat.run(bars, 3);

    CHECK(strat.trade_count() == 0);
    CHECK(strat.get_signed_position_size() > 0.0);
}

static void test_market_close_fills_before_same_bar_opposite_stop_entry() {
    std::printf("test_market_close_fills_before_same_bar_opposite_stop_entry\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                // Match probe 63 ordering: opposite stop is submitted before the
                // market close. TV still closes the existing position at next
                // bar open before evaluating the opposite stop entry.
                strategy_entry("S", false, na<double>(), 95.0);
                strategy_close("L");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 50, 900'000},
        {100.0, 100.0, 100.0, 100.0, 50, 1'800'000},
        {100.0, 105.0, 90.0, 101.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).is_long);
        CHECK(near(strat.get_trade(0).exit_price, 100.0, 1e-9));
    }
    CHECK(strat.get_signed_position_size() < 0.0);
}

// strategy.close(id) issued while that id is absent should not persist and
// later close a future position with that id.
static void test_strategy_close_non_matching_does_not_persist() {
    std::printf("test_strategy_close_non_matching_does_not_persist\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_value_ = 1.0;
            commission_value_ = 0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Long", true);
            } else if (bar_index_ == 1) {
                // Non-matching close request while long.
                strategy_close("Short");
            } else if (bar_index_ == 2) {
                // Flip into short.
                strategy_close("Long");
                strategy_entry("Short", false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100, 101, 99, 100, 50, 900'000},
        {100, 102, 98, 101, 50, 1'800'000},
        {101, 103, 100, 102, 50, 2'700'000},
        {102, 103, 100, 101, 50, 3'600'000},
        {101, 102, 99, 100, 50, 4'500'000},
    };

    strat.run(bars, 5);

    // One closed trade from Long->Short flip, and final position should remain short.
    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_signed_position_size() < 0.0);
}

// strategy.exit orders created for a previous position must not leak into
// future positions after a market close/reversal.
static void test_stale_exit_does_not_carry_to_future_position() {
    std::printf("test_stale_exit_does_not_carry_to_future_position\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Long", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("X", "Long", 120.0, 95.0);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                // Close by market, leaving any pending exits stale.
                strategy_close("Long");
            } else if (bar_index_ == 3 && signed_position_size() == 0.0) {
                strategy_entry("Long", true);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 101.0, 99.0, 100.0, 50, 2'700'000},
        {100.0, 101.0, 99.0, 100.0, 50, 3'600'000},
        {100.0, 101.0, 99.0, 100.0, 50, 4'500'000},
        // Would hit stale stop=95 if old exit bracket leaked.
        {100.0, 101.0, 94.0, 96.0, 50, 5'400'000},
    };

    strat.run(bars, 6);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_signed_position_size() > 0.0);
}

// For separate OCA exit orders (RAW_ORDER stop/limit), fill priority should
// follow first touch on the OHLC path, not insertion order.
static void test_oca_exit_orders_follow_path_priority() {
    std::printf("test_oca_exit_orders_follow_path_priority\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                // Intentionally insert TP first, then SL.
                strategy_order("TP", false, 1.0, 110.0, na<double>(), "TPSL", 2);
                strategy_order("SL", false, 1.0, na<double>(), 95.0, "TPSL", 2);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        // Bullish path O->L->H->C: 100->90->120->110, so SL(95) is touched first.
        {100.0, 120.0, 90.0, 110.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_trade(0).exit_price, 95.0, 0.5));
}

// The OCA sibling ordering helper must also honor the open-proximity path
// rule. This bullish bar still runs O->H->L->C because the open is nearer the
// high, so the TP fires before the later stop.
static void test_oca_exit_orders_use_open_proximity_path_priority() {
    std::printf("test_oca_exit_orders_use_open_proximity_path_priority\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                // Insert the stop first so a regression in sibling ordering would
                // wrongly fill it before the later-touched TP.
                strategy_order("SL", false, 1.0, na<double>(), 95.0, "TPSL", 2);
                strategy_order("TP", false, 1.0, 111.0, na<double>(), "TPSL", 2);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        // Bullish bar, but open is much closer to high than low:
        // |112 - 109| = 3 vs |109 - 90| = 19, so path should be O->H->L->C.
        // The TP at 111 is touched before the stop at 95.
        {109.0, 112.0, 90.0, 110.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).exit_id == "TP");
    CHECK(near(strat.get_trade(0).exit_price, 111.0, 0.5));
}

// A same-direction stop entry that no-ops due pyramiding limit must not consume
// the bar's priced-entry slot; an opposite stop touched later should still fill.
static void test_noop_entry_does_not_block_later_opposite_stop() {
    std::printf("test_noop_entry_does_not_block_later_opposite_stop\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 1;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L0", true);
            }
            if (bar_index_ == 1) {
                // Same-direction long stop touched first on next bar -> no-op (pyramiding max).
                strategy_entry("L1", true, na<double>(), 102.0);
                // Opposite short stop touched later on next bar -> should still reverse.
                strategy_entry("S1", false, na<double>(), 95.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        // Bearish path O->H->L->C: touches 102 first, then 95.
        {100.0, 103.0, 94.0, 96.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_signed_position_size() < 0.0);
}

// If a full exit for the same from_entry is already pending, a later partial
// exit should be ignored (TradingView-style precedence).
static void test_partial_exit_ignored_when_full_exit_present() {
    std::printf("test_partial_exit_ignored_when_full_exit_present\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (bar_index_ == 1) {
                strategy_exit("FULL", "L", 120.0, 95.0);
                strategy_exit("PART", "L",
                    105.0,                                    // limit
                    std::numeric_limits<double>::quiet_NaN(), // no stop
                    std::numeric_limits<double>::quiet_NaN(), // no trail_points
                    std::numeric_limits<double>::quiet_NaN(), // no trail_offset
                    std::numeric_limits<double>::quiet_NaN(), // no trail_price
                    50.0);                                    // qty_percent
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        // Touches PART limit (105), but not FULL limit (120) nor FULL stop (95).
        {100.0, 110.0, 96.0, 108.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 1.0, 1e-9));
}

static void test_partial_exit_reservation_limits_full_exit() {
    std::printf("test_partial_exit_reservation_limits_full_exit\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 2.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            }
            if (signed_position_size() > 0.0) {
                strategy_exit("HALF", "L",
                    105.0,
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    50.0);
                strategy_exit("REST", "L",
                    std::numeric_limits<double>::quiet_NaN(),
                    95.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 101.0, 99.0, 100.0, 50, 1'800'000},
        {100.0, 102.0, 94.0, 96.0, 50, 2'700'000},
        {100.0, 106.0, 99.0, 105.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 2);
    CHECK(near(strat.get_trade(0).qty, 1.0, 1e-9));
    CHECK(near(strat.get_trade(1).qty, 1.0, 1e-9));
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

// With process_orders_on_close=false, priced exits created on a bar should be
// eligible only from the next bar (no same-bar retroactive fill).
static void test_priced_exit_not_filled_same_bar_when_pooc_false() {
    std::printf("test_priced_exit_not_filled_same_bar_when_pooc_false\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                // Market entry fills on next bar open.
                strategy_entry("L", true);
            }
            if (bar_index_ == 2 && signed_position_size() > 0) {
                // Stop sits at current close, so same-bar retroactive fill would trigger.
                strategy_exit("X", "L",
                    std::numeric_limits<double>::quiet_NaN(),
                    bar.close,
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN(),
                    100.0);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000}, // entry fills here (after on_bar)
        {102.0, 103.0, 95.0, 100.0, 50, 2'700'000},  // exit order is created here
        {106.0, 108.0, 105.0, 107.0, 50, 3'600'000}, // would not hit stop=100 if deferred
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 0);
    CHECK(strat.get_signed_position_size() > 0.0);
}

static void test_strategy_close_cancels_prior_pending_entries_but_keeps_same_pass_reversal() {
    std::printf("test_strategy_close_cancels_prior_pending_entries_but_keeps_same_pass_reversal\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("stale_long", true, na<double>(), 110.0);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("L");
                strategy_entry("S", false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 106.0, 100.0, 101.0, 50, 2'700'000},
        {99.0, 105.0, 95.0, 96.0, 50, 3'600'000},
        {96.0, 112.0, 90.0, 91.0, 50, 4'500'000},
    };
    strat.run(bars, 5);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).entry_id == "L");
    CHECK(strat.get_signed_position_size() < 0.0);
}

static void test_strategy_close_any_non_matching_keeps_pending_entry_live() {
    std::printf("test_strategy_close_any_non_matching_keeps_pending_entry_live\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
            close_entries_rule_any_ = true;
            pyramiding_ = 2;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("add_long", true, na<double>(), 110.0);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("missing");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 109.0, 100.0, 103.0, 50, 2'700'000},
        {103.0, 112.0, 101.0, 111.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 2.0, 1e-9));
}

static void test_strategy_close_pooc_missing_id_noops() {
    std::printf("test_strategy_close_pooc_missing_id_noops\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("Missing");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 106.0, 101.0, 105.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 0);
    CHECK(near(strat.get_signed_position_size(), 1.0, 1e-9));
}

static void test_strategy_close_pooc_cancels_same_bar_market_reentry() {
    std::printf("test_strategy_close_pooc_cancels_same_bar_market_reentry\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 2;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L");
                strategy_entry("L_add", true);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 106.0, 101.0, 105.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_strategy_close_pooc_keeps_same_bar_market_reversal() {
    std::printf("test_strategy_close_pooc_keeps_same_bar_market_reversal\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L");
                strategy_entry("S", false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 106.0, 101.0, 105.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_signed_position_size() < 0.0);
}

static void test_strategy_close_immediate_cancels_prior_same_bar_market_reentry() {
    std::printf("test_strategy_close_immediate_cancels_prior_same_bar_market_reentry\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 2;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("L_add", true);
                strategy_close("L", "", na<double>(), na<double>(), true);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {104.0, 106.0, 101.0, 105.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_strategy_close_pooc_keeps_same_bar_pending_entry() {
    std::printf("test_strategy_close_pooc_keeps_same_bar_pending_entry\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L0", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("L1", true, std::numeric_limits<double>::quiet_NaN(), 110.0);
                strategy_close("L0");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        std::string get_open_entry_id() const { return open_trade_entry_id(0); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 106.0, 99.0, 105.0, 50, 1'800'000},
        {106.0, 112.0, 104.0, 111.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).entry_id == "L0");
    CHECK(strat.get_signed_position_size() > 0.0);
    CHECK(strat.get_open_entry_id() == "L1");
}

static void test_strategy_close_pooc_partial_close_keeps_other_exit_bracket() {
    std::printf("test_strategy_close_pooc_partial_close_keeps_other_exit_bracket\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
            close_entries_rule_any_ = true;
            pyramiding_ = 2;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("A", true);
            } else if (bar_index_ == 1) {
                strategy_entry("B", true);
                strategy_exit("XB", "B", 115.0, std::numeric_limits<double>::quiet_NaN());
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("A");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 111.0, 99.0, 110.0, 50, 1'800'000},
        {110.0, 114.0, 108.0, 112.0, 50, 2'700'000},
        {112.0, 116.0, 111.0, 115.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 2);
    CHECK(strat.get_trade(0).entry_id == "A");
    CHECK(strat.get_trade(1).entry_id == "B");
    CHECK(strat.get_trade(1).exit_id == "XB");
    CHECK(strat.get_trade(1).exit_bar_index == 3);
    CHECK(near(strat.get_trade(1).exit_price, 115.0, 1e-9));
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

// Under default FIFO trade reporting, strategy.close(id) should only close the
// quantity associated with the requested id. The closed trade is still reported
// FIFO, so closing "Buy2" here should close Buy1's leg and leave Buy2 open.
static void test_strategy_close_fifo_only_closes_requested_leg_size() {
    std::printf("test_strategy_close_fifo_only_closes_requested_leg_size\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
            pyramiding_ = 2;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Buy1", true);
            } else if (bar_index_ == 1) {
                strategy_entry("Buy2", true);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("Buy2");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        int get_open_trade_count() const { return static_cast<int>(pyramid_entries_.size()); }
        std::string get_open_entry_id() const { return open_trade_entry_id(0); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {101.0, 102.0, 100.0, 101.0, 50, 1'800'000},
        {102.0, 103.0, 101.0, 102.0, 50, 2'700'000},
        {103.0, 104.0, 102.0, 103.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).entry_id == "Buy1");
    CHECK(near(strat.get_signed_position_size(), 1.0, 1e-9));
    CHECK(strat.get_open_trade_count() == 1);
    CHECK(strat.get_open_entry_id() == "Buy2");
}

// The default FIFO close(id) partial-sizing path also has an immediate
// process_orders_on_close branch. It should still close only the requested
// leg's quantity while reporting the closed trade FIFO.
static void test_strategy_close_pooc_fifo_only_closes_requested_leg_size() {
    std::printf("test_strategy_close_pooc_fifo_only_closes_requested_leg_size\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
            pyramiding_ = 2;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Buy1", true);
            } else if (bar_index_ == 1) {
                strategy_entry("Buy2", true);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("Buy2");
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        int get_open_trade_count() const { return static_cast<int>(pyramid_entries_.size()); }
        std::string get_open_entry_id() const { return open_trade_entry_id(0); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {110.0, 111.0, 109.0, 110.0, 50, 1'800'000},
        {120.0, 121.0, 119.0, 120.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).entry_id == "Buy1");
    CHECK(strat.get_trade(0).exit_bar_index == 2);
    CHECK(near(strat.get_trade(0).exit_price, 120.0, 1e-9));
    CHECK(near(strat.get_signed_position_size(), 1.0, 1e-9));
    CHECK(strat.get_open_trade_count() == 1);
    CHECK(strat.get_open_entry_id() == "Buy2");
}

// Grid-bot pattern: the SAME entry id is re-used across sequential
// buy/close cycles. Under the default FIFO close-entries rule, the trade
// record drains the OLDEST physical lot (a different id), so the id-tagged
// lot stays physically open after its close. A later re-entry of that id then
// leaves TWO physical lots carrying it, while only ONE is logically unclosed.
// strategy.close(id) must close ONE slot (the logical/unclosed quantity), not
// the physical sum of both lots — otherwise it over-closes 2x (the bug this
// guards). Mirrors the 3Commas grid-bot corpus strategies.
static void test_strategy_close_reused_id_closes_one_logical_slot() {
    std::printf("test_strategy_close_reused_id_closes_one_logical_slot\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
            pyramiding_ = 10;
        }
        void on_bar(const Bar&) override {
            switch (bar_index_) {
                case 0: strategy_entry("A", true); break;  // older, different id
                case 1: strategy_entry("L", true); break;  // L lot #1
                case 2: strategy_close("L");        break;  // drains A (FIFO), L#1 stays
                case 3: strategy_entry("L", true); break;  // L lot #2 (re-use id)
                case 4: strategy_close("L");        break;  // must close ONE slot, not two
                default: break;
            }
        }
        double pos() const { return signed_position_size(); }
        int open_lots() const { return static_cast<int>(pyramid_entries_.size()); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 50,   900'000},
        { 90.0,  91.0,  89.0,  90.0, 50, 1'800'000},
        {110.0, 111.0, 109.0, 110.0, 50, 2'700'000},
        { 80.0,  81.0,  79.0,  80.0, 50, 3'600'000},
        {120.0, 121.0, 119.0, 120.0, 50, 4'500'000},
    };
    strat.run(bars, 5);

    // Two close("L") calls each close exactly one unit: 2 closed trades, and
    // one L lot (qty 1) remains open. The pre-fix sum-of-id behaviour closed
    // both physical L lots on bar 4 (3 trades, flat) — that is the regression.
    CHECK(strat.trade_count() == 2);
    CHECK(near(strat.pos(), 1.0, 1e-9));
    CHECK(strat.open_lots() == 1);
}

static void test_strategy_close_pooc_sets_exit_comment() {
    std::printf("test_strategy_close_pooc_sets_exit_comment\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L", "manual close");
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {110.0, 111.0, 109.0, 110.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).exit_comment == "manual close");
    }
}


static void test_strategy_close_qty_percent_reduces_position() {
    std::printf("test_strategy_close_qty_percent_reduces_position\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 10.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L", "half", na<double>(), 50.0, false);
            } else if (bar_index_ == 2 && signed_position_size() > 0.0) {
                strategy_close("L", "rest", na<double>(), na<double>(), false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {110.0, 111.0, 109.0, 110.0, 50, 1'800'000},
        {120.0, 121.0, 119.0, 120.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 2);
    if (strat.trade_count() == 2) {
        CHECK(near(strat.get_trade(0).qty, 5.0, 1e-9));
        CHECK(strat.get_trade(0).exit_comment == "half");
        CHECK(near(strat.get_trade(1).qty, 5.0, 1e-9));
    }
    CHECK(near(strat.get_signed_position_size(), 0.0, 1e-9));
}

static void test_strategy_close_qty_reduces_position() {
    std::printf("test_strategy_close_qty_reduces_position\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 10.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L", "three", 3.0, na<double>(), false);
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {110.0, 111.0, 109.0, 110.0, 50, 1'800'000},
    };
    strat.run(bars, 2);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(near(strat.get_trade(0).qty, 3.0, 1e-9));
    }
    CHECK(near(strat.get_signed_position_size(), 7.0, 1e-9));
}

static void test_strategy_close_immediately_fills_current_close() {
    std::printf("test_strategy_close_immediately_fills_current_close\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close("L", "now", na<double>(), na<double>(), true);
            }
        }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {110.0, 112.0, 109.0, 111.0, 50, 1'800'000},
        {120.0, 121.0, 119.0, 120.0, 50, 2'700'000},
    };
    strat.run(bars, 3);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        CHECK(strat.get_trade(0).exit_bar_index == 1);
        CHECK(near(strat.get_trade(0).exit_price, 111.0, 1e-9));
    }
}

static void test_stale_close_all_does_not_close_future_reentry() {
    std::printf("test_stale_close_all_does_not_close_future_reentry\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            process_orders_on_close_ = false;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_exit("TP", "L", 120.0, std::numeric_limits<double>::quiet_NaN());
                strategy_entry("S", false);
                strategy_close_all();
            }
        }
        double get_signed_position_size() const { return signed_position_size(); }
        std::string get_open_entry_id() const { return open_trade_entry_id(0); }
    };

    Strat strat;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 50, 900'000},
        {100.0, 105.0, 99.0, 104.0, 50, 1'800'000},
        {100.0, 125.0, 90.0, 95.0, 50, 2'700'000},
        {95.0, 98.0, 92.0, 94.0, 50, 3'600'000},
    };
    strat.run(bars, 4);

    CHECK(strat.trade_count() == 1);
    CHECK(strat.get_trade(0).entry_id == "L");
    CHECK(strat.get_signed_position_size() < 0.0);
    CHECK(strat.get_open_entry_id() == "S");
}

int main() {
    // Composed TA indicators
    test_ema_of_sma();
    test_composed_recompute();
    test_rsi_of_hl2();
    test_bb_of_atr();
    test_macd_composition();
    test_rsi_sma_bb_chain();
    test_stoch_sma_chain();
    test_ema_chain_recompute();

    // NaN propagation
    test_nan_propagation();

    // Timeframe aggregator
    test_aggregator_single_bar();
    test_aggregator_exact_ratio();
    test_aggregator_volume_accumulation();
    test_aggregator_high_low();

    // Price path sampling
    test_flat_bar_sampling();
    test_high_sample_count();

    // Strategy engine
    test_engine_empty_bars();
    test_engine_single_bar();
    test_request_security_gaps_on_emits_na_between_completions();
    test_priced_entry_not_filled_same_bar_when_pooc_false();
    test_priced_entry_fill_rounds_to_mintick();
    test_barstate_flags_simple_run();
    test_barstate_flags_magnifier_run();
    test_buy_stop_limit_requires_stop_before_limit_on_path();
    test_buy_stop_limit_fills_when_limit_seen_after_activation();
    test_sell_stop_limit_requires_stop_before_limit_on_path();
    test_sell_stop_limit_fills_when_limit_seen_after_activation();
    test_risk_max_position_size();
    test_allow_entry_in_opposite_entry_closes_without_reversing();
    test_blocked_entry_does_not_consume_intraday_fill_quota();
    test_flat_bracket_dual_stop_closes_on_opposite_touch();
    test_flat_bracket_dual_stop_cross_bar_closes_on_opposite_touch();
    test_flat_bracket_dual_stop_open_equals_stop_prefers_long();
    test_flat_armed_priced_entries_pyramid_within_one_bar();
    test_trail_points_activation_ceils_to_mintick();
    test_exit_profit_loss_materializes_after_pending_entry_fill();
    test_strategy_pnl_roundtrip();
    test_per_trade_extremes();
    test_process_orders_on_close();

    // Magnifier
    test_magnifier_sub_bar_count();

    // Standalone indicators
    test_supertrend_basic();
    test_dmi_basic();

    // Multi-indicator and advanced strategy tests
    test_multi_indicator_confluence();
    test_position_reversal();
    test_reversal_uses_explicit_qty_for_new_side();
    test_pyramid_partial_exit();
    test_exit_qty_percent_reduces_position();
    test_partial_exit_id_fills_once_per_position();
    test_close_entries_any();
    test_trailing_stop();
    test_limit_exit_beats_trailing_stop_after_activation();
    test_trailing_stop_fills_at_crossing_level_after_activation();
    test_trailing_stop_does_not_lookahead_bar_high_at_open();
    test_trailing_stop_ignores_entry_bar_extreme_before_exit_creation();
    test_trailing_points_without_offset_exits_at_activation();
    test_magnifier_limit_fill();
    test_magnifier_volume_weighted_toggle();
    test_magnifier_ta_consistency();
    test_risk_halt_max_drawdown();
    test_mae_mfe_exposed_in_report();
    test_equity_extremes_accuracy();
    test_series_history();
    test_pooc_stop_deferred();
    test_oca_one_cancels_other();

    // Position management tests
    test_position_long_lifecycle();
    test_position_short_lifecycle();
    test_pyramid_avg_price();
    test_win_loss_tracking();
    test_position_reversal_state();
    test_same_bar_multi_close_single_fill();
    test_exact_two_call_replacement_carries_prior_first_target();
    test_three_call_batch_does_not_create_two_call_provenance();
    test_three_call_current_batch_invalidates_two_call_carry();
    test_zero_backed_close_reservation_clears_stale_cycle();
    test_positive_truncated_close_reservation_keeps_ledger_only();
    test_pooc_exit_not_triggered_on_entry_bar();
    test_commission_deducted();
    test_slippage_applied();
    test_qty_percent_of_equity();
    test_qty_percent_of_equity_includes_open_profit_for_pyramid_add();

    // Price path fill priority
    test_price_path_bullish_stop_first();
    test_price_path_bearish_limit_first();
    test_price_path_short_stop_first();
    test_price_path_vs_open_proximity();
    test_price_path_bullish_open_near_high_hits_limit_first();
    test_price_path_open_near_low_hits_stop_first();
    test_opposite_stop_entries_follow_path_order();
    test_opposite_stop_entries_use_open_proximity_path_priority();
    test_strategy_entry_oca_cancel_group();
    test_strategy_entry_qty_type_cash_overrides_default_sizing();
    test_strategy_close_respects_entry_id();
    test_market_close_fills_before_same_bar_opposite_stop_entry();
    test_strategy_close_non_matching_does_not_persist();
    test_stale_exit_does_not_carry_to_future_position();
    test_oca_exit_orders_follow_path_priority();
    test_oca_exit_orders_use_open_proximity_path_priority();
    test_noop_entry_does_not_block_later_opposite_stop();
    test_partial_exit_ignored_when_full_exit_present();
    test_partial_exit_reservation_limits_full_exit();
    test_priced_exit_not_filled_same_bar_when_pooc_false();
    test_strategy_close_cancels_prior_pending_entries_but_keeps_same_pass_reversal();
    test_strategy_close_any_non_matching_keeps_pending_entry_live();
    test_strategy_close_pooc_missing_id_noops();
    test_strategy_close_pooc_cancels_same_bar_market_reentry();
    test_strategy_close_pooc_keeps_same_bar_market_reversal();
    test_strategy_close_immediate_cancels_prior_same_bar_market_reentry();
    test_strategy_close_pooc_keeps_same_bar_pending_entry();
    test_strategy_close_pooc_partial_close_keeps_other_exit_bracket();
    test_strategy_close_fifo_only_closes_requested_leg_size();
    test_strategy_close_pooc_fifo_only_closes_requested_leg_size();
    test_strategy_close_reused_id_closes_one_logical_slot();
    test_strategy_close_pooc_sets_exit_comment();
    test_strategy_close_qty_percent_reduces_position();
    test_strategy_close_qty_reduces_position();
    test_strategy_close_immediately_fills_current_close();
    test_stale_close_all_does_not_close_future_reentry();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
