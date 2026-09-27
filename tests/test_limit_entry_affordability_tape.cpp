/*
 * test_limit_entry_affordability_tape.cpp — lane W8A-SIGSTATE-1, rule R3.
 *
 * TradingView checks a default-quantity LIMIT strategy.entry that opens from
 * flat at every call -- its placement and each re-issue -- against the
 * calling bar's close: a call whose quantity costs more than the equity
 * there is rejected, and it takes the resting order of that id with it,
 * however affordable the order's own level is. A call it admitted is not
 * re-checked on later bars, and its fill is still refused when the fill's
 * own notional exceeds the equity. The adapter left LIMIT entries out of
 * its placement check, so a re-issue rejected by TradingView kept its
 * resting order alive.
 *
 * The scraped algotorma-algo-torma-orb-strategy (BINANCE:BTCUSDT 15, strong)
 * re-issues its breakout limit every bar while armed; the last re-issue
 * (2026-01-14 23:45, close 96951.78 over an equity of 96846.71) was
 * rejected in TradingView, and the engine filled the resting order on 01-15
 * 03:15 -- its one engine-only trade.
 *
 * One lab tv export (tests/fixtures/limit_entry_affordability/
 * w8a-btc15-limafford, BINANCE:BTCUSDT 15, equity 95000, a default fixed
 * quantity of 1, margin 100 %) replayed through the Pine adapter under the
 * configuration its generated constructor declares, over the lane's 15m bars
 * of bars.inc (2025-04-25 00:00 .. 04-30 12:00 UTC): every trade must be the
 * engine's -- entry signal, entry and exit time, side, price and quantity --
 * and the rule is read off TradingView's rows and the bars:
 *
 *   E  a sell limit at 95250, placed once on an affordable close: its level
 *      costs more than the equity, and it never fills;
 *   A  a buy limit at 94640 re-issued every bar 18:00 .. 19:45, the last
 *      calls on closes above the equity: it never fills, though the market
 *      reaches it at 21:45;
 *   C  a buy limit at 94680 placed once on an affordable close; later closes
 *      above the equity, with no re-issue, leave it alone: it fills;
 *   B  the control: every call affordable, it fills.
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

#ifndef PINEFORGE_LIMIT_ENTRY_AFFORDABILITY_FIXTURE_DIR
#error "PINEFORGE_LIMIT_ENTRY_AFFORDABILITY_FIXTURE_DIR must name tests/fixtures/limit_entry_affordability"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/limit_entry_affordability/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:BTCUSDT quantity step and price tick.
constexpr double kLot = 0.00001;
constexpr double kTick = 0.01;
constexpr double kEquity = 95000.0;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry signal, entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::string, std::int64_t, bool, long long, long long, std::int64_t,
                       long long>;

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

// 2025-04-<day> <hour>:<minute> UTC, the probe's timestamp("UTC", ...) cells.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 24
             + h - 8) * 60 + mi) * 60'000;
}

std::vector<Row> tape_trades() {
    std::ifstream in(std::string(PINEFORGE_LIMIT_ENTRY_AFFORDABILITY_FIXTURE_DIR)
                     + "/w8a-btc15-limafford/tv_trades.csv");
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
        const double price = std::stod(cell[4]);
        CHECK(on_grid(price, kTick));
        if (cell[1].rfind("Entry", 0) == 0) {
            const double qty = std::stod(cell[5]);
            CHECK(on_grid(qty, kLot));
            std::get<0>(row) = cell[3];
            std::get<1>(row) = tape_ms(cell[2]);
            std::get<2>(row) = cell[1] == "Entry long";
            std::get<3>(row) = ticks(price);
            std::get<4>(row) = lots(qty);
        } else {
            std::get<5>(row) = tape_ms(cell[2]);
            std::get<6>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

// w8a-btc15-limafford as its generated TU lowers it (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = kEquity;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const bool unaff = current_bar_.close > current_equity() + open_profit(current_bar_.close);
        const auto entry = [&](const char* id, bool is_long, double limit) {
            strategy_entry(id, is_long, limit, kNaN, kNaN,
                           std::string(id) + (unaff ? "-u" : "-a"), "", 0, -1);
        };
        if (t == at(25, 12, 15)) entry("E", false, 95250.0);
        if (t >= at(25, 18, 0) && t <= at(25, 19, 45)) entry("A", true, 94640.0);
        if (t == at(29, 14, 15)) entry("C", true, 94680.0);
        if (t == at(29, 22, 0) || t == at(29, 22, 15)) entry("B", true, 93980.01);
        if (t == at(25, 14, 0)) strategy_cancel("E");
        if (t == at(25, 21, 45)) strategy_cancel("A");
        if (t == at(29, 17, 0)) strategy_cancel("C");
        if (t == at(29, 23, 0)) strategy_cancel("B");
        if (signed_position_size() != 0.0) strategy_close("", "x", kNaN, kNaN, false);
    }
};

std::vector<Row> run(std::string& error) {
    ProbeHost host;
    // The bar before the first order's bar (E, placed at the 12:15 close).
    host.set_trade_start_time(at(25, 12, 0));
    std::vector<Bar> bars;
    for (const FeedBar& row : kBtc15) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        // TradingView's Signal column shows the entry's comment.
        out.emplace_back(t.entry_comment, t.entry_time, t.is_long, ticks(t.entry_price),
                         lots(t.qty), t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %-4s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    std::get<0>(r).c_str(), static_cast<long long>(std::get<1>(r)),
                    std::get<2>(r) ? "long" : "short", std::get<3>(r), std::get<4>(r),
                    static_cast<long long>(std::get<5>(r)), std::get<6>(r));
}

const FeedBar* bar_at(std::int64_t ts) {
    for (const FeedBar& row : kBtc15)
        if (row.ts == ts) return &row;
    return nullptr;
}

}  // namespace

int main() {
    const std::vector<Row> tape = tape_trades();
    CHECK(tape.size() == 2);

    std::printf("-- w8a-btc15-limafford\n");
    std::string error;
    const std::vector<Row> engine = run(error);
    CHECK(error.empty());
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }

    // The rule, read off TradingView's rows and the bars.
    std::printf("-- the rule, on the tape\n");
    // Only C and B fill, each at its own limit on its touch bar.
    CHECK(tape.size() == 2 && std::get<0>(tape[0]) == "C-a" && std::get<0>(tape[1]) == "B-a");
    if (tape.size() == 2) {
        CHECK(std::get<1>(tape[0]) == at(29, 17, 0) && std::get<3>(tape[0]) == ticks(94680.0));
        CHECK(std::get<1>(tape[1]) == at(29, 23, 0) && std::get<3>(tape[1]) == ticks(93980.01));
    }
    // A: affordable at its first call, not at its last; the market reaches
    // its level at 21:45 and TradingView books nothing.
    const FeedBar* a0 = bar_at(at(25, 18, 0));
    const FeedBar* a1 = bar_at(at(25, 19, 45));
    const FeedBar* touch = bar_at(at(25, 21, 45));
    CHECK(a0 && a0->close <= kEquity);
    CHECK(a1 && a1->close > kEquity);
    CHECK(touch && touch->low <= 94640.0 && touch->high >= 94640.0);
    // C: placed on an affordable close; closes above the equity follow
    // before its touch, with no re-issue, and it still fills.
    const FeedBar* c0 = bar_at(at(29, 14, 15));
    CHECK(c0 && c0->close <= kEquity);
    bool later_unaffordable = false;
    for (const FeedBar& row : kBtc15)
        if (row.ts > at(29, 14, 15) && row.ts < at(29, 17, 0) && row.close > kEquity)
            later_unaffordable = true;
    CHECK(later_unaffordable);
    // E: placed on an affordable close; its level costs more than the equity
    // and the market reaches it, but it never fills (the fill-time check).
    const FeedBar* e0 = bar_at(at(25, 12, 15));
    CHECK(e0 && e0->close <= kEquity && 95250.0 > kEquity);
    bool e_reached = false;
    for (const FeedBar& row : kBtc15)
        if (row.ts > at(25, 12, 15) && row.ts <= at(25, 14, 0) && row.high >= 95250.0)
            e_reached = true;
    CHECK(e_reached);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
