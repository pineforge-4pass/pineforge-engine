/*
 * test_coof_w2_refill_tapes.cpp -- R5 lane TAIL-C.
 *
 * Under calc_on_order_fills an exit X that the path reaches only at a leg's
 * end is filled there, at the extreme's print, by the adapter (a fill FORCED
 * onto the point). The recalculation X's fill starts places a refill R.
 * TradingView, on the lane's own synthetic script (tests/fixtures/
 * coof_w2_refill, one `lab tv` export per variant, BINANCE:BTCUSDT 15,
 * 2025-04-01 .. 2025-05-01; the README names each tape):
 *   - X forced onto the bar's SECOND extreme: R is live only from the next
 *     bar's open, never on the rest of X's bar -- filled at that open when it
 *     is executable there, otherwise at its own level later -- whether or not
 *     R is executable at X's fill, and even when the leg to the close crosses
 *     its level (-e, -f);
 *   - X forced onto the FIRST extreme: R executable at the second extreme
 *     fills there, at its print (-c, -d), and one that is not rests at its own
 *     level from there (-a, -b: filled later in the bar, on a later bar or
 *     never).
 * The engine released R at the recalculation, where the kernel booked a stop
 * already through at its level on the rest of X's bar (the job-759 PVRPLE
 * refills on BTCUSDT/ETHUSDT.P/EURUSD 1D and BTCUSDT 15m).
 *
 * The script is stateless (every decision reads strategy.* only), so the
 * recalculation's rollback cannot lose a decision. P: a stop entry 40 beyond
 * the close of every even UTC hour's first bar, cancelled on the bar at :45
 * when unfilled; X: P's take profit 30 beyond its average; P closed after six
 * bars. R (per variant, below) closed after two bars, cancelled on the next
 * bar when it never filled. Each row replays the tape through the Pine
 * adapter over the lane's 15m bars (tests/fixtures/coof_refill_waypoint/
 * bars.inc: the btcusdt-15 chart feed's first four days) and requires every
 * trade the tape closes inside those bars to be the engine's: entry and exit
 * time, side, price and quantity.
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

#ifndef PINEFORGE_COOF_W2_REFILL_FIXTURE_DIR
#error "PINEFORGE_COOF_W2_REFILL_FIXTURE_DIR must name tests/fixtures/coof_w2_refill"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_refill_waypoint/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:BTCUSDT quantity step and price tick (lane btcusdt-15).
constexpr double kLot = 0.00001;
constexpr double kTick = 0.01;

// (entry ms, long, entry price, quantity, exit ms, exit price), prices in
// ticks and quantity in lots.
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kBtc15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
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
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

// The tape's trades closed inside the replayed bars, in trade-number order.
std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_COOF_W2_REFILL_FIXTURE_DIR) + "/" + tape
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
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty), ...
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

enum class Refill { StopAtPEntry, LimitAtXPlus10, StopAtXInside5 };

struct Variant {
    const char* tape;
    bool p_long;        // P's side; X is its take profit, 30 beyond the average
    Refill refill;      // R's shape
    std::size_t closed; // tape trades closed inside the bars
};

// The probe's body (tests/fixtures/coof_w2_refill/<slug>/strategy.pine).
class W2RefillHost final : public source::PineStrategyHost {
public:
    explicit W2RefillHost(const Variant& v) : v_(v) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.initial_capital = 10000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const int k = bar_index_;
        const int closed = trade_count();  // strategy.closedtrades
        const bool exit_here = closed > 0 && closed_trade_exit_bar_index(closed - 1) == k;
        const bool exit_was_x = closed > 0 && closed_trade_exit_id(closed - 1) == "X";
        const bool flat = signed_position_size() == 0.0;
        const std::string open_id = flat ? std::string() : open_trade_entry_id(0);
        const int held = flat ? 0 : k - open_trade_entry_bar_index(0);
        const std::int64_t minute_of_day = (current_bar_.timestamp / 60'000) % 1440;
        // minute(time) == 0 and hour(time, "UTC") % 2 == 0 / minute(time) == 45
        const bool slot = minute_of_day % 120 == 0;
        const bool at_45 = minute_of_day % 60 == 45;
        const double side = v_.p_long ? 1.0 : -1.0;
        if (flat && !exit_here && slot)
            strategy_entry("P", v_.p_long, kNaN, current_bar_.close + side * 40.0);
        if (flat && !exit_here && at_45) strategy_cancel("P");
        if (open_id == "P") {
            strategy_exit("X", "P", position_avg_price() + side * 30.0, kNaN);
            if (held >= 6) strategy_close("P");
        }
        if (flat && exit_here && exit_was_x) {
            switch (v_.refill) {
            case Refill::StopAtPEntry:
                strategy_entry("R", v_.p_long, kNaN, closed_trade_entry_price(closed - 1));
                break;
            case Refill::LimitAtXPlus10:
                strategy_entry("R", v_.p_long,
                               closed_trade_exit_price(closed - 1) + side * 10.0);
                break;
            case Refill::StopAtXInside5:
                strategy_entry("R", !v_.p_long, kNaN,
                               closed_trade_exit_price(closed - 1) - side * 5.0);
                break;
            }
        }
        if (flat && !exit_here) strategy_cancel("R");
        if (open_id == "R" && held >= 2) strategy_close("R");
    }

private:
    const Variant& v_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(const Variant& v, std::int64_t end_ms) {
    W2RefillHost host(v);
    host.set_trade_start_time(kBtc15[0].ts);
    const std::vector<Bar> bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    Run out;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
    }
    return out;
}

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
    const Variant variants[] = {
        {"tailc-w2r-a-buystop", true, Refill::StopAtPEntry, 74},
        {"tailc-w2r-b-sellstop", false, Refill::StopAtPEntry, 75},
        {"tailc-w2r-c-buylimit", true, Refill::LimitAtXPlus10, 82},
        {"tailc-w2r-d-selllimit", false, Refill::LimitAtXPlus10, 80},
        {"tailc-w2r-e-sellstop-resting", true, Refill::StopAtXInside5, 81},
        {"tailc-w2r-f-buystop-resting", false, Refill::StopAtXInside5, 80},
    };
    for (const Variant& v : variants) {
        std::printf("-- %s\n", v.tape);
        const std::vector<Row> tape = tape_trades(v.tape, end_ms);
        CHECK(tape.size() == v.closed);
        const Run lane = run(v, end_ms);
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape);
        if (lane.trades != tape) {
            show("tape", tape);
            show("engine", lane.trades);
        }
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
