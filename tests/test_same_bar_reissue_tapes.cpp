/*
 * test_same_bar_reissue_tapes.cpp — lane R1-CONSOLIDATE (the one R1 of lanes
 * W8A-SIGSTATE-1 and W8B-SIGSTATE-2).
 *
 * A strategy.entry called again with the same id on the same bar replaces
 * the order its first call placed. When that first order is a MARKET order
 * reversing the position, TradingView sizes the reissue against the position
 * the first order would leave: the reissue is a plain transaction of its own
 * quantity -- it closes up to that quantity of the held side and opens only
 * the remainder -- not a reversal (held side plus own quantity). It does so
 * on either side and for every quantity kind: a default percent of equity,
 * a fixed or cash default, and an explicit qty; with and without
 * process_orders_on_close. On the buy side the held short keeps its bracket;
 * on the sell side a default-size reissue voids the held long's bracket.
 *
 * The engine kept that rule only for a default percent_of_equity SELL whose
 * first call had reached the native core (ab9714be's R18 pins); it reversed
 * a buy, a fixed or cash default, an explicit qty, and every reissue whose
 * first call waited in the callback's same-bar batch (every fixed default,
 * and a variable size over a single-lot short). The scraped
 * you970716-snrz-0-7-1 calls strategy.entry twice per signal, so its
 * reversals under percent_of_equity 10 opened whole positions where
 * TradingView opened slivers (BINANCE:BTCUSDT 15, 2025-10-19 08:30 UTC: TV
 * long 0.00002, engine 0.0093).
 *
 * A single call, a LIMIT first call re-issued as a MARKET order, and a
 * strategy.cancel between the two calls keep TradingView's full reversal;
 * each is a control below.
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/same_bar_reissue, fifteen lab tv exports on
 * BINANCE:ETHUSDT.P 15: six w8a-* of lane W8A-SIGSTATE-1 and nine w8b-* of
 * lane W8B-SIGSTATE-2) through the Pine adapter under the configuration the
 * generated constructor declares for it, over the corpus 15m bars of
 * tests/fixtures/tvdef_drops (2025-04-07 .. 04-10), and requires every trade
 * the tape closes inside those bars to be the engine's: entry and exit time,
 * side, price and quantity. It also reads the rule off TradingView's own rows.
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

#ifndef PINEFORGE_SAME_BAR_REISSUE_FIXTURE_DIR
#error "PINEFORGE_SAME_BAR_REISSUE_FIXTURE_DIR must name tests/fixtures/same_bar_reissue"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// The corpus 15m BINANCE:ETHUSDT.P bars 2025-04-07 00:00 .. 04-10 12:00 UTC,
// shared with lane TVDEF-DROPS's tapes (the same chart and window).
#include "fixtures/tvdef_drops/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The bar before every tape's first entry order (the earliest is placed
// 2025-04-08 00:15 UTC); the harness trades from here, as run_strategy.py
// does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744070400000LL;  // 2025-04-08 00:00 UTC

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
    for (const FeedBar& row : kEth15) {
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

// 2025-04-<day> <hour>:<minute> UTC, the probes' timestamp("UTC", ...) cells.
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

struct Tape {
    std::vector<Row> trades;         // closed inside the replayed bars, in trade order
    std::vector<std::string> entry;  // each trade's entry signal
    std::vector<std::string> exit;   // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_SAME_BAR_REISSUE_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
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
        const auto signal = exit_signal.find(number);
        if (signal != exit_signal.end() && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entry.push_back(entry_signal[number]);
            out.exit.push_back(signal->second);
        }
    }
    return out;
}

// Lane W8A-SIGSTATE-1's probes (w8a-*), as their generated TUs lower them
// (fixtures/.../strategy.pine): `reissue` is the double-call tape, false its
// single-call control.
enum class Probe { P10, Scope, Kinds };

class ReissueHost final : public source::PineStrategyHost {
public:
    ReissueHost(Probe probe, bool reissue, const source::PineStrategyConfig& config)
        : probe_(probe), reissue_(reissue) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::P10: p10(t); break;
        case Probe::Scope: scope(t); break;
        case Probe::Kinds: kinds(t); break;
        }
    }

private:
    void cleanup() {
        strategy_cancel_all();
        strategy_close("", "", kNaN, kNaN, false);
    }
    int calls() const { return reissue_ ? 2 : 1; }

    // w8a-dbl-p10 / -single: default percent_of_equity 10.
    void p10(std::int64_t t) {
        const double close = current_bar_.close;
        if (t == at(8, 1, 0)) strategy_entry("A1", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 3, 0)) {
            for (int i = 0; i < calls(); ++i) {
                strategy_entry("A2", false, kNaN, kNaN, kNaN, "");
                strategy_exit("XA2", "A2", close * 0.95, close * 1.05, kNaN, kNaN, kNaN, 100.0,
                              "", kNaN, "", kNaN, kNaN);
            }
        }
        if (t == at(8, 7, 0)) strategy_entry("B1", false, kNaN, kNaN, kNaN, "");
        if (t == at(8, 9, 0))
            for (int i = 0; i < calls(); ++i) strategy_entry("B2", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 13, 0)) strategy_entry("C1", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 15, 0))
            for (int i = 0; i < calls(); ++i)
                strategy_entry("C2", false, kNaN, kNaN, 1, "", "", 0, -1);
        if (t == at(9, 1, 0)) strategy_entry("E1", true, kNaN, kNaN, kNaN, "");
        if (t == at(9, 3, 0)) e_level_ = close + 0.01;
        const bool e_window = reissue_
            ? (t >= at(9, 3, 0) && t < at(9, 5, 0) && signed_position_size() > 0.0)
            : t == at(9, 3, 0);
        if (e_window) strategy_entry("E2", false, e_level_, kNaN, kNaN, "", "", 0, -1);
        if (t == at(9, 7, 0))
            for (int i = 0; i < calls(); ++i) strategy_entry("F1", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 5, 0) || t == at(8, 11, 0) || t == at(8, 17, 0) || t == at(9, 5, 0)
            || t == at(9, 9, 0)) {
            cleanup();
        }
    }

    // w8a-dbl-scope3 / -single: default percent_of_equity 10.
    void scope(std::int64_t t) {
        const double close = current_bar_.close;
        if (t == at(8, 1, 0)) strategy_entry("G1", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 3, 0))
            for (int i = 0; i < calls(); ++i)
                strategy_entry("G2", false, kNaN, kNaN, 10, "", "", 0, -1);
        if (t == at(8, 7, 0)) strategy_entry("H1", false, kNaN, kNaN, kNaN, "");
        if (t == at(8, 9, 0))
            for (int i = 0; i < calls(); ++i)
                strategy_entry("H2", true, kNaN, kNaN, 1, "", "", 0, -1);
        if (t == at(8, 19, 0)) strategy_entry("J1", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 21, 0)) {
            for (int i = 0; i < calls(); ++i) {
                strategy_entry("J2", false, kNaN, kNaN, kNaN, "");
                strategy_exit("XJ2", "J2", close * 0.92, close * 1.08, kNaN, kNaN, kNaN, 100.0,
                              "", kNaN, "", kNaN, kNaN);
            }
            strategy_entry("J3", false, kNaN, kNaN, kNaN, "");
            strategy_exit("XJ3", "J3", close * 0.92, close * 1.08, kNaN, kNaN, kNaN, 100.0, "",
                          kNaN, "", kNaN, kNaN);
        }
        if (t == at(9, 7, 0)) strategy_entry("K1", true, kNaN, kNaN, kNaN, "");
        if (t == at(9, 9, 0)) {
            if (reissue_) strategy_entry("K2", false, close * 1.1, kNaN, kNaN, "", "", 0, -1);
            strategy_entry("K2", false, kNaN, kNaN, kNaN, "");
        }
        if (t == at(9, 13, 0)) strategy_entry("N1", true, kNaN, kNaN, kNaN, "");
        if (t == at(9, 15, 0)) {
            strategy_entry("N2", false, kNaN, kNaN, kNaN, "");
            if (reissue_) {
                strategy_cancel("N2");
                strategy_entry("N2", false, kNaN, kNaN, kNaN, "");
            }
        }
        if (t == at(8, 5, 0) || t == at(8, 11, 0) || t == at(8, 23, 0) || t == at(9, 11, 0)
            || t == at(9, 17, 0)) {
            cleanup();
        }
    }

    // w8a-dbl-fixed / w8a-dbl-cash: A and B re-issue, C and D are the
    // single-call controls of the same shapes.
    void kinds(std::int64_t t) {
        if (t == at(8, 1, 0)) strategy_entry("A1", true, kNaN, kNaN, 3, "", "", 0, -1);
        if (t == at(8, 3, 0)) {
            strategy_entry("A2", false, kNaN, kNaN, kNaN, "");
            strategy_entry("A2", false, kNaN, kNaN, kNaN, "");
        }
        if (t == at(8, 7, 0)) strategy_entry("B1", false, kNaN, kNaN, 3, "", "", 0, -1);
        if (t == at(8, 9, 0)) {
            strategy_entry("B2", true, kNaN, kNaN, kNaN, "");
            strategy_entry("B2", true, kNaN, kNaN, kNaN, "");
        }
        if (t == at(8, 13, 0)) strategy_entry("C1", true, kNaN, kNaN, 3, "", "", 0, -1);
        if (t == at(8, 15, 0)) strategy_entry("C2", false, kNaN, kNaN, kNaN, "");
        if (t == at(8, 19, 0)) strategy_entry("D1", false, kNaN, kNaN, 3, "", "", 0, -1);
        if (t == at(8, 21, 0)) strategy_entry("D2", true, kNaN, kNaN, kNaN, "");
        if (t == at(8, 5, 0) || t == at(8, 11, 0) || t == at(8, 17, 0) || t == at(8, 23, 0))
            cleanup();
    }

    Probe probe_;
    bool reissue_;
    double e_level_ = kNaN;
};

// Lane W8B-SIGSTATE-2's probes (w8b-*): a seed on 2025-04-08 00:15 flipped at
// 00:30 (cell 1, after the price rose, so X < P for a long flip) and a seed
// on 04-09 00:00 flipped at 00:15 (cell 2, after it fell, so X > P); which
// side the seed takes, how often the flip bar calls its entry, which
// brackets exist, and how the seed is sized.
struct Shape {
    bool seed_long;        // the seed side (the flip takes the other)
    int calls;             // strategy.entry calls per flip bar
    bool flip_bracket;     // a new-side strategy.exit after each flip call
    bool seed_bracket;     // the seed carries its own bracket
    bool explicit_seeds;   // seeds name qty 2 (cell 1) and 0.5 (cell 2)
};

class FlipHost final : public source::PineStrategyHost {
public:
    FlipHost(const Shape& shape, const source::PineStrategyConfig& config) : shape_(shape) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        if (t == at(8, 0, 15)) seed(1, shape_.explicit_seeds ? 2.0 : kNaN);
        if (t == at(8, 0, 30)) flip(1);
        if (t == at(9, 0, 0)) seed(2, shape_.explicit_seeds ? 0.5 : kNaN);
        if (t == at(9, 0, 15)) flip(2);
        if (t == at(8, 2, 0) || t == at(9, 2, 0)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

private:
    void seed(int cell, double qty) {
        const bool is_long = shape_.seed_long;
        const std::string id = std::string(is_long ? "L" : "S") + std::to_string(cell);
        const std::string comment = id + (is_long ? " seed long" : " seed short");
        if (std::isnan(qty)) strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
        else strategy_entry(id, is_long, kNaN, kNaN, qty, comment, "", 0, -1);
        if (!shape_.seed_bracket) return;
        // Pine: strategy.exit(id, from, stop=..., limit=...). The short seeds
        // exit at stop 1580 / limit 1500 (cell 1) and 1500 / 1400 (cell 2);
        // the long seeds at stop 1500 / limit 1580 and 1400 / 1500.
        const std::string exit_id = "X" + id;
        const std::string exit_comment = id + " bracket";
        if (is_long) {
            if (cell == 1)
                strategy_exit(exit_id, id, 1580.0, 1500.0, kNaN, kNaN, kNaN, 100.0, exit_comment,
                              kNaN, "", kNaN, kNaN);
            else
                strategy_exit(exit_id, id, 1500.0, 1400.0, kNaN, kNaN, kNaN, 100.0, exit_comment,
                              kNaN, "", kNaN, kNaN);
        } else {
            if (cell == 1)
                strategy_exit(exit_id, id, 1500.0, 1580.0, kNaN, kNaN, kNaN, 100.0, exit_comment,
                              kNaN, "", kNaN, kNaN);
            else
                strategy_exit(exit_id, id, 1400.0, 1500.0, kNaN, kNaN, kNaN, 100.0, exit_comment,
                              kNaN, "", kNaN, kNaN);
        }
    }

    void flip(int cell) {
        const bool is_long = !shape_.seed_long;
        const std::string id = std::string(is_long ? "L" : "S") + std::to_string(cell);
        const std::string comment = id + (is_long ? " flip long" : " flip short");
        for (int i = 0; i < shape_.calls; ++i) {
            strategy_entry(id, is_long, kNaN, kNaN, kNaN, comment);
            if (shape_.flip_bracket)
                strategy_exit("X-" + id, id, 2500.0, 1000.0, kNaN, kNaN, kNaN, 100.0,
                              id + " bracket", kNaN, "", kNaN, kNaN);
        }
    }

    Shape shape_;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run trades_of(source::PineStrategyHost& host, std::int64_t end_ms) {
    host.set_trade_start_time(kTradeStartMs);
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

source::PineStrategyConfig config(double capital, QtyType type, double value, bool pooc) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    c.process_orders_on_close = pooc;
    return c;
}

// The w8a-* probes omit initial_capital (v6's 100000); the w8b-* probes
// declare 10000.
source::PineStrategyConfig w8a(QtyType type, double value) {
    return config(100000.0, type, value, false);
}
source::PineStrategyConfig w8b(QtyType type, double value, bool pooc) {
    return config(10000.0, type, value, pooc);
}

// Which host replays a tape.
struct Replay {
    bool flip = false;
    Probe probe = Probe::P10;
    bool reissue = false;
    Shape shape{};
};
Replay reissue_probe(Probe probe, bool reissue) { return {false, probe, reissue, Shape{}}; }
Replay flip_probe(const Shape& shape) { return {true, Probe::P10, false, shape}; }

struct Case {
    const char* tape;
    Replay replay;
    const char* declares;             // the probe's strategy() sizing arguments
    source::PineStrategyConfig lane;  // what its generated constructor declares
    std::size_t closed;               // tape trades closed inside the bars
};

// The rows of `tape` whose entry signal is `id`, in trade order.
std::vector<std::size_t> entered_by(const Tape& tape, const std::string& id) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < tape.trades.size(); ++i)
        if (tape.entry[i] == id) out.push_back(i);
    return out;
}

// Lots of the held side `id` that the order `by` closed.
long long closed_by(const Tape& tape, const std::string& id, const std::string& by) {
    long long total = 0;
    for (std::size_t i = 0; i < tape.trades.size(); ++i)
        if (tape.entry[i] == id && tape.exit[i] == by) total += std::get<3>(tape.trades[i]);
    return total;
}

long long opened(const Tape& tape, const std::string& id) {
    long long total = 0;
    for (const std::size_t i : entered_by(tape, id)) total += std::get<3>(tape.trades[i]);
    return total;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    constexpr QtyType kPercent = QtyType::PERCENT_OF_EQUITY;
    constexpr QtyType kFixed = QtyType::FIXED;
    //                     seed_long calls flip_bracket seed_bracket explicit_seeds
    const Shape single{false, 1, false, false, false};
    const Shape twice{false, 2, false, false, false};
    const Shape twice_bracket{false, 2, true, false, false};
    const Shape twice_seed_bracket{false, 2, false, true, false};
    const Shape sell{true, 2, false, true, false};
    const Shape twice_fixed{false, 2, false, false, true};
    const Shape sell_fixed{true, 2, false, true, true};

    const Case cases[] = {
        {"w8a-dbl-p10", reissue_probe(Probe::P10, true), "percent_of_equity, 10",
         w8a(kPercent, 10.0), 9},
        {"w8a-dbl-p10-single", reissue_probe(Probe::P10, false), "percent_of_equity, 10",
         w8a(kPercent, 10.0), 9},
        {"w8a-dbl-scope3", reissue_probe(Probe::Scope, true), "percent_of_equity, 10",
         w8a(kPercent, 10.0), 10},
        {"w8a-dbl-scope3-single", reissue_probe(Probe::Scope, false), "percent_of_equity, 10",
         w8a(kPercent, 10.0), 10},
        {"w8a-dbl-fixed", reissue_probe(Probe::Kinds, true), "fixed, 2", w8a(kFixed, 2.0), 8},
        {"w8a-dbl-cash", reissue_probe(Probe::Kinds, true), "cash, 5000",
         w8a(QtyType::CASH, 5000.0), 8},
        {"w8b-flip-single", flip_probe(single), "percent_of_equity, 10",
         w8b(kPercent, 10.0, false), 4},
        {"w8b-flip-double", flip_probe(twice), "percent_of_equity, 10",
         w8b(kPercent, 10.0, false), 4},
        {"w8b-flip-double-bracket", flip_probe(twice_bracket), "percent_of_equity, 10",
         w8b(kPercent, 10.0, false), 4},
        {"w8b-flip-double-seedbracket", flip_probe(twice_seed_bracket), "percent_of_equity, 10",
         w8b(kPercent, 10.0, false), 4},
        {"w8b-flip-double-sell", flip_probe(sell), "percent_of_equity, 10",
         w8b(kPercent, 10.0, false), 4},
        {"w8b-flip-double-fixed", flip_probe(twice_fixed), "fixed, 1", w8b(kFixed, 1.0, false),
         4},
        {"w8b-flip-double-sell-fixed", flip_probe(sell_fixed), "fixed, 1",
         w8b(kFixed, 1.0, false), 4},
        {"w8b-flip-double-pooc", flip_probe(twice),
         "percent_of_equity, 10, process_orders_on_close", w8b(kPercent, 10.0, true), 4},
        {"w8b-flip-double-sell-pooc", flip_probe(sell),
         "percent_of_equity, 10, process_orders_on_close", w8b(kPercent, 10.0, true), 4},
    };

    std::map<std::string, Tape> tapes;
    int replayed = 0, matched = 0;
    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);

        Run lane;
        if (c.replay.flip) {
            FlipHost host(c.replay.shape, c.lane);
            lane = trades_of(host, end_ms);
        } else {
            ReissueHost host(c.replay.probe, c.replay.reissue, c.lane);
            lane = trades_of(host, end_ms);
        }
        CHECK(lane.error.empty());
        CHECK(lane.trades == tape.trades);
        ++replayed;
        if (lane.trades == tape.trades) {
            ++matched;
        } else {
            show("tape", tape.trades);
            show("engine", lane.trades);
        }
    }
    std::printf("-- %d of %d tapes replayed trade for trade\n", matched, replayed);

    // The rule, read off TradingView's own rows: a same-bar MARKET reissue
    // moves only its own quantity. Sell side, default percent (A2): it
    // closes its default size of A1 and opens nothing, the rest of A1 rides
    // to the cleanup; the single call reverses A1 in full.
    std::printf("-- the rule, on the tapes\n");
    {
        const Tape& dbl = tapes["w8a-dbl-p10"];
        const Tape& one = tapes["w8a-dbl-p10-single"];
        CHECK(entered_by(dbl, "A2").empty());
        CHECK(closed_by(dbl, "A1", "A2") > 0);
        CHECK(closed_by(dbl, "A1", "Close position order") > 0);
        CHECK(opened(one, "A2") > 0 && closed_by(one, "A1", "A2") == opened(one, "A1"));
        // Buy side, default percent (B2): the whole short is closed and the
        // long it opens is only the default size's excess over it.
        CHECK(closed_by(dbl, "B1", "B2") == opened(dbl, "B1"));
        CHECK(opened(dbl, "B2") > 0 && opened(dbl, "B2") * 100 < opened(dbl, "B1"));
        CHECK(opened(one, "B2") * 2 > opened(one, "B1"));
        // Explicit qty=1 (C2): one unit of C1 closes, nothing opens.
        CHECK(closed_by(dbl, "C1", "C2") == lots(1.0));
        CHECK(entered_by(dbl, "C2").empty());
        CHECK(opened(one, "C2") == lots(1.0));
    }
    {
        const Tape& dbl = tapes["w8a-dbl-scope3"];
        const Tape& one = tapes["w8a-dbl-scope3-single"];
        // Explicit qty=10 over a smaller long (G2): 10 units move in all.
        CHECK(closed_by(dbl, "G1", "G2") + opened(dbl, "G2") == lots(10.0));
        CHECK(opened(one, "G2") == lots(10.0));
        // Explicit qty=1 over a short (H2): one unit of H1 closes.
        CHECK(closed_by(dbl, "H1", "H2") == lots(1.0));
        CHECK(entered_by(dbl, "H2").empty());
        CHECK(opened(one, "H2") == lots(1.0));
        // A later same-side sibling (J3) never fills, reissued or not.
        CHECK(entered_by(dbl, "J3").empty() && entered_by(one, "J3").empty());
        CHECK(entered_by(dbl, "J2").empty() && !entered_by(one, "J2").empty());
        // A limit predecessor (K2) and a strategy.cancel between the calls
        // (N2) keep the full reversal.
        CHECK(closed_by(dbl, "K1", "K2") == opened(dbl, "K1") && opened(dbl, "K2") > 0);
        CHECK(closed_by(dbl, "N1", "N2") == opened(dbl, "N1") && opened(dbl, "N2") > 0);
    }
    for (const char* name : {"w8a-dbl-fixed", "w8a-dbl-cash"}) {
        // Fixed 2 and cash 5000 defaults over a 3-unit seed: the reissued A2
        // and B2 move their default size once; the single-call C2 and D2
        // reverse the whole seed.
        const Tape& tape = tapes[name];
        CHECK(closed_by(tape, "A1", "A2") + opened(tape, "A2") < lots(3.0) + lots(3.5));
        CHECK(closed_by(tape, "B1", "B2") + opened(tape, "B2") < lots(3.0) + lots(3.5));
        CHECK(closed_by(tape, "C1", "C2") == lots(3.0) && opened(tape, "C2") >= lots(2.0));
        CHECK(closed_by(tape, "D1", "D2") == lots(3.0) && opened(tape, "D2") >= lots(2.0));
    }

    // The w8b-* flips. Cell 1 is every trade entered on 2025-04-08, cell 2
    // every trade entered on 2025-04-09.
    const auto in_cell = [](const Row& r, int cell) {
        return std::get<0>(r) >= at(cell == 1 ? 8 : 9, 0, 0)
            && std::get<0>(r) < at(cell == 1 ? 9 : 10, 0, 0);
    };
    {
        // One call reverses: in each cell the seed closes whole at the flip
        // and the flip side opens with its own size.
        const Tape& once = tapes["w8b-flip-single"];
        CHECK(once.trades.size() == 4);
        if (once.trades.size() == 4) {
            for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
                CHECK(!std::get<1>(once.trades[i]));
                CHECK(std::get<1>(once.trades[i + 1]));
                CHECK(std::get<3>(once.trades[i + 1]) > 6000);  // 0.6 of a unit and more
            }
        }
    }
    // Two calls net the flip's own size X: in cell 1 (X below P) the seed side
    // splits in two rows at its one entry and the flip side never opens; in
    // cell 2 (X above P) the flip side opens with X - P only: 0.5 of a unit
    // under the fixed default of 1 against the 0.5 seed, and a sliver (below
    // 0.01, where a reversal opens about 0.68) under 10% of equity.
    for (const char* name : {"w8b-flip-double", "w8b-flip-double-bracket",
                             "w8b-flip-double-seedbracket", "w8b-flip-double-sell",
                             "w8b-flip-double-fixed", "w8b-flip-double-sell-fixed",
                             "w8b-flip-double-pooc", "w8b-flip-double-sell-pooc"}) {
        const Tape& tape = tapes[name];
        const bool seed_long = std::string(name).find("sell") != std::string::npos;
        const bool fixed = std::string(name).find("fixed") != std::string::npos;
        std::vector<Row> cell1, cell2;
        for (const Row& r : tape.trades) (in_cell(r, 1) ? cell1 : cell2).push_back(r);
        CHECK(cell1.size() == 2);
        CHECK(cell2.size() == 2);
        for (const Row& r : cell1) CHECK(std::get<1>(r) == seed_long);
        if (cell1.size() == 2) CHECK(std::get<0>(cell1[0]) == std::get<0>(cell1[1]));
        if (cell2.size() == 2) {
            CHECK(std::get<1>(cell2[0]) == seed_long);
            CHECK(std::get<1>(cell2[1]) != seed_long);
            if (fixed) CHECK(std::get<3>(cell2[1]) == lots(1.0) - std::get<3>(cell2[0]));
            else CHECK(std::get<3>(cell2[1]) < lots(0.01));
        }
    }
    {
        // The held short keeps its bracket: its remainder exits by it.
        const Tape& buy = tapes["w8b-flip-double-seedbracket"];
        CHECK(buy.trades.size() == 4);
        if (buy.trades.size() == 4) CHECK(buy.exit[1] == "S1 bracket");
        // The held long's bracket is void: its remainder rides to the
        // cleanup, although the bracket's limit (1580) trades at 01:30.
        for (const char* name : {"w8b-flip-double-sell", "w8b-flip-double-sell-fixed"}) {
            const Tape& tape = tapes[name];
            CHECK(tape.trades.size() == 4);
            if (tape.trades.size() == 4) CHECK(tape.exit[1] == "cleanup");
        }
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
