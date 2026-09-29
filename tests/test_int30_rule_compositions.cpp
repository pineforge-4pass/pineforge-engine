/*
 * test_int30_rule_compositions.cpp — INT30.
 *
 * Where lane TAIL-B's rules for a bar's adds and lane TAIL-H's rule PK meet,
 * in PineExecutionAdapter::on_applied, read off TradingView's own tapes of
 * synthetic probes written for INT30:
 *
 *   AS  (TAIL-B) each MARKET add a bar places on a held long is judged
 *       against the position held when it was placed plus its own units;
 *   AC  (TAIL-B) the grown book is margin-called once, after the last add:
 *       at that fill for a market add, at the next open, sized at the close,
 *       under process_orders_on_close;
 *   LF  (TAIL-B) after a call that closes several lots of a long the broker
 *       checks the book again after each lot and calls the last check still
 *       short again;
 *   PK  (TAIL-H) once a margin call has shrunk the position, the exits hold
 *       the position that remains in their queue order again: a 50 % exit
 *       keeps the share it reserved, and the stop exit behind it gets what
 *       that leaves (or, created first, the stop exit holds it all).
 *
 * Every call AC or LF takes applies through on_applied as a Margin-family
 * fill, and PK re-reserves the exits of the entry it shrank there, before LF
 * sizes its follow-up. Each row replays one lab tv tape
 * (tests/fixtures/int30_compositions, BINANCE:ETHUSDT.P 15) through the Pine
 * adapter under the configuration the probe's strategy() declares, over the
 * corpus 15m bars of lane W5-ENG-MARGIN-V6 (tests/fixtures/margin_v6/bars.inc),
 * and requires every trade the tape closes inside those bars to be the
 * engine's: entry and exit time, side, price in ticks of 0.01 and quantity in
 * lots of 0.0001.
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

#ifndef PINEFORGE_INT30_COMPOSITIONS_FIXTURE_DIR
#error "PINEFORGE_INT30_COMPOSITIONS_FIXTURE_DIR must name tests/fixtures/int30_compositions"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/margin_v6/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;     // BINANCE:ETHUSDT.P's price tick
constexpr double kLot = 0.0001;    // and TradingView's quantity step there
constexpr std::int64_t kMinute = 60'000;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

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
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct Tape {
    std::vector<Row> trades;   // closed inside the bars
    int margin_calls = 0;      // "Margin call" exits among them
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_INT30_COMPOSITIONS_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    CHECK(in.good());
    std::map<int, Row> by_number;
    std::map<int, bool> closed;
    std::map<int, bool> called;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price, Size (qty), ...
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        const double price = std::stod(cell[4]);
        CHECK(on_grid(price, kTick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, kLot));
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = lots(qty);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
            closed[number] = true;
            called[number] = cell[3] == "Margin call";
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        if (!closed.count(number) || std::get<4>(row) >= end_ms) continue;
        out.trades.push_back(row);
        if (called[number]) ++out.margin_calls;
    }
    std::sort(out.trades.begin(), out.trades.end());
    return out;
}

// The probe int30-c4-grid-pk-*, as its Pine source in the fixture tree reads.
class ProbeHost final : public source::PineStrategyHost {
public:
    explicit ProbeHost(const source::PineStrategyConfig& config) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
        set_syminfo_mintick(kTick);
    }

    // Every 6 hours from 2025-04-02 00:00 UTC, forty cells of 24 bars. On a
    // cell's first bar, flat, E = initial capital + net profit rounded to 100
    // (a tie up, as math.round rounds), so that a cent of an earlier cell's
    // P&L does not move a later cell's sizing. Kind
    // k = cell % 6: n = 1 (k 0, 1), 3 (k 2, 3) or 10 (k 4, 5) lots, one per
    // bar from the cell's first bar: n - 1 seeds S1 .. S(n-1) sharing 0.3 E,
    // then "L" of 0.6 E (0.9 E when n = 1); on the bar after "L" the exits TP
    // (50 %, limit 0.6 % above the close) and SL (stop 0.6 % below) from "L",
    // TP first on even kinds, SL first on odd; two bars later three adds of
    // 0.07 E each on one bar; flattened on the cell's 23rd bar.
    void on_source_bar(const Bar&) override {
        const std::int64_t t0 = days_from_civil(2025, 4, 2) * 24 * 60 * kMinute;
        const std::int64_t step = 360 * kMinute;
        const std::int64_t rel = current_bar_.timestamp - t0;
        if (rel < 0 || rel >= 40 * step) return;
        const std::int64_t bar_in = (rel % step) / (15 * kMinute);
        const int kind = static_cast<int>((rel / step) % 6);
        const int n = kind < 2 ? 1 : kind < 4 ? 3 : 10;
        if (bar_in == 0) equity_ = std::floor(current_equity() / 100.0 + 0.5) * 100.0;
        if (bar_in < n - 1) {
            const std::string id = "S" + std::to_string(bar_in + 1);
            entry(id, q_of(0.3 * equity_ / (n - 1)));
        }
        if (bar_in == n - 1) entry("L", q_of((n == 1 ? 0.9 : 0.6) * equity_));
        if (bar_in == n) {
            const double tp = round_to_tick(current_bar_.close * 1.006);
            const double sl = round_to_tick(current_bar_.close * 0.994);
            if (kind % 2 == 0) {
                strategy_exit("TP", "L", tp, kNaN, kNaN, kNaN, kNaN, 50.0, "TP");
                strategy_exit("SL", "L", kNaN, sl, kNaN, kNaN, kNaN, 100.0, "SL");
            } else {
                strategy_exit("SL", "L", kNaN, sl, kNaN, kNaN, kNaN, 100.0, "SL");
                strategy_exit("TP", "L", tp, kNaN, kNaN, kNaN, kNaN, 50.0, "TP");
            }
        }
        if (bar_in == n + 2) {
            const double q = q_of(0.07 * equity_);
            for (int i = 1; i <= 3; ++i) entry("A" + std::to_string(i), q);
        }
        if (bar_in == 22) strategy_close("", "cleanup", kNaN, kNaN, false);
    }

private:
    // qOf(money): math.floor(money / close * 10000) / 10000
    double q_of(double money) const {
        return std::floor(money / current_bar_.close * 10000.0) / 10000.0;
    }
    // math.round_to_mintick: the nearest tick, a tie rounding up.
    static double round_to_tick(double price) { return std::floor(price / kTick + 0.5) * kTick; }
    void entry(const std::string& id, double qty) {
        strategy_entry(id, true, kNaN, kNaN, qty, id);
    }

    double equity_ = kNaN;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const source::PineStrategyConfig& config, const std::vector<Bar>& bars,
        std::int64_t end_ms) {
    ProbeHost host(config);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
    }
    std::sort(out.trades.begin(), out.trades.end());
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What a probe's strategy() declares: pyramiding 200, 10000 of capital,
// margins of 100, a percent commission and a slippage in ticks.
source::PineStrategyConfig config(bool pooc, double commission, int slippage) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = 10000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 200;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission;
    c.slippage = slippage;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

struct Case {
    const char* tape;
    source::PineStrategyConfig strategy;  // what its strategy() declares
    std::size_t closed;                   // tape trades closed inside the bars
    int margin_calls;                     // "Margin call" exits among them
    // Rows known to differ, each (the tape's, the engine's): a recorded
    // divergence the rules composed here do not decide (see main()).
    std::vector<std::pair<Row, Row>> recorded;
};

}  // namespace

int main() {
    const std::vector<Bar> bars = feed();
    // Both tapes are the engine's trade for trade. At margins of 100 a book's
    // shortfall is its cost plus its fees less the equity, whatever the mark,
    // and TradingView calls four times its lot floor over the print the adds'
    // fill slipped from: cell 37, 2025-04-11 07:00 UTC, "L" 4.4515 alone with
    // SL created first, adds of 0.3473 at 1551.95 slipped from 1551.93, where
    // 830.4416 over 1551.93 is 0.535102 lots and TradingView calls 2.1404 of
    // "L" (over the 1551.95 fill it would be 0.535096, a call of 2.1400), and
    // SL, which holds what the call leaves, exits 2.3111 (lane TAIL-I; INT30
    // recorded the fill-sized call here). Every exit PK re-reserves after a
    // call, in this cell and in the other 45 called cells of the two tapes,
    // is TradingView's for that call.
    const Case cases[] = {
        {"int30-c4-grid-pk-mkt", config(false, 0.1, 2), 351, 111, {}},
        {"int30-c4-grid-pk-pooc", config(true, 0.06, 3), 351, 111, {}},
    };

    for (const Case& c : cases) {
        std::printf("-- %s\n", c.tape);
        const std::int64_t end = bars.back().timestamp;
        const Tape tape = tape_trades(c.tape, end);
        CHECK(tape.trades.size() == c.closed);
        CHECK(tape.margin_calls == c.margin_calls);
        const Run engine = run(c.strategy, bars, end);
        CHECK(engine.error.empty());
        // The recorded rows: each is on the tape, and the engine books its
        // own value there; every other row is the tape's.
        std::vector<Row> expected = tape.trades;
        for (const auto& [tv, own] : c.recorded) {
            const auto found = std::find(expected.begin(), expected.end(), tv);
            CHECK(found != expected.end());
            if (found != expected.end()) *found = own;
            std::printf("  recorded: TradingView q=%lld lots, engine q=%lld lots (entry %lld)\n",
                        std::get<3>(tv), std::get<3>(own),
                        static_cast<long long>(std::get<0>(tv)));
        }
        std::sort(expected.begin(), expected.end());
        CHECK(engine.trades == expected);
        if (engine.trades != expected) {
            std::size_t first = 0;
            while (first < engine.trades.size() && first < expected.size()
                   && engine.trades[first] == expected[first]) {
                ++first;
            }
            std::printf("  engine %zu trades, tape %zu; first difference at sorted row %zu\n",
                        engine.trades.size(), expected.size(), first);
            const auto from = [&](const std::vector<Row>& rows) {
                const std::size_t lo = first > 3 ? first - 3 : 0;
                return std::vector<Row>(rows.begin() + std::min(lo, rows.size()),
                                        rows.begin() + std::min(first + 6, rows.size()));
            };
            show("tape", from(expected));
            show("engine", from(engine.trades));
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
