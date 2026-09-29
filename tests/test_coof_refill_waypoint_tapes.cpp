/*
 * test_coof_refill_waypoint_tapes.cpp -- R5 lane W8D-NOTRADES.
 *
 * Under calc_on_order_fills a fill inside a leg of the bar's path (O -> first
 * extreme -> second extreme -> C) starts a recalculation there. TradingView
 * makes a priced entry that recalculation places live only from the leg's END,
 * the next extreme: an entry already executable at the recalculating fill's
 * price fills AT that extreme, at its print, when it is still executable
 * there. The engine filled such an entry at once, at its own level -- a price
 * the path never offered after the order existed (the bystry1991-ema200
 * refills after a same-bar exit on CME_MINI:ES1! and ETHUSDT.P 15m).
 *
 * The lane's own synthetic script (tests/fixtures/coof_refill_waypoint, one
 * `lab tv` export per variant, BINANCE:BTCUSDT 15, 2025-04-01 .. 2025-05-01;
 * the README names each tape), stateless so the recalculation's rollback
 * cannot lose a decision: a position P opens every four hours (UTC slots) with
 * an exit X, and is closed after eight bars; the recalculation X's fill
 * starts places the refill R at an offset from X's fill; R is closed after
 * four bars and cancelled on the next bar when it never filled.
 *   -a  P long, X stop 150 under P's price: R a BUY limit 600 above X's fill
 *       (executable at the recalculation; the next extreme is the low);
 *   -b  the mirror: P short, R a SELL limit 600 below X's fill;
 *   -c  control: R a BUY limit 60 BELOW X's fill (not executable at the
 *       recalculation), which this engine already books at the extreme;
 *   -d  P long, X a limit 150 above P's price, filled on a rising leg: R a BUY
 *       limit 60 above X's fill -- executable at the recalculation; the next
 *       extreme (the high) may lie above it;
 *   -e  R a BUY STOP 300 under X's fill (already through);
 *   -f  the mirror: P short, R a SELL STOP 300 over X's fill.
 * Each row replays TradingView's own tape through the Pine adapter over the
 * lane's 15m bars embedded in bars.inc (the tapes' first four days) and
 * requires every trade the tape closes inside those bars to be the engine's:
 * entry and exit time, side, price and quantity.
 *
 * An R that is not executable at the next extreme either rests at its own
 * level from there in TradingView, filling later in the bar, on a later bar
 * or never (-d's limits, -e's stops). R5 lane TAIL-C rests it so: a limit
 * arms at the extreme (StopLimit{extreme, level}), a stop rides a trail armed
 * at the extreme at its level's distance -- no print beyond the extreme
 * follows on that bar -- and is the plain stop again from the next open. The
 * rows this file recorded as the engine's own (the level fill at placement)
 * are gone; every variant replays exactly. (A magnified script's refills rest
 * from its intrabar path's next point instead, PineExecutionAdapter::entry().)
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
#include <utility>
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

#ifndef PINEFORGE_COOF_REFILL_FIXTURE_DIR
#error "PINEFORGE_COOF_REFILL_FIXTURE_DIR must name tests/fixtures/coof_refill_waypoint"
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

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

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

struct Tape {
    std::vector<Row> trades;  // closed inside the replayed bars, in entry order
    std::size_t refills = 0;  // of which R entries on the bar X exited
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_COOF_REFILL_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal;
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
            entry_signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
        }
    }
    Tape out;
    std::int64_t previous_exit = -1;
    for (const auto& [number, row] : by_number) {
        if (std::get<4>(row) > 0 && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            if (entry_signal[number] == "R" && std::get<0>(row) == previous_exit) ++out.refills;
        }
        previous_exit = std::get<4>(row);
    }
    return out;
}

struct Variant {
    const char* tape;
    bool p_long;         // P's side
    double x_offset;     // X's level: P's average price + x_offset
    bool x_stop;         // X is a stop (else a limit)
    bool r_long;         // R's side
    double r_offset;     // R's level: X's fill + r_offset
    bool r_stop;         // R is a stop entry (else a limit)
    std::size_t closed;  // tape trades closed inside the bars
    std::size_t refills; // R rows on the bar X exited
    // Recorded rows (entry ms, entry price in ticks): the engine's that the
    // tape lacks, and the tape's that the engine lacks.
    std::vector<std::pair<std::int64_t, long long>> engine_only, tape_only;
};

// The probe's body (tests/fixtures/coof_refill_waypoint/<slug>/strategy.pine).
class RefillHost final : public source::PineStrategyHost {
public:
    explicit RefillHost(const Variant& v) : v_(v) {
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
        // minute(time) == 0 and hour(time, "UTC") % 4 == 0
        const bool slot = (current_bar_.timestamp / 60'000) % 240 == 0;
        if (flat && !exit_here && slot) strategy_entry("P", v_.p_long);
        if (open_id == "P") {
            const double level = position_avg_price() + v_.x_offset;
            if (v_.x_stop) strategy_exit("X", "P", kNaN, level);
            else strategy_exit("X", "P", level, kNaN);
            if (held >= 8) strategy_close("P");
        }
        if (flat && exit_here && exit_was_x) {
            const double level = closed_trade_exit_price(closed - 1) + v_.r_offset;
            if (v_.r_stop) strategy_entry("R", v_.r_long, kNaN, level);
            else strategy_entry("R", v_.r_long, level);
        }
        if (flat && !exit_here) strategy_cancel("R");
        if (open_id == "R" && held >= 4) strategy_close("R");
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
    RefillHost host(v);
    host.set_trade_start_time(kBtc15[0].ts);
    const std::vector<Bar> bars = feed();
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
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// A row opened at `entry_ms` for `price` (ticks): the refill R, never the P
// that may open on the same bar.
bool is_entry(const Row& r, std::int64_t entry_ms, long long price) {
    return std::get<0>(r) == entry_ms && std::get<2>(r) == price;
}

bool has_entry(const std::vector<Row>& rows, std::int64_t entry_ms, long long price) {
    for (const Row& r : rows)
        if (is_entry(r, entry_ms, price)) return true;
    return false;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kBtc15[sizeof(kBtc15) / sizeof(kBtc15[0]) - 1].ts;
    const Variant variants[] = {
        {"w8d-coof2-a-buy-marketable", true, -150.0, true, true, 600.0, false, 41, 17, {}, {}},
        {"w8d-coof2-b-sell-marketable", false, 150.0, true, false, -600.0, false, 43, 19, {}, {}},
        {"w8d-coof2-c-buy-resting", true, -150.0, true, true, -60.0, false, 38, 11, {}, {}},
        {"w8d-coof2-d-buy-after-tp", true, 150.0, false, true, 60.0, false, 41, 11, {}, {}},
        {"w8d-coof2-e-buystop-through", true, -150.0, true, true, -300.0, true, 39, 14, {}, {}},
        {"w8d-coof2-f-sellstop-through", false, 150.0, true, false, 300.0, true, 43, 19, {}, {}},
    };
    for (const Variant& v : variants) {
        std::printf("-- %s\n", v.tape);
        const Tape tape = tape_trades(v.tape, end_ms);
        CHECK(tape.trades.size() == v.closed);
        CHECK(tape.refills == v.refills);
        const Run lane = run(v, end_ms);
        CHECK(lane.error.empty());
        // Every row but the recorded ones is TradingView's; each recorded row
        // is on its own side only.
        const auto recorded = [](const Row& r, const std::vector<std::pair<std::int64_t, long long>>& keys) {
            for (const auto& [ms, price] : keys)
                if (is_entry(r, ms, price)) return true;
            return false;
        };
        std::vector<Row> engine_kept, tape_kept;
        for (const Row& r : lane.trades)
            if (!recorded(r, v.engine_only)) engine_kept.push_back(r);
        for (const Row& r : tape.trades)
            if (!recorded(r, v.tape_only)) tape_kept.push_back(r);
        CHECK(lane.trades.size() == engine_kept.size() + v.engine_only.size());
        CHECK(tape.trades.size() == tape_kept.size() + v.tape_only.size());
        CHECK(engine_kept == tape_kept);
        if (engine_kept != tape_kept) {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
        for (const auto& [ms, price] : v.engine_only) CHECK(!has_entry(tape.trades, ms, price));
        for (const auto& [ms, price] : v.tape_only) CHECK(!has_entry(lane.trades, ms, price));
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
