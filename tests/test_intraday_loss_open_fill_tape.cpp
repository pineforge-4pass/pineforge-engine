/*
 * test_intraday_loss_open_fill_tape.cpp — lane W8A-SIGSTATE-1, rule R4.
 *
 * strategy.risk.max_intraday_loss measures the day from the equity at the
 * day's first tick. An order carried from the previous day that closes a
 * position at that open realizes P&L that equity already holds, so
 * TradingView does not count it as the day's loss -- whether the carried
 * order is a MARKET strategy.close or a priced strategy.exit. The adapter's
 * own-fill exclusion (test_risk_max_intraday_loss_tv t1: an INTRABAR exit's
 * own realized P&L is not yet in the equity TradingView checks) also ran at
 * the open: it read the winner's whole open profit as a loss at least the
 * 1.5 % threshold, blocked the day and dropped every later order. The
 * scraped officialjackofalltrades-aureate-market-architecture-strategy-joat
 * (BINANCE:ETHUSDT.P 1D, 2026-02-07) lost TradingView's two re-entries of
 * that day this way.
 *
 * One lab tv export (tests/fixtures/intraday_loss_open_fill/w8a-ild-open,
 * BINANCE:ETHUSDT.P 1D, 2025-04-01 .. 2026-05-01) replayed through the Pine
 * adapter under the configuration its generated constructor declares, over
 * the lane's own daily bars (bars.inc): every trade must be the engine's --
 * entry and exit time, side, price and quantity -- and the rule is read off
 * TradingView's rows:
 *
 *   K  the control: a long held into 2025-10-07 closes at that day's low by
 *      "Close Position (Max intraday Loss)";
 *   P  a short winner closed at the 2025-11-13 open by a limit exit carried
 *      from the 11-12 close; the buy limit PL placed beside it fills that day
 *      and is closed at the low by the rule;
 *   W  the probe's shape: a short winner closed at the 2026-02-07 open by a
 *      strategy.close carried from the 02-06 close; WL fills and is closed at
 *      the low by the rule.
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

#ifndef PINEFORGE_INTRADAY_LOSS_OPEN_FILL_FIXTURE_DIR
#error "PINEFORGE_INTRADAY_LOSS_OPEN_FILL_FIXTURE_DIR must name tests/fixtures/intraday_loss_open_fill"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/intraday_loss_open_fill/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;

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

// The probe's cell(y, m, d): the daily bar of that UTC date.
std::int64_t day(int y, unsigned m, unsigned d) { return days_from_civil(y, m, d) * 86'400'000; }

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 24
             + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;
    std::vector<std::string> entry, exit;
};

Tape tape_trades() {
    std::ifstream in(std::string(PINEFORGE_INTRADAY_LOSS_OPEN_FILL_FIXTURE_DIR)
                     + "/w8a-ild-open/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
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
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.entry.push_back(entry_signal[number]);
        out.exit.push_back(exit_signal[number]);
    }
    return out;
}

// w8a-ild-open as its generated TU lowers it (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.commission_type = static_cast<int>(CommissionType::PERCENT);
        cfg.commission_value = 0.01;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        set_pine_risk_max_intraday_loss(1.5, true);
        const std::int64_t t = current_bar_.timestamp;
        if (t == day(2025, 10, 6))
            strategy_entry("K", true, kNaN, kNaN, 15, "K long", "", 0, -1);
        if (t == day(2025, 10, 7))
            strategy_close("", "K cleanup", kNaN, kNaN, false);
        if (t == day(2025, 11, 2))
            strategy_entry("P", false, kNaN, kNaN, 4.8, "P short", "", 0, -1);
        if (t == day(2025, 11, 12)) {
            strategy_exit("PX", "P", 3500.0, kNaN, kNaN, kNaN, kNaN, 100.0, "P limit exit",
                          kNaN, "", kNaN, kNaN);
            strategy_order("PL", true, 8.3, 3400.0, kNaN, "", 0);
        }
        if (t == day(2025, 11, 13)) {
            strategy_cancel_all();
            strategy_close("", "P cleanup", kNaN, kNaN, false);
        }
        if (t == day(2026, 1, 30))
            strategy_entry("W", false, kNaN, kNaN, 3.7064, "W short", "", 0, -1);
        if (t == day(2026, 2, 6)) {
            strategy_close("W", "W close", kNaN, kNaN, false, 146028888083ULL);
            strategy_order("WL", true, 29.8, 2062.0, kNaN, "", 0);
        }
        if (t == day(2026, 2, 7)) {
            strategy_cancel_all();
            strategy_close("", "W cleanup", kNaN, kNaN, false);
        }
    }
};

std::vector<Row> run(std::string& error) {
    ProbeHost host;
    // The bar before the first order's bar (K, placed at the 10-06 close).
    host.set_trade_start_time(day(2025, 10, 5));
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth1d) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "1D", false);
    error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
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
    const Tape tape = tape_trades();
    CHECK(tape.trades.size() == 5);

    std::printf("-- w8a-ild-open\n");
    std::string error;
    const std::vector<Row> engine = run(error);
    CHECK(error.empty());
    CHECK(engine == tape.trades);
    if (engine != tape.trades) {
        show("tape", tape.trades);
        show("engine", engine);
    }

    // The rule, read off TradingView's rows.
    std::printf("-- the rule, on the tape\n");
    const std::string fire = "Close Position (Max intraday Loss)";
    if (tape.trades.size() == 5) {
        // K: the control fires at the day's low.
        CHECK(tape.entry[0] == "K long" && tape.exit[0] == fire);
        CHECK(std::get<5>(tape.trades[0]) == ticks(4422.44));
        // P and W: the winners close at the open by their own carried orders
        // (priced and market), and the rule does not fire there...
        CHECK(tape.entry[1] == "P short" && tape.exit[1] == "P limit exit");
        CHECK(std::get<4>(tape.trades[1]) == day(2025, 11, 13));
        CHECK(tape.entry[3] == "W short" && tape.exit[3] == "W close");
        CHECK(std::get<4>(tape.trades[3]) == day(2026, 2, 7));
        CHECK(std::get<5>(tape.trades[3]) == ticks(2062.55));
        // ...so the day's buy limits fill, and the rule fires on their loss.
        CHECK(tape.entry[2] == "PL long" && tape.exit[2] == fire);
        CHECK(tape.entry[4] == "WL long" && tape.exit[4] == fire);
        CHECK(std::get<5>(tape.trades[4]) == ticks(1993.34));
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
