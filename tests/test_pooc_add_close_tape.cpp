/*
 * test_pooc_add_close_tape.cpp -- R5 lanes W10-DIAG-UNKNOWN (diag-w5r) and
 * TAIL-C.
 *
 * Under process_orders_on_close the close pass fills every priced entry the
 * bar's calculation placed whose price the close's tick reaches: limits at
 * that tick unslipped, stops slipped -- a limit or stop ADD placed while its
 * own side is held (cells A, B, C, E, H), a flat limit or stop (F, G) and a
 * limit placed against the held side, reversing it (J); an add the close does
 * not reach rests (D). The pass then decides every exit the calculation
 * placed against that same close, the exits that name an entry the pass just
 * filled included (A2x, B2x, E2x, F1x, G1x, H2x, J2x). The engine filled only
 * flat entries and reversing stops there, and decided the exits before the
 * close-pass entries had filled: the adds and their exits a bar late (job-2847
 * adaptive S/R DCA on ETHUSDT.P 1D, its pyramid add S2 and exit xS2).
 * strategy.equity at t1's close is net of the open lot's paid entry fee (cell
 * N; lane W8E-EXITS's rule).
 *
 * The lane W10 diag-w5r synthetic script (tests/fixtures/pooc_add_close, one
 * `lab tv` export, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01; cells on
 * 2025-04-14 .. 04-16, each a market entry at t0 and orders at t0 + 15m,
 * fixed 1, slippage 3, commission 0.05 %). The row replays the tape through
 * the Pine adapter over the corpus 15m chart feed (bars.inc) and requires all
 * 23 trades -- entry and exit time, side, price and quantity -- to be the
 * engine's.
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

#ifndef PINEFORGE_POOC_ADD_CLOSE_FIXTURE_DIR
#error "PINEFORGE_POOC_ADD_CLOSE_FIXTURE_DIR must name tests/fixtures/pooc_add_close"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/pooc_add_close/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// BINANCE:ETHUSDT.P's quantity step and price tick (lane eth-scraped-15).
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;

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

// timestamp("UTC", 2025, 4, day, hour, minute)
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

std::vector<Row> tape_trades() {
    std::ifstream in(std::string(PINEFORGE_POOC_ADD_CLOSE_FIXTURE_DIR)
                     + "/w5r-pooc-add-close/tv_trades.csv");
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
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

// The probe's body (tests/fixtures/pooc_add_close/w5r-pooc-add-close/strategy.pine).
class AddCloseHost final : public source::PineStrategyHost {
public:
    AddCloseHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.process_orders_on_close = true;
        c.pyramiding = 3;
        c.initial_capital = 100000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.05;
        c.slippage = 3;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        t_ = current_bar_.timestamp;
        if (t_ == at(14, 0, 0)) entry("A1", false);
        if (t_ == at(14, 0, 15)) {
            entry("A2", false, close() - 5);
            exit("A1x", "A1", kNaN, close() - 3);
            exit("A2x", "A2", kNaN, close() - 3);
        }
        if (t_ == at(14, 4, 0)) entry("B1", true);
        if (t_ == at(14, 4, 15)) {
            entry("B2", true, close() + 5);
            exit("B1x", "B1", kNaN, close() + 3);
            exit("B2x", "B2", kNaN, close() + 3);
        }
        if (t_ == at(14, 8, 0)) entry("C1", false);
        if (t_ == at(14, 8, 15)) {
            entry("C2", false, close() - 5);
            exit("C1x", "C1", close() - 30, close() + 30);
            exit("C2x", "C2", close() - 30, close() + 30);
        }
        if (t_ == at(14, 12, 0)) entry("D1", true);
        if (t_ == at(14, 12, 15)) entry("D2", true, close() - 5);
        if (t_ == at(14, 16, 0)) entry("E1", false);
        if (t_ == at(14, 16, 15)) {
            entry("E2", false, close() - 5);
            exit("E1x", "E1", close() + 3, kNaN);
            exit("E2x", "E2", close() + 3, kNaN);
        }
        if (t_ == at(15, 0, 0)) {
            entry("F1", true, close() + 5);
            exit("F1x", "F1", kNaN, close() + 3);
        }
        if (t_ == at(15, 4, 0)) {
            strategy_entry("G1", false, kNaN, close() + 5, kNaN, "G1");
            exit("G1x", "G1", kNaN, close() - 3);
        }
        if (t_ == at(15, 8, 0)) entry("H1", true);
        if (t_ == at(15, 8, 15)) {
            strategy_entry("H2", true, kNaN, close() - 5, kNaN, "H2 add");
            exit("H2x", "H2", kNaN, close() + 3);
        }
        if (t_ == at(15, 12, 0)) entry("I1", false);
        if (t_ == at(15, 12, 15)) {
            exit("I2x", "I2", kNaN, close() - 3);
            entry("I2", false, close() - 5);
        }
        if (t_ == at(15, 16, 0)) entry("J1", true);
        if (t_ == at(15, 16, 15)) {
            entry("J2", false, close() - 5);
            exit("J2x", "J2", kNaN, close() - 3);
        }
        if (t_ == at(16, 0, 0)) entry("L1", false);
        if (t_ == at(16, 0, 15)) {
            entry("L2", false);
            exit("L2x", "L2", kNaN, close() - 3);
        }
        if (t_ == at(16, 4, 0)) entry("N1", true);
        if (t_ == at(16, 4, 15)) {
            // strategy.equity and strategy.openprofit at this close
            const double equity = current_equity() + open_profit(close());
            const double open = open_profit(close());
            strategy_entry("N2", true, kNaN, kNaN, round1(equity - 99000.0) / 1000.0, "N2 equity");
            strategy_entry("N3", true, kNaN, kNaN, round1(open + 100.0) / 1000.0, "N3 openprofit");
        }
        for (const auto& [d, h] : {std::pair<unsigned, int>{14, 2}, {14, 6}, {14, 10}, {14, 14},
                                   {14, 18}, {15, 2}, {15, 6}, {15, 10}, {15, 14}, {15, 18},
                                   {16, 2}, {16, 6}}) {
            if (t_ == at(d, h, 0)) {
                strategy_cancel_all();
                strategy_close("", "cleanup", kNaN, kNaN, false);
            }
        }
    }

private:
    double close() const { return current_bar_.close; }
    // math.round(x, 1)
    static double round1(double x) { return std::round(x * 10.0) / 10.0; }
    void entry(const char* id, bool is_long, double limit = kNaN) {
        strategy_entry(id, is_long, limit, kNaN, kNaN, id);
    }
    void exit(const char* id, const char* from, double limit, double stop) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, 100.0, id);
    }
    std::int64_t t_ = 0;
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
    const std::vector<Row> tape = tape_trades();
    CHECK(tape.size() == 23u);
    std::vector<Bar> bars;
    for (const FeedBar& row : kEthAdd) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    AddCloseHost host;
    host.set_trade_start_time(kEthAdd[0].ts);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> lane;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
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
