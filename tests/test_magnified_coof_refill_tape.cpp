/*
 * test_magnified_coof_refill_tape.cpp -- R5 lane TAIL-C.
 *
 * With the bar magnifier on, the recalculation of a calc_on_order_fills exit
 * fill places a priced entry R. TradingView makes R live at the next fill
 * point of its own intrabar path (the next intrabar's open after an
 * intrabar's second extreme, the extreme ending the leg the exit filled on
 * otherwise): executable there, R fills at that point's print; a limit that
 * is not rests at its level from that point on. After a fill on the chart
 * bar's last path point -- its close print -- the next fill point is the next
 * bar's opening, and R is live from that open. The engine applied the chart
 * path's leg-end rule (lane W8D-NOTRADES) only off the magnified path and
 * filled R at once at its own level on the exit's leg (the bystry1991
 * EMA200 regime BOS/CHoCH FVG strategy on CME_MINI:ES1! 15, which declares
 * the magnifier).
 *
 * The lane's own synthetic script (tests/fixtures/magnified_coof_refill/
 * tailc-mag-coof-refill-leg, one `lab tv --no-note` export, BINANCE:ETHUSDT.P
 * 15 magnified, 2025-04-01 .. 2025-05-01), stateless: P, a one-unit short on
 * every even UTC hour's first bar; X, its buy stop at the average + 3, P
 * closed after 8 bars; R, placed by the recalculation of X's fill and by that
 * bar's close while still flat -- cell A (X on an hour % 4 < 2): a SELL limit
 * at X's fill - 3, executable at X's fill and at the leg's end; cell B: a BUY
 * limit at X's fill + 0.20, executable at X's fill, usually not at the leg's
 * end; R closed after 2 bars, cancelled on the next bar when unfilled. The
 * row replays the tape's 2025-04-05 through the Pine adapter on the lane's
 * 1-minute feed with the magnifier on (bars_1m.inc) and requires every trade
 * the tape opens and closes inside that day to be the engine's, on its chart
 * bars: entry and exit bar, side, price and quantity.
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

#ifndef PINEFORGE_MAGNIFIED_REFILL_FIXTURE_DIR
#error "PINEFORGE_MAGNIFIED_REFILL_FIXTURE_DIR must name tests/fixtures/magnified_coof_refill"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/magnified_coof_refill/bars_1m.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// BINANCE:ETHUSDT.P's quantity step and price tick (lane eth-scraped-15).
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
constexpr std::int64_t kChartMs = 15 * 60'000;

// (entry chart bar ms, long, entry price, quantity, exit chart bar ms, exit
// price), prices in ticks and quantity in lots.
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
// The 24x7 UTC chart's 15-minute bar holding `ms`.
std::int64_t chart_bar(std::int64_t ms) { return ms - ms % kChartMs; }

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

// The tape's trades opened at or after `begin_ms` and closed before `end_ms`.
std::vector<Row> tape_trades(std::int64_t begin_ms, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_MAGNIFIED_REFILL_FIXTURE_DIR)
                     + "/tailc-mag-coof-refill-leg/tv_trades.csv");
    std::map<int, Row> by_number;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        Row& row = by_number[std::stoi(cell[0])];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = lots(std::stod(cell[5]));
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number)
        if (std::get<0>(row) >= begin_ms && std::get<4>(row) > 0 && std::get<4>(row) < end_ms)
            out.push_back(row);
    return out;
}

// The probe's body (tests/fixtures/magnified_coof_refill/
// tailc-mag-coof-refill-leg/strategy.pine).
class MagnifiedRefillHost final : public source::PineStrategyHost {
public:
    MagnifiedRefillHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.initial_capital = 10000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_metadata("qty_step", kLot);
        set_syminfo_mintick(kTick);
    }

    void on_source_bar(const Bar&) override {
        const int k = pine_bar_index();
        const int closed = trade_count();  // strategy.closedtrades
        const bool exit_here = closed > 0 && closed_trade_exit_bar_index(closed - 1) == k;
        const bool exit_was_x = closed > 0 && closed_trade_exit_id(closed - 1) == "X";
        const bool flat = signed_position_size() == 0.0;
        const std::string open_id = flat ? std::string() : open_trade_entry_id(0);
        const int held = flat ? 0 : k - open_trade_entry_bar_index(0);
        const std::int64_t x_time = closed > 0 ? closed_trade_exit_time(closed - 1)
                                               : current_bar_.timestamp;
        const bool cell_a = (x_time / 3'600'000) % 24 % 4 < 2;  // hour(nz(xBar, time), "UTC")
        const std::int64_t minute_of_day = (current_bar_.timestamp / 60'000) % 1440;
        if (flat && !exit_here && minute_of_day % 120 == 0) strategy_entry("P", false);
        if (open_id == "P") {
            strategy_exit("X", "P", kNaN, position_avg_price() + 3.0);
            if (held >= 8) strategy_close("P");
        }
        if (flat && exit_here && exit_was_x) {
            const double x_price = closed_trade_exit_price(closed - 1);
            if (cell_a) strategy_entry("RA", false, x_price - 3.0);
            else strategy_entry("RB", true, x_price + 0.2);
        }
        if (flat && !exit_here) {
            strategy_cancel("RA");
            strategy_cancel("RB");
        }
        if ((open_id == "RA" || open_id == "RB") && held >= 2)
            strategy_close("", "R done", kNaN, kNaN, false);
    }
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
    const std::int64_t begin_ms = kEth1m[0].ts;
    // The last chart bar: a position still open there is closed by the range
    // end and is not a tape trade.
    const std::int64_t end_ms = chart_bar(kEth1m[sizeof(kEth1m) / sizeof(kEth1m[0]) - 1].ts);
    const std::vector<Row> tape = tape_trades(begin_ms, end_ms);
    CHECK(tape.size() == 15u);

    std::vector<Bar> bars;
    for (const FeedBar& row : kEth1m) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    MagnifiedRefillHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    CHECK(host.last_error().empty());
    std::vector<Row> lane;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (chart_bar(t.exit_time) >= end_ms) continue;
        lane.emplace_back(chart_bar(t.entry_time), t.is_long, ticks(t.entry_price), lots(t.qty),
                          chart_bar(t.exit_time), ticks(t.exit_price));
    }
    CHECK(lane == tape);
    if (lane != tape) {
        show("tape", tape);
        show("engine", lane);
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
