/*
 * test_pyramiding_open_order_tape.cpp — lane W8A-SIGSTATE-1, rule R-B.
 *
 * R-B. TradingView checks strategy.entry's pyramiding cap when the entry is
 * called, counting the open trades and the pending same-side MARKET entries
 * of other ids. A pending LIMIT entry never counts, and a LIMIT entry the
 * call admitted fills however many trades are open by then. The adapter
 * counted every pending same-side entry at the call and, under a non-fixed
 * default quantity, refused a priced entry at its fill once the open trades
 * reached the cap.
 *
 * The scraped job-2712-3commas-3commas-gold-vault-long (BINANCE:BTCUSDT and
 * BINANCE:ETHUSDT.P 1D) rests its safety-order limits at the cap:
 * TradingView books five lots at pyramiding=4 where the engine refused the
 * fifth.
 *
 * One lab tv export (tests/fixtures/pyramiding_open_order/w8a-dca-open,
 * BINANCE:ETHUSDT.P 15, pyramiding=3, a cash default, an explicit qty on
 * every call) replayed through the Pine adapter under the configuration its
 * generated constructor declares, over the corpus 15m bars of bars.inc
 * (2025-04-14 .. 04-16 UTC): every trade but E2a's must be the engine's --
 * entry id, entry and exit time, side, price and quantity -- and the rule is
 * read off TradingView's rows:
 *
 *   E3  one lot open, limits P1, P2, P3: all three fill, four lots at
 *       pyramiding=3;
 *   E4a the control, two lots open, a market GM, then a limit GL: GL is
 *       ignored (the pending MARKET entry counts);
 *   E4b two lots open, a limit HL, then a market HM: both fill.
 *
 * The order the trades of one opening fill in, and E2a's close_all and limit
 * at one opening price, are a second rule the tape also pins.
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

#ifndef PINEFORGE_PYRAMIDING_OPEN_ORDER_FIXTURE_DIR
#error "PINEFORGE_PYRAMIDING_OPEN_ORDER_FIXTURE_DIR must name tests/fixtures/pyramiding_open_order"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/pyramiding_open_order/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry id, entry ms, long, entry price, quantity, exit ms, exit price).
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
    std::ifstream in(std::string(PINEFORGE_PYRAMIDING_OPEN_ORDER_FIXTURE_DIR)
                     + "/w8a-dca-open/tv_trades.csv");
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
            CHECK(cell[3] == "Close position order");
            std::get<5>(row) = tape_ms(cell[2]);
            std::get<6>(row) = ticks(price);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, row] : by_number) out.push_back(row);
    return out;
}

// w8a-dca-open as its generated TU lowers it (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::CASH);
        cfg.default_qty_value = 100.0;
        cfg.pyramiding = 3;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double close = current_bar_.close;
        const auto entry = [&](const char* id, double qty, double limit) {
            strategy_entry(id, true, limit, kNaN, qty, "", "", 0, -1);
        };
        const auto close_all = [&] { strategy_close("", "", kNaN, kNaN, false); };
        const auto cleanup = [&] {
            strategy_cancel_all();
            close_all();
        };
        if (t == at(15, 1, 0)) {
            entry("B", 1, close * 1.010);
            entry("A", 2, close * 1.030);
            entry("C", 3, close * 1.020);
        }
        if (t == at(15, 3, 0)) cleanup();
        if (t == at(15, 5, 0)) entry("X1", 1, kNaN);
        if (t == at(15, 6, 0)) {
            entry("L1", 2, close * 1.02);
            close_all();
        }
        if (t == at(15, 8, 0)) cleanup();
        if (t == at(15, 10, 0)) entry("X2", 1, kNaN);
        if (t == at(15, 11, 0)) {
            close_all();
            entry("L2", 2, close * 1.02);
        }
        if (t == at(15, 13, 0)) cleanup();
        if (t == at(15, 15, 0)) entry("F1", 1, kNaN);
        if (t == at(15, 16, 0)) {
            entry("P1", 1, close * 1.01);
            entry("P2", 2, close * 1.02);
            entry("P3", 3, close * 1.03);
        }
        if (t == at(15, 18, 0)) cleanup();
        if (t == at(15, 20, 0)) entry("G1", 1, kNaN);
        if (t == at(15, 21, 0)) entry("G2", 1, kNaN);
        if (t == at(15, 22, 0)) {
            entry("GM", 1, kNaN);
            entry("GL", 2, close * 1.02);
        }
        if (t == at(16, 0, 0)) cleanup();
        if (t == at(16, 2, 0)) entry("H1", 1, kNaN);
        if (t == at(16, 3, 0)) entry("H2", 1, kNaN);
        if (t == at(16, 4, 0)) {
            entry("HL", 2, close * 1.02);
            entry("HM", 1, kNaN);
        }
        if (t == at(16, 6, 0)) cleanup();
    }
};

std::vector<Row> run(std::string& error) {
    ProbeHost host;
    // The bar before the first order's bar (E1, placed at the 01:00 close).
    host.set_trade_start_time(at(15, 0, 45));
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15) {
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
        out.emplace_back(t.entry_id, t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                         t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %-3s %lld %s @%lld ticks q=%lld lots -> %lld @%lld ticks\n", tag,
                    std::get<0>(r).c_str(), static_cast<long long>(std::get<1>(r)),
                    std::get<2>(r) ? "long" : "short", std::get<3>(r), std::get<4>(r),
                    static_cast<long long>(std::get<5>(r)), std::get<6>(r));
}

// The ids of the rows entered inside [from, to), in trade order.
std::vector<std::string> ids(const std::vector<Row>& rows, std::int64_t from, std::int64_t to) {
    std::vector<std::string> out;
    for (const Row& r : rows)
        if (std::get<1>(r) >= from && std::get<1>(r) < to) out.push_back(std::get<0>(r));
    return out;
}

}  // namespace

int main() {
    const std::vector<Row> tape = tape_trades();
    CHECK(tape.size() == 18);

    std::printf("-- w8a-dca-open\n");
    std::string error;
    const std::vector<Row> engine = run(error);
    CHECK(error.empty());
    // Every trade but E2a's (05:00 .. 08:00), in any order.
    const auto outside_e2a = [](const std::vector<Row>& rows) {
        std::vector<Row> out;
        for (const Row& r : rows)
            if (std::get<1>(r) < at(15, 5, 0) || std::get<1>(r) >= at(15, 8, 0)) out.push_back(r);
        std::sort(out.begin(), out.end());
        return out;
    };
    CHECK(outside_e2a(engine) == outside_e2a(tape));
    if (outside_e2a(engine) != outside_e2a(tape)) {
        show("tape", outside_e2a(tape));
        show("engine", outside_e2a(engine));
    }

    // The rule, read off TradingView's rows.
    std::printf("-- the rule, on the tape\n");
    using V = std::vector<std::string>;
    const auto sorted = [](V v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    // E3: one lot open, the three limits all fill -- four lots at
    // pyramiding=3, none refused at its fill.
    CHECK((ids(tape, at(15, 15, 0), at(15, 18, 0)) == V{"F1", "P1", "P2", "P3"}));
    // E4a: two lots open, the pending market GM counts and GL is ignored.
    CHECK((ids(tape, at(15, 20, 0), at(16, 0, 0)) == V{"G1", "G2", "GM"}));
    // E4b: the pending limit HL does not count, so HM is accepted.
    CHECK((sorted(ids(tape, at(16, 2, 0), at(16, 6, 0))) == V{"H1", "H2", "HL", "HM"}));

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
