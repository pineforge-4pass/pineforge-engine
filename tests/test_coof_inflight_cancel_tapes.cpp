/*
 * test_coof_inflight_cancel_tapes.cpp -- R5 lane TAIL-D.
 *
 * Under calc_on_order_fills a fill inside a leg of the bar's path starts a
 * recalculation there, and TradingView applies what that recalculation
 * cancels or closes -- strategy.cancel, strategy.cancel_all, a full
 * strategy.close -- at the END of that leg: an exit resting from before whose
 * level the rest of the leg reaches still fills there, at its level, and one
 * the leg does not reach is gone after it. The adapter withdrew the exit at
 * once, and since the recalculation's close waits for the leg's end (lane
 * TAIL-D, tests/test_coof_immediate_close_tapes.cpp) that close took the
 * position at the extreme instead of the exit at its level
 * (job-2898-repost64-orb-meeeeeks-quote-ccy on BINANCE:BTCUSDT 15: TP1's
 * recalculation calls cancel_all() and close(immediately = true), and
 * TradingView fills TP2 on the same leg).
 *
 * The lane's own synthetic scripts (tests/fixtures/coof_inflight_cancel, one
 * `lab tv --no-note` export each, BINANCE:ETHUSDT.P 15, 2025-04-01 ..
 * 2025-04-05, process_orders_on_close on; README.md names every tape): a
 * position of 2 (long on 8-hour slots, short between them) carries two
 * take-profits of one unit, TP1 3 points and TP2 6 points from the average;
 * the recalculation TP1's fill starts calls --
 *   -a  strategy.cancel_all() and strategy.close("P", immediately = true);
 *   -b  strategy.cancel_all();
 *   -c  strategy.cancel("TP2");
 *   -d  strategy.close("P", immediately = true) (no cancel).
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every trade the tape closes inside
 * those bars to be the engine's: entry and exit time, side, price in ticks
 * and quantity.
 *
 * Fail-before (lane report): -a and -d close the rest at TP1's fill price in
 * four cells, one of them (2025-04-04 17:30) where TradingView fills TP2;
 * -b and -c withdraw TP2 in the two cells whose TP2 the rest of TP1's leg
 * reaches (2025-04-01 12:30, 2025-04-04 17:30) and hold to the time exit.
 * With only the lane's leg-end close, -a and -d took those two cells' rest
 * at the leg's end instead of TP2.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

#ifndef PINEFORGE_TAIL_D_INFLIGHT_CANCEL_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_INFLIGHT_CANCEL_FIXTURE_DIR must name tests/fixtures/coof_inflight_cancel"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_inflight_cancel/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;

// One trade as both sides report it: (entry ms, long, entry ticks, quantity,
// exit ms, exit ticks).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

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

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_INFLIGHT_CANCEL_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
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
        Row& row = by_number[std::stoi(cell[0])];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(price);
            std::get<3>(row) = std::llround(std::stod(cell[5]));
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (std::get<4>(row) > 0 && std::get<4>(row) < end_ms) out.push_back(row);
    return out;
}

enum class Action { CancelAllAndClose, CancelAll, CancelTp2, Close };

struct Variant {
    const char* tape;
    Action action;
    std::size_t closed;  // tape trades closed inside the bars
};

// The probes' bodies (tests/fixtures/coof_inflight_cancel/<slug>/strategy.pine).
class InflightCancelHost final : public source::PineStrategyHost {
public:
    explicit InflightCancelHost(const Variant& v) : v_(v) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.process_orders_on_close = true;
        c.initial_capital = 10000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 0.0001);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const int k = bar_index_;
        const int n = trade_count();  // strategy.closedtrades
        const bool exit_here = n > 0 && closed_trade_exit_bar_index(n - 1) == k;
        const bool exit_was_tp1 = n > 0 && closed_trade_exit_id(n - 1) == "TP1";
        const double units = signed_position_size();
        const std::int64_t minute = current_bar_.timestamp / 60'000;
        // minute(time) == 0 and hour(time, "UTC") % 4 == 0, long when hour % 8 == 0.
        if (units == 0.0 && !exit_here && minute % 240 == 0)
            strategy_entry("P", minute % 480 == 0, kNaN, kNaN, 2.0);
        if (units != 0.0) {
            const double avg = position_avg_price();
            const double side = units > 0.0 ? 1.0 : -1.0;
            if (std::fabs(units) == 2.0) {
                strategy_exit("TP1", "P", avg + side * 3.0, kNaN, kNaN, kNaN, kNaN, 100.0, {}, 1.0);
                strategy_exit("TP2", "P", avg + side * 6.0, kNaN, kNaN, kNaN, kNaN, 100.0, {}, 1.0);
            }
            if (exit_here && exit_was_tp1) {
                switch (v_.action) {
                case Action::CancelAllAndClose:
                    strategy_cancel_all();
                    strategy_close("P", "IMM", kNaN, kNaN, true);
                    break;
                case Action::CancelAll: strategy_cancel_all(); break;
                case Action::CancelTp2: strategy_cancel("TP2"); break;
                case Action::Close: strategy_close("P", "IMM", kNaN, kNaN, true); break;
                }
            }
            // strategy.opentrades.entry_bar_index(0) is na until the open trade is
            // booked, which a fill's recalculation can precede; na compares false.
            const int entry_bar = open_trade_entry_bar_index(0);
            if (!is_na(entry_bar) && k - entry_bar >= 12)
                strategy_close("", "TIME", kNaN, kNaN, false);
        }
    }

private:
    const Variant& v_;
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

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-6s %lld %s @%lld q=%lld -> %lld @%lld\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

void replay(const Variant& v) {
    std::printf("-- %s\n", v.tape);
    static const std::vector<Bar> bars = feed();
    const std::int64_t end_ms = bars.back().timestamp;
    const std::vector<Row> tape = tape_trades(v.tape, end_ms);
    CHECK(tape.size() == v.closed);
    InflightCancelHost host(v);
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), std::llround(t.qty),
                            t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
}

}  // namespace

int main() {
    const Variant variants[] = {
        {"td-m7a", Action::CancelAllAndClose, 45},
        {"td-m7b", Action::CancelAll, 45},
        {"td-m7c", Action::CancelTp2, 45},
        {"td-m7d", Action::Close, 45},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
