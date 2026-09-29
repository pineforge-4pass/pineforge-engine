/*
 * test_coof_open_newborn_exit_tape.cpp -- R5 lane TAIL-C.
 *
 * Under calc_on_order_fills the recalculation of a bar's FIRST open fill may
 * place a market entry that the same open executes (a newborn, here reversing
 * that first fill). The recalculation of that SECOND fill places an exit stop
 * already through at the open. TradingView makes that stop live from the
 * bar's first extreme, not from the open: filled at the extreme's print when
 * it is still through there, at the open when the extreme is the open itself,
 * otherwise at its own level from the extreme on (tests/fixtures/
 * coof_open_newborn_exit, cells A, B and C). After a single open fill the
 * through stop fills at that open (cell D; lane W4's LA/SA). The engine held
 * such a stop for the next bar (job-1856 DPO/MACD/DI/SMA on EURUSD and
 * ETHUSDT.P 1D).
 *
 * The lane's own synthetic script, one `lab tv --no-note` export
 * (BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-15), stateless per cell: an
 * 8-hour UTC cycle, the first entry placed at the hh:30 close and filled at
 * the hh:45 open;
 *   A (h % 8 == 0): S at the open; its recalculation places L, which reverses
 *                   it at the open; exit LX(stop = average + 5);
 *   B (h % 8 == 2): as A with LX(stop = average + 5, qty = 1);
 *   C (h % 8 == 4): the mirror, L then S; exit SX(stop = average - 5);
 *   D (h % 8 == 6): L alone at the open; exit LX(stop = average + 5);
 * close_all at the next non-cell hour's first bar. The row replays the tape
 * through the Pine adapter over the corpus 15m chart feed's first four days
 * (bars.inc) and requires every trade the tape closes inside those bars to be
 * the engine's: entry and exit time, side, price and quantity.
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

#ifndef PINEFORGE_COOF_OPEN_NEWBORN_FIXTURE_DIR
#error "PINEFORGE_COOF_OPEN_NEWBORN_FIXTURE_DIR must name tests/fixtures/coof_open_newborn_exit"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_open_newborn_exit/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// BINANCE:ETHUSDT.P's quantity step and price tick (lane eth-scraped-15).
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;

// (entry ms, long, entry price, quantity, exit ms, exit price), prices in
// ticks and quantity in lots.
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }

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

std::vector<Row> tape_trades(std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_COOF_OPEN_NEWBORN_FIXTURE_DIR)
                     + "/tailc-1856-open-newborn/tv_trades.csv");
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
        if (std::get<4>(row) > 0 && std::get<4>(row) < end_ms) out.push_back(row);
    return out;
}

// The probe's body (tests/fixtures/coof_open_newborn_exit/
// tailc-1856-open-newborn/strategy.pine).
class OpenNewbornHost final : public source::PineStrategyHost {
public:
    OpenNewbornHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t minute_of_day = (current_bar_.timestamp / 60'000) % 1440;
        const int h = static_cast<int>(minute_of_day / 60);
        const int m = static_cast<int>(minute_of_day % 60);
        const int c = h % 8;
        const bool cell = c == 0 || c == 2 || c == 4 || c == 6;
        const double size = signed_position_size();
        const bool held = size != 0.0;
        const bool opened_here = held && open_trade_entry_bar_index(0) == bar_index_;
        if (cell && m == 30) {
            if (c == 0 || c == 2) strategy_entry("S", false);
            else strategy_entry("L", true);
        }
        if (cell && m == 45 && opened_here) {
            if ((c == 0 || c == 2) && size < 0.0) strategy_entry("L", true);
            if (c == 4 && size > 0.0) strategy_entry("S", false);
        }
        // The script reads strategy.position_size afresh for each statement.
        if (signed_position_size() > 0.0 && open_trade_entry_id(0) == "L") {
            const double stop = position_avg_price() + 5.0;
            if (c == 2) {
                strategy_exit("LX", "L", kNaN, stop, kNaN, kNaN, kNaN, 100.0, {}, 1.0);
            } else if (c != 4) {
                strategy_exit("LX", "L", kNaN, stop);
            }
        }
        if (signed_position_size() < 0.0 && open_trade_entry_id(0) == "S" && c == 4)
            strategy_exit("SX", "S", kNaN, position_avg_price() - 5.0);
        if (!cell && m == 0 && signed_position_size() != 0.0) strategy_close_all();
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
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    const std::vector<Row> tape = tape_trades(end_ms);
    CHECK(tape.size() == 84u);

    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    OpenNewbornHost host;
    host.set_trade_start_time(kEth15[0].ts);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> lane;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        lane.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                          t.exit_time, ticks(t.exit_price));
    }
    CHECK(lane == tape);
    if (lane != tape) {
        show("tape", tape);
        show("engine", lane);
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
