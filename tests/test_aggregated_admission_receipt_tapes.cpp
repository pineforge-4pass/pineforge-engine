/*
 * test_aggregated_admission_receipt_tapes.cpp -- lane FIX-E1E2 (E2): on an
 * aggregated chart an admission receipt names its command in the command's
 * own bar space.
 *
 * The Pine adapter binds each entry command's admission observation to the
 * row's projection bar, the input slot the kernel presented
 * (PineExecutionAdapter::projection_bar_index). Since #289 the kernel's
 * public interval_index is the script bar, and the receipts that follow a
 * command -- the sizing revision a margin call makes of a pending
 * default-quantity entry, the terminal review of a process_orders_on_close
 * pair -- were stamped with it. On a native chart the two indices agree; on
 * an aggregated chart the script bar is smaller than the input slot, the
 * receipt appeared to precede its own command, and
 * admission::Draft::sizing_revised / reviewed stopped the run with
 * "admission receipt requires its exact earlier command". Pine v6's default
 * sizing (100 % of equity) with a commission under process_orders_on_close is
 * enough, as is a process_orders_on_close pair of fixed-quantity calls at
 * pyramiding 0.
 *
 * Each row replays TradingView's own tape of a synthetic probe written for
 * this lane (tests/fixtures/aggregated_admission_receipt, lab tv exports on
 * BINANCE:ETHUSDT.P 60; README.md names each one) through the Pine adapter, on
 * the chart's 60-minute bars and on the same two days aggregated from 15- and
 * 5-minute bars, and requires every trade the tape closes inside the replayed
 * bars -- entry and exit time, side, price in ticks, quantity in lots, net
 * profit to the cent -- to be the engine's.
 *
 * Fail-before (35db01c8): both aggregated rows of both tapes stop with the
 * receipt error; the chart rows pass.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
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

#ifndef PINEFORGE_FIX_E1E2_FIXTURE_DIR
#error "PINEFORGE_FIX_E1E2_FIXTURE_DIR must name tests/fixtures/aggregated_admission_receipt"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEth5m, kEth15m, kEth60m: BINANCE:ETHUSDT.P 2025-04-03 00:00 .. 2025-04-04
// 23:59 UTC.
#include "fixtures/aggregated_admission_receipt/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kLot = 0.0001;
constexpr std::int64_t kMinute = 60'000;

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

// timestamp("UTC", 2025, 4, day, hour, 0)
std::int64_t utc(int day, int hour) {
    return ((days_from_civil(2025, 4, static_cast<unsigned>(day)) * 24 + hour) * 60) * kMinute;
}

// The tapes print times at UTC+8.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct Row {
    std::int64_t entry_ms = 0, exit_ms = 0;
    bool is_long = false;
    long long entry_ticks = 0, exit_ticks = 0, qty_lots = 0;
    double net_pnl = 0.0;
    std::string entry_signal, exit_signal;
};

std::vector<Row> tape_rows(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_FIX_E1E2_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::set<int> closed;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 8) continue;
        const int number = std::stoi(cell[0]);
        Row& r = by_number[number];
        const double price = std::stod(cell[4]);
        if (cell[1].rfind("Entry", 0) == 0) {
            r.entry_ms = tape_ms(cell[2]);
            r.is_long = cell[1] == "Entry long";
            r.entry_ticks = ticks(price);
            r.qty_lots = lots(std::stod(cell[5]));
            r.entry_signal = cell[3];
        } else {
            r.exit_ms = tape_ms(cell[2]);
            r.exit_ticks = ticks(price);
            r.net_pnl = std::stod(cell[7]);
            r.exit_signal = cell[3];
            closed.insert(number);
        }
    }
    std::vector<Row> out;
    for (const auto& [number, r] : by_number)
        if (closed.count(number) && r.exit_ms < end_ms) out.push_back(r);
    return out;
}

// Both probes run on BINANCE:ETHUSDT.P as the lane templates declare it.
void chart_symbol(source::PineStrategyHost& host) {
    host.set_syminfo_mintick(kTick);
    host.set_syminfo_metadata("qty_step", kLot);
    host.set_syminfo_timezone("Etc/UTC");
    host.set_syminfo_session("24x7");
}

// fixtures/aggregated_admission_receipt/fe2-pooc-mc-cost-m50/strategy.pine, as
// its generated TU lowers it: a short at 100 % of equity, reversed on the bar
// that margin-calls it; the margin call books after the reversal's command, so
// it revises the pending default-quantity long.
class McHost final : public source::PineStrategyHost {
public:
    McHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        cfg.commission_type = static_cast<int>(CommissionType::PERCENT);
        cfg.commission_value = 0.04;
        cfg.margin_long = 50.0;
        configure_pine_strategy(cfg);
        chart_symbol(*this);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const auto at = [&](int d, int h) { return t == utc(d, h); };
        const auto in_cell = [&](int d, int h0, int h1) { return t >= utc(d, h0) && t < utc(d, h1); };
        if (at(3, 1) || at(3, 15) || at(4, 8))
            strategy_entry("A-S", false, kNaN, kNaN, kNaN, "A short");
        if (at(3, 8) || at(4, 1) || at(4, 15))
            strategy_entry("B-S", false, kNaN, kNaN, kNaN, "B short");
        const double size = signed_position_size();
        const bool adverse = size < 0.0 && std::fabs(size) > 1e-10
            && current_bar_.high > position_entry_price_
            && std::fabs(current_bar_.high - position_entry_price_) > 1e-10;
        const bool cell_a = in_cell(3, 2, 6) || in_cell(3, 16, 20) || in_cell(4, 9, 13);
        const bool cell_b = in_cell(3, 9, 13) || in_cell(4, 2, 6) || in_cell(4, 16, 20);
        if (adverse && cell_a && open_trade_entry_id(0) == "A-S") {
            strategy_close("A-S", "A close", kNaN, kNaN, false, 111669149715ULL);
            strategy_entry("A-L", true, kNaN, kNaN, kNaN, "A long");
        }
        if (adverse && cell_b && open_trade_entry_id(0) == "B-S")
            strategy_entry("B-L", true, kNaN, kNaN, kNaN, "B long");
        if (at(3, 6) || at(3, 13) || at(3, 20) || at(4, 6) || at(4, 13) || at(4, 20)) {
            strategy_cancel_all();
            strategy_close("", "flat", kNaN, kNaN, false);
        }
    }
};

// fixtures/aggregated_admission_receipt/fe2-pooc-pair/strategy.pine, as its
// generated TU lowers it: a flat pair of opposite fixed-quantity market calls
// under process_orders_on_close at pyramiding 0, reviewed together at the close.
class PairHost final : public source::PineStrategyHost {
public:
    PairHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        cfg.pyramiding = 0;
        configure_pine_strategy(cfg);
        chart_symbol(*this);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const bool active = t >= utc(3, 0) && t < utc(5, 0);
        const double phase = std::fmod(static_cast<double>(pine_hour(t, "UTC")), 12.0);
        const double size = signed_position_size();
        const bool flat = size == 0.0 || std::fabs(size) <= 1e-10;
        const double equity = current_equity() + open_profit(current_bar_.close);
        const double q_over = std::floor(0.8 * equity / current_bar_.close * 1000) / 1000;
        const double q_under = std::floor(0.3 * equity / current_bar_.close * 1000) / 1000;
        const auto pair = [&](bool long_first, double qty, const char* cell) {
            const std::string c(cell);
            if (long_first) {
                strategy_entry("L", true, kNaN, kNaN, qty, c + "-LF-L", "", 0, -1);
                strategy_entry("S", false, kNaN, kNaN, qty, c + "-LF-S", "", 0, -1);
            } else {
                strategy_entry("S", false, kNaN, kNaN, qty, c + "-SF-S", "", 0, -1);
                strategy_entry("L", true, kNaN, kNaN, qty, c + "-SF-L", "", 0, -1);
            }
        };
        if (active && flat && phase == 1) pair(true, q_over, "O");
        if (active && flat && phase == 4) pair(false, q_over, "O");
        if (active && flat && phase == 7) pair(true, q_under, "U");
        if (active && flat && phase == 10) pair(false, q_under, "U");
        if (phase == 3 || phase == 6 || phase == 9 || phase == 0)
            strategy_close("", "X", kNaN, kNaN, false);
    }
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

struct Path {
    const char* name;
    const char* input_tf;
    const std::vector<Bar>* bars;
};

std::string hhmm(std::int64_t ms) {
    const std::int64_t minutes = ms / kMinute;
    char text[24];
    std::snprintf(text, sizeof(text), "%02lld %02lld:%02lld",
                  static_cast<long long>((ms - utc(1, 0)) / (1440 * kMinute) + 1),
                  static_cast<long long>(minutes % 1440 / 60),
                  static_cast<long long>(minutes % 60));
    return text;
}

void show(const char* tag, const Row& r) {
    std::printf("      %-6s %s %s @%lld -> %s @%lld  %lld lots  pnl %.4f  %s/%s\n", tag,
                hhmm(r.entry_ms).c_str(), r.is_long ? "L" : "S", r.entry_ticks,
                hhmm(r.exit_ms).c_str(), r.exit_ticks, r.qty_lots, r.net_pnl,
                r.entry_signal.c_str(), r.exit_signal.c_str());
}

template <class Host>
void replay(const char* tape, const Path& path) {
    const std::vector<Bar>& bars = *path.bars;
    const std::int64_t end_ms = kEth60m[47].ts;  // 2025-04-04 23:00, the last chart bar
    const std::vector<Row> expected = tape_rows(tape, end_ms);
    std::printf("-- %s [%s]: %zu TradingView trades\n", tape, path.name, expected.size());
    CHECK(!expected.empty());

    Host host;
    host.run(bars.data(), static_cast<int>(bars.size()), path.input_tf, "60", false);
    const std::string error = host.last_error();
    if (!error.empty()) std::printf("      run error: '%s'\n", error.c_str());
    CHECK(error.empty());

    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        Row r;
        r.entry_ms = t.entry_time;
        r.exit_ms = t.exit_time;
        r.is_long = t.is_long;
        r.entry_ticks = ticks(t.entry_price);
        r.exit_ticks = ticks(t.exit_price);
        r.qty_lots = lots(t.qty);
        r.net_pnl = t.pnl;
        engine.push_back(r);
    }
    CHECK(engine.size() == expected.size());
    const std::size_t rows = std::min(engine.size(), expected.size());
    bool all = engine.size() == expected.size();
    for (std::size_t i = 0; i < rows; ++i) {
        const Row& tv = expected[i];
        const Row& e = engine[i];
        const bool ok = e.entry_ms == tv.entry_ms && e.exit_ms == tv.exit_ms
            && e.is_long == tv.is_long && e.entry_ticks == tv.entry_ticks
            && e.exit_ticks == tv.exit_ticks && e.qty_lots == tv.qty_lots
            && std::fabs(e.net_pnl - tv.net_pnl) < 0.005 + 1e-9;
        CHECK(ok);
        if (!ok) {
            all = false;
            std::printf("    row %zu\n", i + 1);
            show("tape", tv);
            show("engine", e);
        }
    }
    if (!all && engine.size() != expected.size()) {
        for (std::size_t i = rows; i < engine.size(); ++i) show("extra", engine[i]);
        for (std::size_t i = rows; i < expected.size(); ++i) show("missing", expected[i]);
    }
}

}  // namespace

int main() {
    const std::vector<Bar> m5 = feed(kEth5m);
    const std::vector<Bar> m15 = feed(kEth15m);
    const std::vector<Bar> m60 = feed(kEth60m);
    const Path paths[] = {
        {"chart 60 (control)", "60", &m60},
        {"aggregated 15 -> 60", "15", &m15},
        {"aggregated 5 -> 60", "5", &m5},
    };
    for (const Path& path : paths) replay<McHost>("fe2-pooc-mc-cost-m50", path);
    for (const Path& path : paths) replay<PairHost>("fe2-pooc-pair", path);

    std::printf("\n%s aggregated admission receipt tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
