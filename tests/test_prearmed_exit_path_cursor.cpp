/*
 * A resting strategy.exit bracket becomes eligible only once its priced
 * from_entry parent fills. On that entry bar it may consume the remaining
 * synthetic OHLC path, never a stop touch that preceded the parent fill.
 *
 * The four cells mirror the TradingView-pinned clean-room probe
 * order-pooc-resting-bracket-path-01: A/C have a pre-entry-only stop touch and
 * must exit next bar; B/D touch the stop after entry and must exit same bar.
 */

#include <cmath>
#include <cstdio>
#include <limits>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Cell { LongPre, LongPost, ShortPre, ShortPost };

class RestingBracketProbe final : public BacktestEngine {
public:
    explicit RestingBracketProbe(Cell cell) : cell_(cell) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        const bool is_long = cell_ == Cell::LongPre || cell_ == Cell::LongPost;
        const double entry = is_long ? 110.0 : 90.0;
        const double stop = is_long ? 90.0 : 110.0;
        strategy_entry("E", is_long, kNaN, entry, 1.0, "priced parent");
        strategy_exit("X", "E", kNaN, stop, kNaN, kNaN, kNaN,
                      100.0, "resting child");
    }

private:
    Cell cell_;
};

static Bar bar(double o, double h, double l, double c, int64_t ts) {
    return {o, h, l, c, 1'000.0, ts};
}

static void check_cell(Cell cell, bool is_long, bool pre_entry_touch) {
    RestingBracketProbe probe(cell);
    Bar bars[3] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        // LongPre:  O->L->H->C, SL 90 before entry 110.
        // LongPost: O->H->L->C, entry 110 before SL 90.
        // ShortPre: O->H->L->C, SL 110 before entry 90.
        // ShortPost:O->L->H->C, entry 90 before SL 110.
        cell == Cell::LongPre
            ? bar(100.0, 120.0, 80.0, 105.0, 1'800'000)
            : cell == Cell::LongPost
                ? bar(100.0, 115.0, 80.0, 105.0, 1'800'000)
                : cell == Cell::ShortPre
                    ? bar(100.0, 115.0, 80.0, 95.0, 1'800'000)
                    : bar(100.0, 120.0, 85.0, 95.0, 1'800'000),
        is_long
            ? bar(105.0, 108.0, 85.0, 95.0, 2'700'000)
            : bar(95.0, 115.0, 90.0, 100.0, 2'700'000),
    };

    probe.run(bars, 3);

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& trade = probe.get_trade(0);
    CHECK(trade.is_long == is_long);
    CHECK(near(trade.entry_price, is_long ? 110.0 : 90.0));
    CHECK(near(trade.exit_price, is_long ? 90.0 : 110.0));
    CHECK(trade.entry_bar_index == 1);
    CHECK(trade.exit_bar_index == (pre_entry_touch ? 2 : 1));
}

int main() {
    std::printf("pre-armed from_entry bracket path cursor\n");
    check_cell(Cell::LongPre, true, true);
    check_cell(Cell::LongPost, true, false);
    check_cell(Cell::ShortPre, false, true);
    check_cell(Cell::ShortPost, false, false);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
