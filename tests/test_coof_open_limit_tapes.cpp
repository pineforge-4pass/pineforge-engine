/*
 * test_coof_open_limit_tapes.cpp -- R5 lane TAIL-D.
 *
 * Under calc_on_order_fills the recalculation of an entry's fill at the
 * bar's open places the exits a script issues once a position exists.
 * TradingView fills such an exit limit at once when it sits at or through
 * the open print -- at the print, without slippage, on the entry bar -- and
 * rests one that sits short of the print at its own level for the rest of
 * that bar. The adapter judged both against the slipped entry fill and held
 * them for the next bar (finnp17-cleantradequantum on NSE:NIFTY 1D: a TP1
 * already below the open fills there on TradingView).
 *
 * The lane's own synthetic script (tests/fixtures/coof_open_limit, one `lab
 * tv --no-note` export, NSE:NIFTY 1D, 2025-05-01 .. 2025-08-01,
 * calc_on_order_fills on, slippage 2, a fixed size of 1): one cell a week,
 * each a market entry on a Monday whose exit the recalculation of its open
 * fill places ("O" the entry bar's open, "t" one tick) --
 *   LA  long, exit limit O - 20t with a far stop;
 *   SB  short, exit limit O + 20t with a far stop;
 *   LE  long, exit limit O - 20t alone;
 *   LG  long, exit limit O + 1t (short of the print, under the slipped fill);
 *   LF  long, exit limit O + 10t (control: reached later in the bar);
 *   LC / SD  strategy.close of the long / short at that recalculation.
 * The row replays TradingView's own tape through the Pine adapter over the
 * tape's bars (bars.inc) and requires every limit cell's trade to be the
 * engine's: entry and exit time, side, price in ticks and quantity.
 *
 * Fail-before (lane report): LA, SB, LE and LG exit on a later bar.
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

#ifndef PINEFORGE_TAIL_D_OPEN_LIMIT_FIXTURE_DIR
#error "PINEFORGE_TAIL_D_OPEN_LIMIT_FIXTURE_DIR must name tests/fixtures/coof_open_limit"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_open_limit/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.05;  // NSE:NIFTY
constexpr std::int64_t kIst = 330;  // Asia/Kolkata, minutes east of UTC

// (entry ms, long, entry ticks, quantity, exit ms, exit ticks)
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

struct TapeRow {
    Row row;
    std::string entry_signal;
};

std::vector<TapeRow> tape_trades(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_TAIL_D_OPEN_LIMIT_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, TapeRow> by_number;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        TapeRow& t = by_number[std::stoi(cell[0])];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(t.row) = tape_ms(cell[2]);
            std::get<1>(t.row) = cell[1] == "Entry long";
            std::get<2>(t.row) = ticks(price);
            std::get<3>(t.row) = std::llround(std::stod(cell[5]));
            t.entry_signal = cell[3];
        } else {
            std::get<4>(t.row) = tape_ms(cell[2]);
            std::get<5>(t.row) = ticks(price);
        }
    }
    std::vector<TapeRow> out;
    for (const auto& [number, t] : by_number) out.push_back(t);
    return out;
}

// The probe's body (tests/fixtures/coof_open_limit/td-fp17-coof-open/strategy.pine).
class OpenLimitHost final : public source::PineStrategyHost {
public:
    OpenLimitHost() {
        attach_pine_execution_adapter();
        set_syminfo_session("0915-1530");
        set_syminfo_timezone("Asia/Kolkata");
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.slippage = 2;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 1.0);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar& bar) override {
        // The bar's exchange-local date and weekday.
        const std::int64_t local_day = (current_bar_.timestamp / 60'000 + kIst) / 1440;
        const auto day = [local_day](unsigned m, unsigned d) {
            return local_day == days_from_civil(2025, m, d);
        };
        const bool friday = (local_day + 4) % 7 == 5;  // 1970-01-01 was a Thursday
        const auto enter = [this](const char* id, bool is_long) {
            strategy_entry(id, is_long, kNaN, kNaN, kNaN, id);
            active_ = id;
        };
        if (day(6, 2)) enter("LA", true);
        if (day(6, 9)) enter("SB", false);
        if (day(6, 16)) enter("LC", true);
        if (day(6, 23)) enter("SD", false);
        if (day(6, 30)) enter("LE", true);
        if (day(7, 7)) enter("LG", true);
        if (day(7, 14)) enter("LF", true);
        const double units = signed_position_size();
        const double o = bar.open;
        const double t = kTick;
        if (units > 0.0 && active_ == "LA")
            strategy_exit("LA-X", "LA", o - 20 * t, o - 8000 * t);
        if (units < 0.0 && active_ == "SB")
            strategy_exit("SB-X", "SB", o + 20 * t, o + 8000 * t);
        if (units > 0.0 && active_ == "LC") strategy_close("LC", "LC close");
        if (units < 0.0 && active_ == "SD") strategy_close("SD", "SD close");
        if (units > 0.0 && active_ == "LE") strategy_exit("LE-X", "LE", o - 20 * t, kNaN);
        if (units > 0.0 && active_ == "LG")
            strategy_exit("LG-X", "LG", o + 1 * t, o - 8000 * t);
        if (units > 0.0 && active_ == "LF")
            strategy_exit("LF-X", "LF", o + 10 * t, o - 8000 * t);
        if (friday && units != 0.0) strategy_close("", "cleanup", kNaN, kNaN, false);
    }

protected:
    // var string active: a fill's recalculation starts from the bar's
    // committed value and leaves it as it was.
    void snapshot_script_state() override { saved_ = active_; }
    void restore_script_state() override { active_ = saved_; }
    void commit_script_state() override { saved_ = active_; }

private:
    std::string active_;
    std::string saved_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kNifty1d) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

void show(const char* tag, const Row& r) {
    std::printf("    %-6s %lld %s @%lld q=%lld -> %lld @%lld\n", tag,
                static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                std::get<5>(r));
}

}  // namespace

int main() {
    std::printf("-- td-fp17-coof-open\n");
    const std::vector<Bar> bars = feed();
    const std::vector<TapeRow> tape = tape_trades("td-fp17-coof-open");
    CHECK(tape.size() == 7);
    OpenLimitHost host;
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "1D", false);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == static_cast<int>(tape.size()));
    for (int i = 0; i < host.trade_count() && i < static_cast<int>(tape.size()); ++i) {
        const Trade& t = host.get_trade(i);
        const Row engine{t.entry_time, t.is_long, ticks(t.entry_price), std::llround(t.qty),
                         t.exit_time, ticks(t.exit_price)};
        const TapeRow& tv = tape[static_cast<std::size_t>(i)];
        CHECK(engine == tv.row);
        if (engine != tv.row) {
            std::printf("    cell %s\n", tv.entry_signal.c_str());
            show("tape", tv.row);
            show("engine", engine);
        }
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
