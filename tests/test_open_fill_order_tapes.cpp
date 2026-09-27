/*
 * test_open_fill_order_tapes.cpp — lane R1-CONSOLIDATE, rule R-A (lane
 * W8A-SIGSTATE-1's pinned patch).
 *
 * At one opening price TradingView fills every MARKET order first, entries
 * and exits alike (a strategy.close_all queued at the previous close among
 * them), then the buy LIMIT entries the open has already reached, lowest
 * limit first -- whether a limit was placed at the previous close or has
 * rested since an earlier bar; neither the order they were placed in nor
 * their ids decide. A limit filled that way keeps the quantity it was placed
 * with. The native matcher breaks a same-point tie by queue order, so the
 * engine filled them in placement order: a safety-order limit resting beside
 * a take-profit close_all opened and was closed by it at once, and limits
 * placed in descending order filled in that order. The scraped
 * job-2712-3commas-3commas-gold-vault-long (BINANCE:ETHUSDT.P 1D) and
 * 3commas-3commas-silicon-vault-long-intc-dca-strategy keep their DCA limits
 * resting beside the take-profit close_all.
 *
 * Two lab tv --no-note exports (tests/fixtures/open_fill_order):
 *
 *   w8a-dca-open (lane W8A-SIGSTATE-1, BINANCE:ETHUSDT.P 15, pyramiding=3, a
 *   cash default, an explicit qty on every call; a byte-identical copy of
 *   W8A's tests/fixtures/pyramiding_open_order tape), cells placed at one
 *   close and filled at the next open:
 *     E1  flat, buy limits B (+1.0 %), A (+3.0 %), C (+2.0 %) above the
 *         close: they fill as B, C, A;
 *     E2a a limit L1, then strategy.close_all: the close fills first and L1
 *         opens after it, held to the cleanup;
 *     E2b the control: strategy.close_all, then L2.
 *   Its cells E3, E4a and E4b are W8A's rule R-B (a pending LIMIT entry does
 *   not count against pyramiding), which this tree does not carry; they are
 *   read off TradingView's rows only.
 *
 *   r1c-ra-prior-limit (this lane, NYSE:F 1D): each cell rests a buy limit
 *   from day d0 inside the gap between d1's low and d2's open, places market
 *   orders at d1's close, and is flattened at d2's close:
 *     E   then a same-side market entry: the market fills first;
 *     X   in position, then strategy.close_all: the close fills first, the
 *         limit opens after it and is held to the cleanup;
 *     O   then an opposite market entry: the short opens first, and the
 *         limit closes its own 100 of it -- no reversal;
 *     M, N and P are the opposite-pair family (a flat buy and sell market
 *         pair, with and without the resting limit): TradingView fills the
 *         pair first there too, as one transaction admitted at its gross
 *         (M's 55 % pair drops its later call). The pair's route is its own
 *         rule, left to it here: those cells are read off TradingView's rows
 *         only.
 *
 * Every other cell replays through the Pine adapter under the configuration
 * the probe's generated constructor declares, over the lane feeds embedded
 * beside the tapes, and must be TradingView's trade for trade, in its order:
 * entry id, entry time, side, price and quantity, exit time and price.
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

#ifndef PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR
#error "PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR must name tests/fixtures/open_fill_order"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/open_fill_order/eth15_bars.inc"
#include "fixtures/open_fill_order/f1d_bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;  // both charts' price tick

// One trade as both sides report it, prices in ticks and quantity in lots:
// (entry id, entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::string, std::int64_t, bool, long long, long long, std::int64_t,
                       long long>;

long long ticks(double price) { return std::llround(price / kTick); }
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

// 2025-04-<day> <hour>:<minute> UTC, w8a-dca-open's timestamp("UTC", ...) cells.
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * 60'000;
}

// 00:00 UTC of a NYSE:F session day (its daily bar is stamped at the open).
std::int64_t day_ms(int y, unsigned m, unsigned d) { return days_from_civil(y, m, d) * 86'400'000; }

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 24
             + h - 8) * 60 + mi) * 60'000;
}

struct Tape {
    std::vector<Row> trades;         // in trade order
    std::vector<std::string> exit;   // each trade's exit signal
};

Tape tape_trades(const std::string& tape, double lot) {
    std::ifstream in(std::string(PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> exit_signal;
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
            CHECK(on_grid(qty, lot));
            std::get<0>(row) = cell[3];
            std::get<1>(row) = tape_ms(cell[2]);
            std::get<2>(row) = cell[1] == "Entry long";
            std::get<3>(row) = ticks(price);
            std::get<4>(row) = std::llround(qty / lot);
        } else {
            std::get<5>(row) = tape_ms(cell[2]);
            std::get<6>(row) = ticks(price);
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.exit.push_back(exit_signal[number]);
    }
    return out;
}

// w8a-dca-open as its generated TU lowers it (fixtures/.../strategy.pine).
class DcaOpenHost final : public source::PineStrategyHost {
public:
    DcaOpenHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::CASH);
        cfg.default_qty_value = 100.0;
        cfg.pyramiding = 3;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 0.0001);
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

// r1c-ra-prior-limit as its generated TU lowers it (fixtures/.../strategy.pine).
class PriorLimitHost final : public source::PineStrategyHost {
public:
    PriorLimitHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 10000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 2;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const auto on = [&](int y, unsigned m, unsigned d) {
            return t >= day_ms(y, m, d) && t < day_ms(y, m, d) + 86'400'000;
        };
        const auto entry = [&](const char* id, bool is_long, double qty, double limit) {
            strategy_entry(id, is_long, limit, kNaN, qty, "", "", 0, -1);
        };
        const auto cleanup = [&](const std::string& tag) {
            strategy_cancel_all();
            strategy_close("", tag + " cleanup", kNaN, kNaN, false);
        };
        if (on(2024, 12, 13)) {
            entry("P-PAIR-S", false, 150, kNaN);
            entry("P-PAIR-L", true, 150, kNaN);
        }
        if (on(2024, 12, 16)) cleanup("P");
        if (on(2025, 1, 30)) entry("M-REST-L", true, 100, 9.85);
        if (on(2025, 1, 31)) {
            entry("M-PAIR-S", false, 550, kNaN);
            entry("M-PAIR-L", true, 550, kNaN);
        }
        if (on(2025, 2, 3)) cleanup("M");
        if (on(2025, 2, 4)) entry("E-REST-L", true, 100, 9.70);
        if (on(2025, 2, 5)) entry("E-MKT-L", true, 200, kNaN);
        if (on(2025, 2, 6)) cleanup("E");
        if (on(2025, 3, 31)) entry("X-HOLD", true, 100, kNaN);
        if (on(2025, 4, 2)) entry("X-REST-L", true, 200, 9.42);
        if (on(2025, 4, 3)) strategy_close("", "X close_all", kNaN, kNaN, false);
        if (on(2025, 4, 4)) cleanup("X");
        if (on(2025, 7, 2)) entry("N-REST-L", true, 100, 11.68);
        if (on(2025, 7, 3)) {
            entry("N-PAIR-S", false, 150, kNaN);
            entry("N-PAIR-L", true, 150, kNaN);
        }
        if (on(2025, 7, 7)) cleanup("N");
        if (on(2025, 7, 14)) entry("O-REST-L", true, 100, 11.44);
        if (on(2025, 7, 15)) entry("O-MKT-S", false, 200, kNaN);
        if (on(2025, 7, 16)) cleanup("O");
    }
};

template <std::size_t N>
std::vector<Bar> bars_of(const FeedBar (&rows)[N]) {
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

std::vector<Row> trades_of(source::PineStrategyHost& host, const std::vector<Bar>& bars,
                           const char* tf, std::int64_t start_ms, double lot,
                           std::string& error) {
    host.set_trade_start_time(start_ms);
    host.run(bars.data(), static_cast<int>(bars.size()), tf, tf, false);
    error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, lot));
        out.emplace_back(t.entry_id, t.entry_time, t.is_long, ticks(t.entry_price),
                         std::llround(t.qty / lot), t.exit_time, ticks(t.exit_price));
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %-9s %lld %s @%lld ticks q=%lld -> %lld @%lld ticks\n", tag,
                    std::get<0>(r).c_str(), static_cast<long long>(std::get<1>(r)),
                    std::get<2>(r) ? "long" : "short", std::get<3>(r), std::get<4>(r),
                    static_cast<long long>(std::get<5>(r)), std::get<6>(r));
}

// The rows entered inside [from, to), in trade order.
std::vector<Row> cell(const std::vector<Row>& rows, std::int64_t from, std::int64_t to) {
    std::vector<Row> out;
    for (const Row& r : rows)
        if (std::get<1>(r) >= from && std::get<1>(r) < to) out.push_back(r);
    return out;
}

std::vector<std::string> ids(const std::vector<Row>& rows) {
    std::vector<std::string> out;
    for (const Row& r : rows) out.push_back(std::get<0>(r));
    return out;
}

int replayed = 0, matched = 0;

void same_cell(const char* name, const std::vector<Row>& engine, const std::vector<Row>& tape,
               std::int64_t from, std::int64_t to) {
    const auto want = cell(tape, from, to);
    const auto got = cell(engine, from, to);
    std::printf("-- cell %s: %zu TradingView trade(s)\n", name, want.size());
    CHECK(!want.empty());
    CHECK(got == want);
    ++replayed;
    if (got == want) {
        ++matched;
    } else {
        show("tape", want);
        show("engine", got);
    }
}

}  // namespace

int main() {
    using V = std::vector<std::string>;

    // w8a-dca-open: cells E1, E2a and E2b.
    const Tape dca = tape_trades("w8a-dca-open", 0.0001);
    CHECK(dca.trades.size() == 18);
    {
        DcaOpenHost host;
        std::string error;
        // The bar before the first order's bar (E1, placed at the 01:00 close).
        const auto engine = trades_of(host, bars_of(kEth15), "15", at(15, 0, 45), 0.0001, error);
        CHECK(error.empty());
        same_cell("w8a-dca-open E1", engine, dca.trades, at(15, 1, 0), at(15, 3, 0));
        same_cell("w8a-dca-open E2a", engine, dca.trades, at(15, 5, 0), at(15, 8, 0));
        same_cell("w8a-dca-open E2b", engine, dca.trades, at(15, 10, 0), at(15, 13, 0));
    }

    // r1c-ra-prior-limit: cells E, X and O.
    const Tape prior = tape_trades("r1c-ra-prior-limit", 1.0);
    CHECK(prior.trades.size() == 13);
    {
        PriorLimitHost host;
        std::string error;
        const auto engine = trades_of(host, bars_of(kF1d), "1D", kF1d[0].ts, 1.0, error);
        CHECK(error.empty());
        same_cell("r1c-ra-prior-limit E", engine, prior.trades, day_ms(2025, 2, 4),
                  day_ms(2025, 2, 8));
        same_cell("r1c-ra-prior-limit X", engine, prior.trades, day_ms(2025, 3, 31),
                  day_ms(2025, 4, 8));
        same_cell("r1c-ra-prior-limit O", engine, prior.trades, day_ms(2025, 7, 14),
                  day_ms(2025, 7, 18));
    }
    std::printf("-- %d of %d cells replayed trade for trade\n", matched, replayed);

    // The rule, read off TradingView's rows.
    std::printf("-- the rule, on the tapes\n");
    // E1: one opening price, lowest limit first -- B (+1 %), C (+2 %),
    // A (+3 %) -- not the call order B, A, C.
    const auto e1 = cell(dca.trades, at(15, 1, 0), at(15, 3, 0));
    CHECK((ids(e1) == V{"B", "C", "A"}));
    for (const Row& r : e1)
        CHECK(std::get<1>(r) == at(15, 1, 15) && std::get<3>(r) == std::get<3>(e1[0]));
    // E2a: the close_all placed after L1 fills first; L1 opens after it at
    // the same price and is held to the cleanup.
    const auto e2a = cell(dca.trades, at(15, 5, 0), at(15, 8, 0));
    CHECK((ids(e2a) == V{"X1", "L1"}));
    if (e2a.size() == 2) {
        CHECK(std::get<5>(e2a[0]) == std::get<1>(e2a[1])
              && std::get<6>(e2a[0]) == std::get<3>(e2a[1]));
        CHECK(std::get<5>(e2a[1]) == at(15, 8, 15));
    }
    // E4b (W8A's R-B admits HM): the market HM, placed after HL, fills first.
    CHECK((ids(cell(dca.trades, at(16, 4, 0), at(16, 6, 0))) == V{"HM", "HL"}));

    // r1c-ra-prior-limit: the limit that rested since d0 fills after the
    // market orders d1's close placed, with the quantity it was placed with.
    const auto ce = cell(prior.trades, day_ms(2025, 2, 4), day_ms(2025, 2, 8));
    CHECK((ids(ce) == V{"E-MKT-L", "E-REST-L"}));
    const auto cx = cell(prior.trades, day_ms(2025, 3, 31), day_ms(2025, 4, 8));
    CHECK((ids(cx) == V{"X-HOLD", "X-REST-L"}));
    if (cx.size() == 2) {
        CHECK(std::get<5>(cx[0]) == std::get<1>(cx[1]));  // the close_all, then the limit
        CHECK(std::get<5>(cx[1]) > std::get<1>(cx[1]));   // held to the cleanup
    }
    const auto co = cell(prior.trades, day_ms(2025, 7, 14), day_ms(2025, 7, 18));
    CHECK((ids(co) == V{"O-MKT-S", "O-MKT-S"}));
    if (co.size() == 2) {
        CHECK(!std::get<2>(co[0]) && std::get<4>(co[0]) == 100 && std::get<4>(co[1]) == 100);
        CHECK(prior.exit[11] == "O-REST-L");  // 100 of the 200 short closed, no long opened
    }
    // M and N: the flat pair fills before the resting limit; M's later call
    // (a 55 % pair, 1100 x 10.08 over 10 000) is dropped at its gross.
    const auto cm = cell(prior.trades, day_ms(2025, 1, 30), day_ms(2025, 2, 5));
    CHECK((ids(cm) == V{"M-PAIR-S", "M-PAIR-S"}));
    if (cm.size() == 2) CHECK(prior.exit[2] == "M-REST-L" && std::get<4>(cm[0]) == 100);
    const auto cn = cell(prior.trades, day_ms(2025, 7, 2), day_ms(2025, 7, 9));
    CHECK((ids(cn) == V{"N-PAIR-L", "N-PAIR-L", "N-REST-L"}));
    const auto cp = cell(prior.trades, day_ms(2024, 12, 13), day_ms(2024, 12, 18));
    CHECK((ids(cp) == V{"P-PAIR-L", "P-PAIR-L"}));

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
