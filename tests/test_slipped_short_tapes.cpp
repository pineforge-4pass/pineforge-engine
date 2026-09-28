/*
 * test_slipped_short_tapes.cpp — lane INT28-FIX.
 *
 * A default-quantity percent_of_equity MARKET short from a flat book with
 * slippage, at Pine v6's defaults (100% of equity per order, margin 100 both
 * ways), on NYSE:F's one-share lot grid. Each rule is read off TradingView's
 * own tapes of synthetic probes written for this lane:
 *
 *   SS  The short is sized at the signal close less the slippage ticks (and
 *       the commission) and opens at the next open only while those units'
 *       margin at the signal close, on its tick, fits the equity there.
 *   OU  A margin call whose restore floors below one unit takes that unit
 *       only where it restores the book at the call's own fill: the whole
 *       position's requirement at the liquidation's slipped print less the
 *       equity at the mark is under a unit's margin.
 *   OF  A one-unit call that leaves the book short at its own fill -- by more
 *       than a unit's margin and two slippage steps, re-marked there -- is
 *       followed by one more unit at the bar's next path point, where that
 *       point's own check calls nothing and its print is inside the call's
 *       fill; a unit so taken is followed alike.
 *
 * Each row replays one lab tv tape (tests/fixtures/slipped_short) through the
 * Pine adapter under the configuration the generated constructor declares for
 * its probe, over the lab lane f-15 NYSE:F 15m bars in bars.inc, and requires
 * every trade the tape closes inside those bars (or before its own window's
 * end) to be the engine's: entry and exit time, side, price in ticks and
 * quantity in shares. On the commit before a rule every rule row fails here,
 * and every control row beside it passes.
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

#ifndef PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR
#error "PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR must name tests/fixtures/slipped_short"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/slipped_short/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;   // NYSE:F's price tick
constexpr double kLot = 1.0;     // one share
constexpr std::int64_t kMinute = 60'000;

// One trade as both sides report it, prices in ticks and quantity in shares:
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

// 2025-<month>-<day> 00:00 UTC.
std::int64_t midnight(unsigned month, unsigned day) {
    return days_from_civil(2025, month, day) * 1440 * kMinute;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_SLIPPED_SHORT_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    CHECK(in.good());
    std::map<int, Row> by_number;
    std::map<int, bool> closed;
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
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) {
        if (closed.count(number) && std::get<4>(row) < end_ms) out.push_back(row);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
enum class Probe {
    TwoCells,  // int28fix-adm-*: 10:00 and 13:00 New York, flattened 45 minutes in
    Hourly,    // int28fix-ou-*: on the hour 10:00 .. 14:00 New York, alike
};

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, bool is_long, const source::PineStrategyConfig& config)
        : probe_(probe), is_long_(is_long) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("0930-1600");
    }

    // hour(time, "America/New_York") and minute(time, ...): the window is
    // EDT (UTC-4) throughout.
    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const std::int64_t minute_of_day = ((t / kMinute) % 1440 + 1440) % 1440;
        const int hour = static_cast<int>((minute_of_day / 60 + 24 - 4) % 24);
        const int minute = static_cast<int>(minute_of_day % 60);
        const bool hour_in = probe_ == Probe::TwoCells ? (hour == 10 || hour == 13)
                                                        : (hour >= 10 && hour <= 14);
        if (minute == 0 && hour_in && signed_position_size() == 0.0)
            strategy_entry("E", is_long_, kNaN, kNaN, kNaN);
        if (minute == 45 && hour_in) strategy_close("", "cleanup", kNaN, kNaN, false);
    }

private:
    Probe probe_;
    bool is_long_;
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

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, bool is_long, const source::PineStrategyConfig& config,
        const std::vector<Bar>& bars, std::int64_t end_ms) {
    ProbeHost host(probe, is_long, config);
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
        std::printf("    %-8s %lld %s @%lld ticks q=%lld -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What a probe's strategy() declares, over v6's defaults (100 % of equity,
// pyramiding 1, margin 100 both ways).
source::PineStrategyConfig config(double capital, double commission_percent, int slippage) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 1;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission_percent;
    c.slippage = slippage;
    return c;
}

struct Case {
    const char* rule;
    const char* tape;
    Probe probe;
    bool is_long;
    source::PineStrategyConfig lane;  // what its generated constructor declares
    std::size_t closed;               // tape trades closed inside the bars
    std::int64_t end_ms = 0;          // the tape's window end, when before the bars'
};

}  // namespace

int main() {
    const std::vector<Bar> bars = feed(kF15Q2);

    const Case cases[] = {
        {"SS OU", "int28fix-adm-s-s", Probe::TwoCells, false, config(10000.0, 0.0, 1), 14},
        {"SS OU", "int28fix-ou-s3", Probe::Hourly, false, config(4000.0, 0.04, 3), 17},
        {"SS", "int28fix-ou-s4", Probe::Hourly, false, config(4000.0, 0.04, 4), 0},
        // The windows end before the two shapes these tapes hold beyond these
        // rules (README, "Open"): a one-unit call a bar's close takes at the
        // next open, and a follow-up on a close that a close_all placed there
        // then overshoots.
        {"SS OU OF", "int28fix-adm-s-cs", Probe::TwoCells, false, config(10000.0, 0.04, 1), 135,
         midnight(6, 26)},
        {"SS OU OF", "int28fix-ou-s1", Probe::Hourly, false, config(4000.0, 0.04, 1), 192,
         midnight(5, 1)},
        {"control", "int28fix-adm-s-0", Probe::TwoCells, false, config(10000.0, 0.0, 0), 218},
        {"control", "int28fix-adm-s-c", Probe::TwoCells, false, config(10000.0, 0.04, 0), 231},
        {"control", "int28fix-adm-l-cs", Probe::TwoCells, true, config(10000.0, 0.04, 1), 121},
    };

    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rule, c.tape);
        const std::int64_t end = c.end_ms != 0 ? c.end_ms : bars.back().timestamp;
        const std::vector<Row> tape = tape_trades(c.tape, end);
        CHECK(tape.size() == c.closed);
        if (tape.size() != c.closed) std::printf("    tape closes %zu\n", tape.size());
        const Run lane = run(c.probe, c.is_long, c.lane, bars, end);
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
