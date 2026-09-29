/*
 * test_explicit_market_admission_tapes.cpp -- R5 lane TAIL-C.
 *
 * An explicit-quantity MARKET entry placed from flat is admitted at its
 * placement against strategy.equity. TradingView costs a BUY at its signal
 * close's tick plus the slippage ticks its fill will carry, and a SELL at the
 * close's tick, never lowered by the slippage. The engine costed the buy at
 * the plain close, so an all-in long whose slipped fill came in under that
 * close opened: the extra longs of the blitz-locked MACD pullback sniper on
 * OANDA:EURUSD 15 and NYSE:F 15.
 *
 * The lane's own synthetic script (tests/fixtures/explicit_market_admission,
 * three `lab tv --no-note` exports on NYSE:F 15, 2025-04-01 .. 2025-10-01,
 * differing only in strategy()): one cell per session, placed on its last bar
 * (15:45 New York) from flat, LONG on even sessions and SHORT on odd ones,
 * qty = floor(strategy.equity / close), filled at the next session's open and
 * flattened by close_all on that session's 10:30 bar.
 *   -a  slippage 2, no commission: no long cell ever opens;
 *   -b  slippage 0, commission 0.05 %: no commission term (control);
 *   -c  slippage 0, no commission (control).
 * Each row replays the tape through the Pine adapter over the f-15 lane chart
 * feed (tests/fixtures/slipped_short/bars.inc, 2025-04-01 .. 2025-06-30) and
 * requires every trade the tape closes inside its window to be the engine's:
 * entry and exit time, side, price and quantity. -a's window ends on
 * 2025-05-16: on 2025-05-19 its short takes a second one-unit margin call at
 * the 10:30 bar's close that the engine does not (the slipped_short README's
 * "Open": one-unit calls at two slippage ticks and more).
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

#ifndef PINEFORGE_EXPLICIT_ADMISSION_FIXTURE_DIR
#error "PINEFORGE_EXPLICIT_ADMISSION_FIXTURE_DIR must name tests/fixtures/explicit_market_admission"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/slipped_short/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// NYSE:F's quantity step and price tick (lane f-15).
constexpr double kLot = 1.0;
constexpr double kTick = 0.01;
// New York is UTC-4 over the whole replay (EDT, 2025-03-09 .. 11-02).
constexpr std::int64_t kNewYorkOffsetMinutes = -4 * 60;

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

std::vector<Row> tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_EXPLICIT_ADMISSION_FIXTURE_DIR) + "/" + tape
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

struct Variant {
    const char* tape;
    int slippage;
    double commission_percent;
    std::int64_t end_ms;  // compare the trades closed before it (0: the last bar)
    std::size_t closed;   // tape trades closed before it
};

// The probe's body (tests/fixtures/explicit_market_admission/<slug>/strategy.pine).
class AdmissionHost final : public source::PineStrategyHost {
public:
    explicit AdmissionHost(const Variant& v) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.initial_capital = 10000.0;
        c.margin_long = 100.0;
        c.margin_short = 100.0;
        c.slippage = v.slippage;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = v.commission_percent;
        c.pyramiding = 1;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kLot);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t local = (current_bar_.timestamp / 60'000 + kNewYorkOffsetMinutes)
            % 1440;
        const bool last_bar = local == 15 * 60 + 45;
        const bool flat_bar = local == 10 * 60 + 30;
        if (last_bar) {
            ++session_;
            if (signed_position_size() == 0.0) {
                // math.floor(strategy.equity / close)
                const double equity = current_equity() + open_profit(current_bar_.close);
                const double q = std::floor(equity / current_bar_.close);
                if (session_ % 2 == 0) strategy_entry("L", true, kNaN, kNaN, q);
                else strategy_entry("S", false, kNaN, kNaN, q);
            }
        }
        if (flat_bar && signed_position_size() != 0.0)
            strategy_close("", "X", kNaN, kNaN, false);
    }

private:
    int session_ = 0;
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
    const std::int64_t last_ms = kF15Q2[sizeof(kF15Q2) / sizeof(kF15Q2[0]) - 1].ts;
    std::vector<Bar> bars;
    for (const FeedBar& row : kF15Q2) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    const Variant variants[] = {
        {"tailc-b-admission-a-slip2", 2, 0.0, 1747612800000LL, 10},  // 2025-05-19 00:00 UTC
        {"tailc-b-admission-b-fee", 0, 0.05, 0, 45},
        {"tailc-b-admission-c-control", 0, 0.0, 0, 43},
    };
    for (const Variant& v : variants) {
        std::printf("-- %s\n", v.tape);
        const std::int64_t end_ms = v.end_ms > 0 ? v.end_ms : last_ms;
        const std::vector<Row> tape = tape_trades(v.tape, end_ms);
        CHECK(tape.size() == v.closed);
        AdmissionHost host(v);
        host.set_trade_start_time(bars.front().timestamp);
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
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
