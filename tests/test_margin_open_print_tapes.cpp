/*
 * test_margin_open_print_tapes.cpp -- R5 lane TAIL-I.
 *
 * TradingView's margin calls on an all-in, commissioned and slipped
 * explicit-quantity MARKET short, read off its own tapes of one synthetic
 * script (tests/fixtures/margin_open_print, `lab tv --no-note` exports):
 * every 8 bars from flat one short of lot-floor(strategy.equity / close) is
 * placed, filled at the next open less 2 ticks, and flattened by
 * strategy.close_all() 4 bars later; 10000 of capital, margin 100 both ways,
 * slippage 2, commission 0.05 %.
 *
 *   O   the entry bar is checked from its opening print: the book, its entry
 *       fee paid, marked at the open's tick is called four times its
 *       lot-floored shortfall there, filled at that tick plus the slippage,
 *       before the rest of the bar's path -- as a slipped market short with
 *       no commission already was; the adapter deferred a commissioned
 *       explicit-quantity short to a post-script trim at its fill and one
 *       slice at the bar's high;
 *   P   every later point of the path is checked on the book the calls
 *       before it left, the points after the bar's adverse extreme too;
 *   F   under a percent commission a call of any size is followed: the book
 *       it called, re-marked at its fill, still short by more than the
 *       called units' margin and two slippage steps each gives up four
 *       times its lot-floored shortfall over the fill (one unit where that
 *       floors to none) at the next path point inside that fill, on a
 *       fractional lot grid too.
 *
 * Each row replays one tape through the Pine adapter over the lane chart
 * feed of its window -- NYSE:F 15 2025-04-01 .. 2025-06-30
 * (../slipped_short/bars.inc), 2025-07-01 .. 2025-09-30 (f15_q3_bars.inc),
 * OANDA:EURUSD 15 2025-04-01 .. 2025-04-29 (../margin_residual/eur15_bars.inc)
 * -- from the first bar TradingView computed, with TradingView's lot as the
 * `qty_step`, and requires every trade the tape closes before the row's end
 * to be the engine's: entry and exit time, side, price in ticks and quantity
 * in lots. The NYSE:F Q2 row ends at 2025-04-07 15:30 UTC, whose short
 * TradingView calls at the bar's high alone, 180 at 9.33, not at the 9.105
 * open 1.83 units short there (the lane report's open item); the EURUSD row
 * ends at 2025-04-28 14:00 UTC, where TradingView takes no call at a high
 * whose shortfall, 0.043, is under one lot of margin by the equity's last
 * cents.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

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

#ifndef PINEFORGE_MARGIN_OPEN_PRINT_FIXTURE_DIR
#error "PINEFORGE_MARGIN_OPEN_PRINT_FIXTURE_DIR must name tests/fixtures/margin_open_print"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/slipped_short/bars.inc"
#include "fixtures/margin_open_print/f15_q3_bars.inc"
#include "fixtures/margin_residual/eur15_bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// (entry ms, long?, entry price in ticks, quantity in lots, exit ms, exit price in ticks)
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Case {
    const char* tape;
    const FeedBar* bars;
    std::size_t bar_count;
    double tick;           // the symbol's price tick
    double lot;            // TradingView's quantity step there
    std::int64_t end_ms;   // compare the trades closed before it
    std::size_t closed;    // tape trades closed before it
    int margin_calls;      // "Margin call" exits among them
};

struct Tape {
    std::vector<Row> trades;
    int margin_calls = 0;
};

Tape tape_trades(const Case& c) {
    std::ifstream in(std::string(PINEFORGE_MARGIN_OPEN_PRINT_FIXTURE_DIR) + "/" + c.tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, bool> called;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = std::llround(std::stod(cell[4]) / c.tick);
            std::get<3>(row) = std::llround(std::stod(cell[5]) / c.lot);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = std::llround(std::stod(cell[4]) / c.tick);
            called[number] = cell[3] == "Margin call";
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        if (std::get<4>(row) > 0 && std::get<4>(row) < c.end_ms) {
            out.trades.push_back(row);
            out.margin_calls += called[number] ? 1 : 0;
        }
    }
    std::sort(out.trades.begin(), out.trades.end());
    return out;
}

// The tapes' body (tests/fixtures/margin_open_print/<slug>/strategy.pine):
//   var int k = 0; k += 1
//   if strategy.position_size == 0 and k % 8 == 0
//       strategy.entry("S", strategy.short, qty = math.floor(strategy.equity / close / lot) * lot)
//   if strategy.position_size != 0 and k % 8 == 5
//       strategy.close_all(comment = "X")
class OpenPrintHost final : public source::PineStrategyHost {
public:
    explicit OpenPrintHost(const Case& c) : lot_(c.lot) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 2;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.05;
        config.pyramiding = 1;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", c.lot);
        syminfo_mintick_ = c.tick;
    }

    void on_source_bar(const Bar&) override {
        ++k_;
        if (signed_position_size() == 0.0 && k_ % 8 == 0) {
            const double equity = current_equity() + open_profit(current_bar_.close);
            const double q = std::floor(equity / current_bar_.close / lot_) * lot_;
            strategy_entry("S", false, kNaN, kNaN, q);
        }
        if (signed_position_size() != 0.0 && k_ % 8 == 5)
            strategy_close("", "X", kNaN, kNaN, false);
    }

private:
    double lot_;
    long long k_ = 0;
};

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

}  // namespace

int main() {
    const Case cases[] = {
        // NYSE:F 15, 2025-04-01 .. : the trades closed before 2025-04-07 15:30 UTC.
        {"taili-mop-f", kF15Q2, sizeof(kF15Q2) / sizeof(kF15Q2[0]), 0.01, 1.0,
         1744039800000LL, 36, 25},
        // NYSE:F 15, 2025-07-01 .. 2025-10-01: every trade closed before the last bar.
        {"taili-mop-f3", kF15Q3, sizeof(kF15Q3) / sizeof(kF15Q3[0]), 0.01, 1.0,
         1759261500000LL, 445, 278},
        // OANDA:EURUSD 15, 2025-04-01 .. : the trades closed before 2025-04-28 14:00 UTC.
        {"taili-mop-eur", kEurUsd15, sizeof(kEurUsd15) / sizeof(kEurUsd15[0]), 0.00001, 0.01,
         1745848800000LL, 625, 408},
    };
    for (const Case& c : cases) {
        std::printf("-- %s\n", c.tape);
        const Tape tape = tape_trades(c);
        CHECK(tape.trades.size() == c.closed);
        CHECK(tape.margin_calls == c.margin_calls);
        std::printf("  tape: %zu trades closed in the window, %d margin calls\n",
                    tape.trades.size(), tape.margin_calls);
        std::vector<Bar> bars;
        for (std::size_t i = 0; i < c.bar_count; ++i) {
            Bar b{};
            b.timestamp = c.bars[i].ts;
            b.open = c.bars[i].open;
            b.high = c.bars[i].high;
            b.low = c.bars[i].low;
            b.close = c.bars[i].close;
            b.volume = c.bars[i].volume;
            bars.push_back(b);
        }
        OpenPrintHost host(c);
        host.set_trade_start_time(bars.front().timestamp);
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
        CHECK(host.last_error().empty());
        std::vector<Row> engine;
        for (int i = 0; i < host.trade_count(); ++i) {
            const Trade& t = host.get_trade(i);
            if (t.exit_time >= c.end_ms) continue;
            engine.emplace_back(t.entry_time, t.is_long, std::llround(t.entry_price / c.tick),
                                std::llround(t.qty / c.lot), t.exit_time,
                                std::llround(t.exit_price / c.tick));
        }
        std::sort(engine.begin(), engine.end());
        CHECK(engine == tape.trades);
        if (engine != tape.trades) {
            std::size_t first = 0;
            while (first < engine.size() && first < tape.trades.size()
                   && engine[first] == tape.trades[first]) {
                ++first;
            }
            std::printf("  engine %zu trades, tape %zu; first difference at sorted row %zu\n",
                        engine.size(), tape.trades.size(), first);
            const auto from = [&](const std::vector<Row>& rows) {
                const std::size_t lo = first > 3 ? first - 3 : 0;
                return std::vector<Row>(rows.begin() + std::min(lo, rows.size()),
                                        rows.begin() + std::min(first + 6, rows.size()));
            };
            show("tape", from(tape.trades));
            show("engine", from(engine));
        }
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
