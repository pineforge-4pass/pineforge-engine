/*
 * test_nonfinite_entry_qty_tape.cpp -- R5 lane TAIL-C.
 *
 * strategy.entry with an infinite quantity trades the strategy's default
 * quantity, as a quantity of na does: TradingView enters every +Infinity,
 * -Infinity and na leg of the lane's own synthetic script at the default
 * 0.01 (tests/fixtures/nonfinite_entry_qty/tailc-a-qty-nonfinite2, one
 * `lab tv --no-note` export, BINANCE:BTCUSDT 15, 2025-04-01 .. 2025-04-08).
 * The adapter dropped an infinite quantity (the job-2545 MR VWAP SVP TDV
 * strategy sizes math.floor(risk / 0) on its VWAP reset bars).
 *
 * The script, stateless: one leg per UTC hour (hour % 9) at the hh:00 bar's
 * close under process_orders_on_close, closed by close_all at the hh:15
 * bar's close; the default quantity is 0.01 fixed. This row ports the legs
 * whose quantity reaches the adapter as a double: C (0.02, the control),
 * FINF (100 / 0), FNA (na) and NINF (-100 / 0) -- the int legs are the
 * codegen's (pineforge-codegen, lane TAIL-C). It replays the tape through the
 * Pine adapter over the btcusdt-15 lane's first four days
 * (tests/fixtures/coof_refill_waypoint/bars.inc) and requires every trade of
 * those legs the tape closes inside the bars to be the engine's: entry and
 * exit time, side, price and quantity.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
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

#ifndef PINEFORGE_NONFINITE_QTY_FIXTURE_DIR
#error "PINEFORGE_NONFINITE_QTY_FIXTURE_DIR must name tests/fixtures/nonfinite_entry_qty"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_refill_waypoint/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
// TradingView's BINANCE:BTCUSDT quantity step and price tick (lane btcusdt-15).
constexpr double kLot = 0.00001;
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

// The tape's trades of the ported legs closed before `end_ms`.
std::vector<Row> tape_trades(std::int64_t end_ms) {
    const std::set<std::string> ported = {"C", "FINF", "FNA", "NINF"};
    std::ifstream in(std::string(PINEFORGE_NONFINITE_QTY_FIXTURE_DIR)
                     + "/tailc-a-qty-nonfinite2/tv_trades.csv");
    std::map<int, Row> by_number;
    std::set<int> leg;
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
            if (ported.count(cell[3])) leg.insert(number);
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
        if (leg.count(number) && std::get<4>(row) > 0 && std::get<4>(row) < end_ms)
            out.push_back(row);
    return out;
}

// The probe's body (tests/fixtures/nonfinite_entry_qty/tailc-a-qty-nonfinite2/
// strategy.pine), its double-quantity legs.
class NonfiniteQtyHost final : public source::PineStrategyHost {
public:
    NonfiniteQtyHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.process_orders_on_close = true;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 0.01;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t minute_of_day = (current_bar_.timestamp / 60'000) % 1440;
        const int slot = minute_of_day % 60 == 0 ? static_cast<int>(minute_of_day / 60) % 9 : -1;
        if (signed_position_size() != 0.0) strategy_close("", "x", kNaN, kNaN, false);
        if (slot == 0) strategy_entry("C", true, kNaN, kNaN, 0.02);
        if (slot == 1) strategy_entry("FINF", true, kNaN, kNaN, kInf);   // 100 / z
        if (slot == 4) strategy_entry("FNA", true, kNaN, kNaN, kNaN);    // qty = na
        if (slot == 7) strategy_entry("NINF", true, kNaN, kNaN, -kInf);  // -100 / z
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
    const std::int64_t end_ms = kBtc15[sizeof(kBtc15) / sizeof(kBtc15[0]) - 1].ts;
    const std::vector<Row> tape = tape_trades(end_ms);
    CHECK(tape.size() == 44u);

    std::vector<Bar> bars;
    for (const FeedBar& row : kBtc15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    NonfiniteQtyHost host;
    host.set_trade_start_time(kBtc15[0].ts);
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
