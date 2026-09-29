/*
 * test_coof_immediate_close_tapes.cpp -- R5 lane TAIL-D.
 *
 * Under calc_on_order_fills a fill inside a leg of the bar's path starts a
 * recalculation there, and TradingView fills a close that recalculation
 * places at the END of that leg (the next extreme), at its print: the
 * adapter already did for strategy.close_all() and strategy.close(id)
 * (R4 slice C, ab9714be pine_scheduler.cpp:541-563). With `immediately =
 * true` the adapter executed the close at the recalculating fill's own
 * price instead; TradingView books the same trades with and without it (the
 * -a, -c and -d tapes are one trade list). On NYSE:F the next extreme can
 * sit between two cents (H 9.445): the close_all rested its trigger at the
 * booked tick (9.45), which the path never reaches, and rolled to the bar's
 * close; the close of one id already rested at the raw extreme (-h, -i).
 *
 * The lane's own synthetic scripts (tests/fixtures/coof_immediate_close, one
 * `lab tv --no-note` export each; README.md names every tape): a position of
 * 2 (long on 8-hour slots, short between them on ETH; alternating on every
 * NYSE:F bar) carries a take-profit TP of one unit; the recalculation TP's
 * fill starts closes the rest --
 *   -a  strategy.close_all(immediately = true), process_orders_on_close on;
 *   -b  the same with process_orders_on_close off;
 *   -c  strategy.close_all() (control: the rule the adapter already had);
 *   -d  strategy.close("P", immediately = true);
 *   -h  -a on NYSE:F 15 (a one-cent tick, half-cent extremes);
 *   -i  -d on NYSE:F 15 (control).
 * Each row replays TradingView's own tape through the Pine adapter over the
 * tape's bars embedded in bars_eth.inc / bars_ford.inc and requires every
 * trade the tape closes inside those bars to be the engine's: entry and exit
 * time, side, price in ticks and quantity.
 *
 * Fail-before (lane report): -a, -b and -d book the rest at TP's fill price
 * (8, 22 and 8 rows); -h books 5 rows at the bar's close or TP's price.
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

#ifndef PINEFORGE_TAIL_D_IMMEDIATE_CLOSE_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_IMMEDIATE_CLOSE_FIXTURE_DIR must name tests/fixtures/coof_immediate_close"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_immediate_close/bars_eth.inc"
#include "fixtures/coof_immediate_close/bars_ford.inc"

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
    std::ifstream in(std::string(PINEFORGE_TAIL_D_IMMEDIATE_CLOSE_FIXTURE_DIR) + "/" + tape
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

enum class Close { AllImmediate, All, IdImmediate };

struct Variant {
    const char* tape;
    bool ford;        // NYSE:F 15 (else BINANCE:ETHUSDT.P 15)
    bool pooc;
    Close close;
    std::size_t closed;  // tape trades closed inside the bars
};

// The probes' bodies (tests/fixtures/coof_immediate_close/<slug>/strategy.pine).
class ImmediateCloseHost final : public source::PineStrategyHost {
public:
    explicit ImmediateCloseHost(const Variant& v) : v_(v) {
        attach_pine_execution_adapter();
        if (v.ford) {
            set_syminfo_session("0930-1600");
            set_syminfo_timezone("America/New_York");
        }
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.process_orders_on_close = v.pooc;
        c.initial_capital = v.ford ? 1000000.0 : 10000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 2.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", v.ford ? 1.0 : 0.0001);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const int k = bar_index_;
        const int n = trade_count();  // strategy.closedtrades
        const bool exit_here = n > 0 && closed_trade_exit_bar_index(n - 1) == k;
        const bool exit_was_tp = n > 0 && closed_trade_exit_id(n - 1) == "TP";
        const double units = signed_position_size();
        const std::int64_t minute = current_bar_.timestamp / 60'000;
        if (units == 0.0 && !exit_here) {
            // ETH: minute(time) == 0 and hour(time, "UTC") % 4 == 0, long when
            // hour % 8 == 0; NYSE:F: every bar, long on even bar indexes.
            if (v_.ford) strategy_entry("P", k % 2 == 0, kNaN, kNaN, 2.0);
            else if (minute % 240 == 0) strategy_entry("P", minute % 480 == 0, kNaN, kNaN, 2.0);
        }
        if (units != 0.0) {
            const double avg = position_avg_price();
            const double offset = v_.ford ? 0.02 : 3.0;
            if (std::fabs(units) == 2.0)
                strategy_exit("TP", "P", units > 0.0 ? avg + offset : avg - offset, kNaN, kNaN,
                              kNaN, kNaN, 100.0, {}, 1.0);
            if (exit_here && exit_was_tp) {
                switch (v_.close) {
                case Close::AllImmediate: strategy_close("", "IMM", kNaN, kNaN, true); break;
                case Close::All: strategy_close("", "IMM", kNaN, kNaN, false); break;
                case Close::IdImmediate: strategy_close("P", "IMM", kNaN, kNaN, true); break;
                }
            }
            if (k - open_trade_entry_bar_index(0) >= (v_.ford ? 3 : 12))
                strategy_close("", "TIME", kNaN, kNaN, false);
        }
    }

private:
    const Variant& v_;
};

template <std::size_t N>
std::vector<Bar> feed(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& row : rows) {
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
    static const std::vector<Bar> eth = feed(kEth15);
    static const std::vector<Bar> ford = feed(kFord15);
    const std::vector<Bar>& bars = v.ford ? ford : eth;
    const std::int64_t end_ms = bars.back().timestamp;
    const std::vector<Row> tape = tape_trades(v.tape, end_ms);
    CHECK(tape.size() == v.closed);
    ImmediateCloseHost host(v);
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
        {"td-m1a", false, true, Close::AllImmediate, 45},
        {"td-m1b", false, false, Close::AllImmediate, 47},
        {"td-m1c", false, true, Close::All, 45},
        {"td-m1d", false, true, Close::IdImmediate, 45},
        {"td-m1h", true, true, Close::AllImmediate, 118},
        {"td-m1i", true, true, Close::IdImmediate, 118},
    };
    for (const Variant& v : variants) replay(v);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
