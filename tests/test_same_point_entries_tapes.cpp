/*
 * test_same_point_entries_tapes.cpp -- lane W6-ENG-FILL-ORDER.
 *
 * Which order fills first when several reach one fill point, on
 * TradingView's own tapes of synthetic probes (tests/fixtures/
 * same_point_entries, lab tv exports on BINANCE:ETHUSDT.P 15).
 *
 *   F10  Two strategy.entry calls placed on one bar from flat, both already
 *        marketable, fill at one price. TradingView fills them in a fixed
 *        order of side and kind -- buy market, buy stop, sell market, sell
 *        stop, buy limit, sell limit -- and one rank in placement order. The
 *        later of two opposite calls is one transaction of its own quantity
 *        plus the earlier pending MARKET call's (a pending stop or limit
 *        lends it nothing), and the earlier call trades only its own, so a
 *        buy that fills first opens both quantities as one lot the sell then
 *        partly closes: the tape's two rows of the buy's signal.
 *        TradingView costs that transaction at the signal, and past the
 *        equity it drops the later call.
 *        Under process_orders_on_close a market pair fills at the close in
 *        the same order.
 *        With a commission (lane W6B-ENG-PAIRS) the order and the one
 *        transaction are the same: its fee is split across the rows it books
 *        by quantity, and its admission against the equity leaves the fee
 *        out, however little of the equity the fee would leave. Under
 *        pyramiding 2 a pair books as under 0, and same-side lots fill by
 *        rank too, three of them under pyramiding 3.
 *   F12  A held position's protective strategy.exit stop and a reversing
 *        strategy.entry stop on the same side of the price: the level the
 *        bar's path reaches first fills first, the magnifier changes nothing,
 *        and when both are already through at one opening the exit fills
 *        first and the entry then trades its own frozen quantity from flat.
 *        Under the v6 default size the exit fills at that opening even when
 *        the reversal it outranks is declined there.
 *
 * Each row replays one tape through the Pine adapter under the configuration
 * the generated constructor declares for its probe, over the corpus 15m bars
 * of fixtures/tvdef_drops/bars.inc, trading from the bar before TradingView's
 * first entry as run_strategy.py does for a corpus tape, and requires every
 * trade the tape closes in the rows' cells -- entry and exit time, side,
 * price in ticks of 0.01 and quantity in lots of 0.0001 -- to be the
 * engine's, in the tape's order. A row names the cells it covers; the README
 * lists the cells TradingView and the engine still book differently.
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

#ifndef PINEFORGE_SAME_POINT_FIXTURE_DIR
#error "PINEFORGE_SAME_POINT_FIXTURE_DIR must name tests/fixtures/same_point_entries"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/tvdef_drops/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
constexpr std::int64_t kBarMs = 15 * 60'000;
constexpr std::int64_t kStepMs = 2 * 60 * 60'000;
constexpr std::int64_t kHourMs = 60 * 60'000;
constexpr std::int64_t kCleanupMs = 30 * 60'000;

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
    std::vector<Row> trades;           // closed inside the replayed bars, in tape order
    std::vector<std::string> entries;  // each trade's entry signal
    std::vector<std::string> exits;    // each trade's exit signal
    std::vector<double> fees;          // each trade's commission
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_SAME_POINT_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
    std::map<int, double> fee;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USDT, Size (qty),
        // Size (value), Net PnL, Return %, Commission, ...
        if (cell.size() < 10) continue;
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
            fee[number] = std::stod(cell[9]);
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(price);
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        const auto exit = exit_signal.find(number);
        if (exit != exit_signal.end() && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entries.push_back(entry_signal[number]);
            out.exits.push_back(exit->second);
            out.fees.push_back(fee[number]);
        }
    }
    return out;
}

// One strategy.entry leg of an F10 cell: side, kind (M market, S stop, L
// limit; a priced leg sits at half or twice the close, where it is already
// marketable), quantity and comment.
struct Leg {
    bool is_long;
    char kind;
    double qty;
    const char* comment;
};
struct Cell {
    Leg first, second;
    std::vector<Leg> more = {};  // w6b-p2c: a third leg
};

// The probes' cell tables, as their strategy.pine sources declare them.
const std::vector<Cell> kOpenPair = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{true, 'M', 1, "MS-LF-1"}, {false, 'S', 1, "MS-LF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
    {{true, 'S', 1, "SM-LF-1"}, {false, 'M', 1, "SM-LF-2"}},
    {{false, 'S', 1, "SM-SF-1"}, {true, 'M', 1, "SM-SF-2"}},
    {{true, 'S', 1, "SS-LF-1"}, {false, 'S', 1, "SS-LF-2"}},
    {{false, 'S', 1, "SS-SF-1"}, {true, 'S', 1, "SS-SF-2"}},
    {{false, 'M', 1, "ML-SF-1"}, {true, 'L', 1, "ML-SF-2"}},
    {{false, 'L', 1, "LM-SF-1"}, {true, 'M', 1, "LM-SF-2"}},
    {{false, 'L', 1, "LL-SF-1"}, {true, 'L', 1, "LL-SF-2"}},
    {{false, 'L', 1, "LS-SF-1"}, {true, 'S', 1, "LS-SF-2"}},
    {{false, 'S', 1, "SL-SF-1"}, {true, 'L', 1, "SL-SF-2"}},
    {{false, 'M', 2, "MS-SF-Q2-1"}, {true, 'S', 3, "MS-SF-Q3-2"}},
};
const std::vector<Cell> kLimitClass = {
    {{true, 'L', 1, "LM-LF-1"}, {false, 'M', 1, "LM-LF-2"}},
    {{true, 'L', 1, "LS-LF-1"}, {false, 'S', 1, "LS-LF-2"}},
    {{true, 'L', 1, "LL-LF-1"}, {false, 'L', 1, "LL-LF-2"}},
    {{true, 'M', 1, "ML-LF-1"}, {false, 'L', 1, "ML-LF-2"}},
    {{true, 'S', 1, "SL-LF-1"}, {false, 'L', 1, "SL-LF-2"}},
    {{true, 'L', 2, "LM-LF-Q2-1"}, {false, 'M', 3, "LM-LF-Q3-2"}},
    {{false, 'M', 2, "ML-SF-Q2-1"}, {true, 'L', 3, "ML-SF-Q3-2"}},
    {{false, 'L', 2, "LM-SF-Q2-1"}, {true, 'M', 3, "LM-SF-Q3-2"}},
};
const std::vector<Cell> kSameSide = {
    {{true, 'L', 1, "LM-L-1"}, {true, 'M', 2, "LM-L-2"}},
    {{true, 'L', 1, "LS-L-1"}, {true, 'S', 2, "LS-L-2"}},
    {{true, 'S', 1, "SM-L-1"}, {true, 'M', 2, "SM-L-2"}},
    {{true, 'M', 1, "MS-L-1"}, {true, 'S', 2, "MS-L-2"}},
    {{false, 'L', 1, "LM-S-1"}, {false, 'M', 2, "LM-S-2"}},
    {{false, 'L', 1, "LS-S-1"}, {false, 'S', 2, "LS-S-2"}},
    {{false, 'S', 1, "SM-S-1"}, {false, 'M', 2, "SM-S-2"}},
    {{false, 'M', 1, "MS-S-1"}, {false, 'S', 2, "MS-S-2"}},
};
const std::vector<Cell> kGross = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{true, 'M', 1, "MS-LF-1"}, {false, 'S', 1, "MS-LF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
};
const std::vector<Cell> kClosePair = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
    {{false, 'S', 1, "SM-SF-1"}, {true, 'M', 1, "SM-SF-2"}},
    {{false, 'L', 1, "LM-SF-1"}, {true, 'M', 1, "LM-SF-2"}},
    {{true, 'L', 1, "LM-LF-1"}, {false, 'M', 1, "LM-LF-2"}},
    {{false, 'M', 2, "MM-SF-Q2-1"}, {true, 'M', 3, "MM-SF-Q3-2"}},
    {{false, 'M', 1, "ML-SF-1"}, {true, 'L', 1, "ML-SF-2"}},
};

// w6b-p2c: three same-side legs, a market, a stop and a limit in every order,
// quantities 1, 2 and 3 in placement order, under pyramiding 3.
const std::vector<Cell> kSameSideTriple = {
    {{true, 'M', 1, "MSL-L-1"}, {true, 'S', 2, "MSL-L-2"},
     {{true, 'L', 3, "MSL-L-3"}}},
    {{true, 'M', 1, "MLS-L-1"}, {true, 'L', 2, "MLS-L-2"},
     {{true, 'S', 3, "MLS-L-3"}}},
    {{true, 'S', 1, "SML-L-1"}, {true, 'M', 2, "SML-L-2"},
     {{true, 'L', 3, "SML-L-3"}}},
    {{true, 'S', 1, "SLM-L-1"}, {true, 'L', 2, "SLM-L-2"},
     {{true, 'M', 3, "SLM-L-3"}}},
    {{true, 'L', 1, "LMS-L-1"}, {true, 'M', 2, "LMS-L-2"},
     {{true, 'S', 3, "LMS-L-3"}}},
    {{true, 'L', 1, "LSM-L-1"}, {true, 'S', 2, "LSM-L-2"},
     {{true, 'M', 3, "LSM-L-3"}}},
    {{false, 'M', 1, "MSL-S-1"}, {false, 'S', 2, "MSL-S-2"},
     {{false, 'L', 3, "MSL-S-3"}}},
    {{false, 'M', 1, "MLS-S-1"}, {false, 'L', 2, "MLS-S-2"},
     {{false, 'S', 3, "MLS-S-3"}}},
    {{false, 'S', 1, "SML-S-1"}, {false, 'M', 2, "SML-S-2"},
     {{false, 'L', 3, "SML-S-3"}}},
    {{false, 'S', 1, "SLM-S-1"}, {false, 'L', 2, "SLM-S-2"},
     {{false, 'M', 3, "SLM-S-3"}}},
    {{false, 'L', 1, "LMS-S-1"}, {false, 'M', 2, "LMS-S-2"},
     {{false, 'S', 3, "LMS-S-3"}}},
    {{false, 'L', 1, "LSM-S-1"}, {false, 'S', 2, "LSM-S-2"},
     {{false, 'M', 3, "LSM-S-3"}}},
};

// w6-f10i / w6-f10j: the pairs with a commission.
const std::vector<Cell> kCommissionPair = {
    {{true, 'M', 1, "MM-LF-1"}, {false, 'M', 1, "MM-LF-2"}},
    {{false, 'M', 1, "MM-SF-1"}, {true, 'M', 1, "MM-SF-2"}},
    {{false, 'M', 1, "MS-SF-1"}, {true, 'S', 1, "MS-SF-2"}},
    {{false, 'M', 2, "MS-SF-Q2-1"}, {true, 'S', 3, "MS-SF-Q3-2"}},
};
// w6b-p1a/b/c: the gross-admission bands (the quantities are sized off the
// equity at the signal, so the table's are unused).
const std::vector<Cell> kGrossBands = {
    {{true, 'M', 0, "MM-LF-W-1"}, {false, 'M', 0, "MM-LF-W-2"}},
    {{true, 'M', 0, "MM-LF-U-1"}, {false, 'M', 0, "MM-LF-U-2"}},
    {{true, 'M', 0, "MM-LF-O-1"}, {false, 'M', 0, "MM-LF-O-2"}},
    {{false, 'M', 0, "MM-SF-W-1"}, {true, 'M', 0, "MM-SF-W-2"}},
    {{false, 'M', 0, "MM-SF-U-1"}, {true, 'M', 0, "MM-SF-U-2"}},
    {{false, 'M', 0, "MM-SF-O-1"}, {true, 'M', 0, "MM-SF-O-2"}},
    {{false, 'M', 0, "MS-SF-W-1"}, {true, 'S', 0, "MS-SF-W-2"}},
    {{false, 'M', 0, "MS-SF-U-1"}, {true, 'S', 0, "MS-SF-U-2"}},
    {{false, 'M', 0, "MS-SF-O-1"}, {true, 'S', 0, "MS-SF-O-2"}},
};

enum class Probe { Pair, GuardedPair, Stops, Gross };

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const std::vector<Cell>* cells, int passes,
              const source::PineStrategyConfig& config, std::int64_t step_ms, double fee)
        : probe_(probe), cells_(cells), passes_(passes), step_ms_(step_ms), fee_(fee) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        if (probe_ == Probe::Stops) stops(current_bar_.timestamp);
        else if (probe_ == Probe::Gross) gross(current_bar_.timestamp);
        else pair(current_bar_.timestamp);
    }

private:
    // A buy stop / sell limit at half the close, a sell stop / buy limit at
    // twice it: each is already through the price.
    void place(const std::string& id, const Leg& leg) {
        const double lo = current_bar_.close * 0.5;
        const double hi = current_bar_.close * 2.0;
        const double stop = leg.kind == 'S' ? (leg.is_long ? lo : hi) : kNaN;
        const double limit = leg.kind == 'L' ? (leg.is_long ? hi : lo) : kNaN;
        strategy_entry(id, leg.is_long, limit, stop, leg.qty, leg.comment, "", 0, -1);
    }

    // w6-f10a/b/c: the pair every two hours from 2025-04-08 00:00 UTC while
    // flat, flattened 30 minutes later; w6-f10d/e/f act once per confirmed
    // bar (a fill recalculation places nothing).
    void pair(std::int64_t t) {
        const std::int64_t n = static_cast<std::int64_t>(cells_->size());
        const std::int64_t rel = t - at(8, 0, 0);
        const bool in_range = rel >= 0 && rel < passes_ * n * step_ms_;
        const bool event = in_range && rel % step_ms_ == 0;
        const bool cleanup = in_range && rel % step_ms_ == kCleanupMs;
        const bool guarded = probe_ == Probe::GuardedPair;
        const bool first_pass = pine_bar_index() != acted_bar_;
        if (guarded && !(is_last_tick_ && first_pass)) return;
        if (event && signed_position_size() == 0.0) {
            acted_bar_ = pine_bar_index();
            const Cell& cell = (*cells_)[static_cast<std::size_t>((rel / step_ms_) % n)];
            place("E1", cell.first);
            place("E2", cell.second);
            for (std::size_t k = 0; k < cell.more.size(); ++k)
                place("E" + std::to_string(3 + k), cell.more[k]);
        }
        if (cleanup && (!guarded || signed_position_size() != 0.0)) {
            acted_bar_ = pine_bar_index();
            strategy_cancel_all();
            strategy_close("", "FLT", kNaN, kNaN, false);
        }
    }

    // w6b-p1a/b/c: the pair's transaction sized off strategy.equity at the
    // signal close -- well under it (W), under it by less than the fee (U),
    // or just over it (O) -- every hour from 2025-04-08 00:00 UTC.
    void gross(std::int64_t t) {
        const std::int64_t n = static_cast<std::int64_t>(cells_->size());
        const std::int64_t rel = t - at(8, 0, 0);
        const bool in_range = rel >= 0 && rel < passes_ * n * step_ms_;
        const int cell = in_range ? static_cast<int>((rel / step_ms_) % n) : -1;
        const double close = current_bar_.close;
        const double equity = current_equity() + open_profit(close);
        const double fee = fee_ > 0.0 ? fee_ : -fee_ * equity;
        const int band = cell % 3;
        const double gap = band == 0 ? 2 * fee : (band == 1 ? 0.5 * fee : -0.5 * fee);
        const double total = std::floor((equity - gap) / close * 10000) / 10000;
        const double qa = std::floor(total / 2 * 10000) / 10000;
        const double qb = std::round((total - qa) * 10000) / 10000;
        if (in_range && rel % step_ms_ == 0 && signed_position_size() == 0.0) {
            const Cell& c = (*cells_)[static_cast<std::size_t>(cell)];
            strategy_entry("E1", c.first.is_long, kNaN, kNaN, qa, c.first.comment, "", 0, -1);
            strategy_entry("E2", c.second.is_long, kNaN,
                           c.second.kind == 'S' ? close * 0.5 : kNaN, qb, c.second.comment,
                           "", 0, -1);
        }
        if (in_range && rel % step_ms_ == kCleanupMs) {
            strategy_cancel_all();
            strategy_close("", "FLT", kNaN, kNaN, false);
        }
    }

    void arm(bool reverse_first, double reverse_stop, double exit_stop, bool long_held,
             const char* reverse, const char* exit) {
        const char* held = long_held ? "L" : "S";
        const char* entry = long_held ? "S" : "L";
        const char* exit_id = long_held ? "LX" : "SX";
        if (reverse_first)
            strategy_entry(entry, !long_held, kNaN, reverse_stop, kNaN, reverse, "", 0, -1);
        strategy_exit(exit_id, held, kNaN, exit_stop, kNaN, kNaN, kNaN, 100.0, exit, kNaN, "",
                      kNaN, kNaN);
        if (!reverse_first)
            strategy_entry(entry, !long_held, kNaN, reverse_stop, kNaN, reverse, "", 0, -1);
    }

    // w6-f12a/b/c: each cell seeds a position, arms a protective exit stop and
    // a reversing entry stop on the next bar, and is flattened later.
    void stops(std::int64_t t) {
        const double c = current_bar_.close;
        const bool long_held = signed_position_size() > 0.0;
        const bool short_held = signed_position_size() < 0.0;
        if (t == at(8, 13, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A1 seed");
        if (t == at(8, 13, 15) && long_held) arm(true, 1570.0, 1560.0, true, "A1 reverse", "A1 stop");
        if (t == at(7, 13, 45)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A2 seed");
        if (t == at(7, 14, 0) && long_held) arm(true, 1575.0, 1560.0, true, "A2 reverse", "A2 stop");
        if (t == at(8, 16, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A3 seed");
        if (t == at(8, 16, 15) && long_held) arm(true, 1500.0, 1510.0, true, "A3 reverse", "A3 stop");
        if (t == at(9, 0, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A4 seed");
        if (t == at(9, 0, 15) && long_held) arm(true, c * 1.02, c * 1.01, true, "A4 reverse", "A4 stop");
        if (t == at(9, 2, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A5 seed");
        if (t == at(9, 2, 15) && long_held) arm(false, c * 1.02, c * 1.01, true, "A5 reverse", "A5 stop");
        if (t == at(9, 4, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A6 seed");
        if (t == at(9, 4, 15) && long_held) arm(true, c * 1.02, c * 0.9, true, "A6 reverse", "A6 stop");
        if (t == at(9, 6, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "A7 seed");
        if (t == at(9, 6, 15) && long_held) arm(true, c * 0.9, c * 1.01, true, "A7 reverse", "A7 stop");
        if (t == at(8, 17, 0)) strategy_entry("S", false, kNaN, kNaN, kNaN, "B1 seed");
        if (t == at(8, 17, 15) && short_held) arm(true, 1468.0, 1478.0, false, "B1 reverse", "B1 stop");
        if (t == at(9, 8, 0)) strategy_entry("S", false, kNaN, kNaN, kNaN, "B2 seed");
        if (t == at(9, 8, 15) && short_held) arm(true, c * 0.98, c * 0.99, false, "B2 reverse", "B2 stop");
        const bool cleanup = t == at(8, 14, 0) || t == at(7, 14, 45) || t == at(8, 16, 45)
            || t == at(9, 1, 0) || t == at(9, 3, 0) || t == at(9, 5, 0) || t == at(9, 7, 0)
            || t == at(8, 18, 0) || t == at(9, 9, 0);
        if (cleanup) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

    Probe probe_;
    const std::vector<Cell>* cells_;
    std::int64_t passes_;
    std::int64_t step_ms_;
    double fee_;  // the gross probe's fee: money, or (negative) a share of the equity
    int acted_bar_ = -1;
};

struct Run {
    std::vector<Row> trades;
    std::vector<double> fees;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, const std::vector<Cell>* cells, int passes,
        const source::PineStrategyConfig& config, std::int64_t trade_start_ms,
        std::int64_t end_ms, std::int64_t step_ms, double fee) {
    ProbeHost host(probe, cells, passes, config, step_ms, fee);
    host.set_trade_start_time(trade_start_ms);
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
        out.fees.push_back(t.commission);
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

source::PineStrategyConfig fixed_config(int pyramiding, bool pooc, bool coof,
                                        double capital = 1000000.0) {
    source::PineStrategyConfig c{};
    c.initial_capital = capital;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyramiding;
    c.process_orders_on_close = pooc;
    c.calc_on_order_fills = coof;
    return c;
}

source::PineStrategyConfig commissioned(source::PineStrategyConfig c, CommissionType type,
                                        double value) {
    c.commission_type = static_cast<int>(type);
    c.commission_value = value;
    return c;
}

source::PineStrategyConfig v6_default_config() {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;  // omitted: v6's defaults
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 0;
    return c;
}

// The pair cell an entry time belongs to (its slot), -1 outside.
int pair_cell(std::int64_t entry_ms, std::size_t cells, int passes,
              std::int64_t step_ms = kStepMs) {
    const std::int64_t rel = entry_ms - at(8, 0, 0);
    if (rel < 0 || rel >= passes * static_cast<std::int64_t>(cells) * step_ms) return -1;
    return static_cast<int>((rel / step_ms) % static_cast<std::int64_t>(cells));
}

struct Case {
    const char* tape;
    Probe probe;
    const std::vector<Cell>* cells;
    int passes;                        // how many times the cell table repeats
    const char* declares;              // the probe's strategy() arguments that matter
    source::PineStrategyConfig lane;   // what its generated constructor declares
    std::vector<int> covered;          // the cells this row requires (empty: every trade)
    std::size_t closed;                // tape trades closed inside the bars
    // Entry-time windows [from, to) of cells this row leaves out (README).
    std::vector<std::pair<std::int64_t, std::int64_t>> left_out = {};
    std::int64_t step_ms = kStepMs;    // one cell's slot
    double fee = 0.0;                  // Probe::Gross: the probe's feeEst
    bool fees = false;                 // also require each trade's commission
};

std::vector<Row> covered_rows(const std::vector<Row>& rows, const Case& c,
                              const std::vector<double>* fees = nullptr,
                              std::vector<double>* covered_fees = nullptr) {
    std::vector<Row> out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const std::int64_t entry = std::get<0>(row);
        bool keep = c.covered.empty();
        for (const int wanted : c.covered)
            if (pair_cell(entry, c.cells->size(), c.passes, c.step_ms) == wanted) keep = true;
        for (const auto& [from, to] : c.left_out)
            if (entry >= from && entry < to) keep = false;
        if (!keep) continue;
        out.push_back(row);
        if (fees && covered_fees) covered_fees->push_back((*fees)[i]);
    }
    return out;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;
    const std::vector<int> all14 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    const std::vector<int> all8 = {0, 1, 2, 3, 4, 5, 6, 7};
    const std::vector<int> all9 = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<int> all12 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const std::vector<int> all4 = {0, 1, 2, 3};
    const auto per_order = CommissionType::CASH_PER_ORDER;
    const auto percent = CommissionType::PERCENT;

    const Case cases[] = {
        {"w6-f10a-open-pair", Probe::Pair, &kOpenPair, 2, "fixed 1, pyramiding 0",
         fixed_config(0, false, false), all14, 40},
        {"w6-f10b-limit-class", Probe::Pair, &kLimitClass, 2, "fixed 1, pyramiding 0",
         fixed_config(0, false, false), all8, 24},
        {"w6-f10g-pair-gross", Probe::Pair, &kGross, 7, "fixed 1, pyramiding 0, capital 3000",
         fixed_config(0, false, false, 3000.0), {0, 1, 2, 3}, 38},
        {"w6-f10h-pair-gross-pyramiding1", Probe::Pair, &kGross, 7,
         "fixed 1, pyramiding 1, capital 3000", fixed_config(1, false, false, 3000.0),
         {0, 1, 2, 3}, 38},
        {"w6-f10d-pooc-pair", Probe::GuardedPair, &kClosePair, 2,
         "fixed 1, pyramiding 0, process_orders_on_close", fixed_config(0, true, false),
         {0, 1, 6}, 26},
        {"w6-f10e-pooc-coof-pair", Probe::GuardedPair, &kClosePair, 2,
         "fixed 1, pyramiding 0, process_orders_on_close, calc_on_order_fills",
         fixed_config(0, true, true), {0, 1, 6}, 26},
        {"w6-f12a-stop-priority", Probe::Stops, nullptr, 0, "fixed 1",
         fixed_config(0, false, false), {}, 17},
        {"w6-f12c-stop-priority-magnifier", Probe::Stops, nullptr, 0,
         "fixed 1, use_bar_magnifier (the tape equals w6-f12a's rows)",
         fixed_config(0, false, false), {}, 17},
        {"w6-f12b-stop-priority-default", Probe::Stops, nullptr, 0,
         "nothing (v6: percent_of_equity, 100)", v6_default_config(), {}, 11,
         {{at(9, 0, 0), at(9, 1, 0)}}},
        // Lane W6B-ENG-PAIRS, item 1: a commissioned pair.
        {"w6-f10i-pair-commission-percent", Probe::Pair, &kCommissionPair, 2,
         "fixed 1, pyramiding 0, commission 0.1 percent",
         commissioned(fixed_config(0, false, false), percent, 0.1), all4, 16, {}, kStepMs, 0.0,
         true},
        {"w6-f10j-pair-commission-per-order", Probe::Pair, &kCommissionPair, 2,
         "fixed 1, pyramiding 0, commission 5 per order",
         commissioned(fixed_config(0, false, false), per_order, 5.0), all4, 16, {}, kStepMs,
         0.0, true},
        {"w6b-p1d-open-pair-per-order", Probe::Pair, &kOpenPair, 2,
         "fixed 1, pyramiding 0, commission 5 per order",
         commissioned(fixed_config(0, false, false), per_order, 5.0), all14, 40, {}, kStepMs,
         0.0, true},
        {"w6b-p1a-pair-gross-commission-off", Probe::Gross, &kGrossBands, 2,
         "fixed 1, capital 10000, no commission", fixed_config(0, false, false, 10000.0), all9,
         30, {}, kHourMs, 50.0, true},
        {"w6b-p1b-pair-gross-per-order", Probe::Gross, &kGrossBands, 2,
         "fixed 1, capital 10000, commission 50 per order",
         commissioned(fixed_config(0, false, false, 10000.0), per_order, 50.0), all9, 30, {},
         kHourMs, 50.0, true},
        {"w6b-p1c-pair-gross-percent", Probe::Gross, &kGrossBands, 2,
         "fixed 1, capital 10000, commission 1 percent",
         commissioned(fixed_config(0, false, false, 10000.0), percent, 1.0), all9, 30, {},
         kHourMs, -0.01, true},
        // Item 2: pyramiding above 1.
        {"w6-f10c-same-side-class", Probe::Pair, &kSameSide, 2, "fixed 1, pyramiding 2",
         fixed_config(2, false, false), all8, 32},
        {"w6b-p2a-open-pair-pyramiding2", Probe::Pair, &kOpenPair, 2,
         "fixed 1, pyramiding 2 (the tape equals w6-f10a's)", fixed_config(2, false, false),
         all14, 40},
        {"w6b-p2b-limit-class-pyramiding2", Probe::Pair, &kLimitClass, 2,
         "fixed 1, pyramiding 2 (the tape equals w6-f10b's)", fixed_config(2, false, false),
         all8, 24},
        {"w6b-p2c-same-side-pyramiding3", Probe::Pair, &kSameSideTriple, 2,
         "fixed 1, pyramiding 3", fixed_config(3, false, false), all12, 72},
    };

    for (const Case& c : cases) {
        std::printf("-- %s (declares %s)\n", c.tape, c.declares);
        const Tape tape = tape_trades(c.tape, end_ms);
        CHECK(tape.trades.size() == c.closed);
        const std::int64_t trade_start = tape.trades.empty()
            ? at(7, 0, 0) : std::get<0>(tape.trades.front()) - kBarMs;
        const Run lane = run(c.probe, c.cells, c.passes, c.lane, trade_start, end_ms, c.step_ms,
                             c.fee);
        CHECK(lane.error.empty());
        std::vector<double> want_fees, got_fees;
        const std::vector<Row> want = covered_rows(tape.trades, c, &tape.fees, &want_fees);
        const std::vector<Row> got = covered_rows(lane.trades, c, &lane.fees, &got_fees);
        CHECK(!want.empty());
        CHECK(got == want);
        if (got != want) {
            show("tape", want);
            show("engine", got);
        }
        if (c.fees && got == want) {
            // The commission of each row: one combined order's fee is split
            // across the rows it books by quantity.
            for (std::size_t i = 0; i < want.size(); ++i) {
                CHECK(std::abs(got_fees[i] - want_fees[i]) < 1e-6);
                if (std::abs(got_fees[i] - want_fees[i]) >= 1e-6)
                    std::printf("    fee row %zu: tape %.8f engine %.8f\n", i, want_fees[i],
                                got_fees[i]);
            }
        }
    }

    // What TradingView's own rows say, on every pair tape -- the ones the
    // engine replays above and the ones it does not yet (README). In each
    // cell where both legs traded, the first row is the leg of the lower
    // rank, and when that is the later call opposite an earlier market, it
    // opened both quantities as one lot the earlier call then partly closed.
    std::printf("-- the order, on the tapes\n");
    const auto rank = [](const Leg& leg) {
        const int kind = leg.kind == 'M' ? 0 : (leg.kind == 'S' ? 1 : 4);
        return kind == 4 ? (leg.is_long ? 4 : 5) : kind + (leg.is_long ? 0 : 2);
    };
    struct PairTape {
        const char* tape;
        const std::vector<Cell>* cells;
        std::size_t both_traded;  // cells, over every pass, where both legs traded
    };
    const PairTape pair_tapes[] = {
        {"w6-f10a-open-pair", &kOpenPair, 28},
        {"w6-f10b-limit-class", &kLimitClass, 16},
        {"w6-f10c-same-side-class", &kSameSide, 16},
        {"w6-f10d-pooc-pair", &kClosePair, 16},
        {"w6-f10e-pooc-coof-pair", &kClosePair, 16},
        {"w6-f10f-coof-pair", &kClosePair, 16},
        {"w6-f10g-pair-gross", &kGross, 10},
        {"w6-f10h-pair-gross-pyramiding1", &kGross, 10},
        {"w6-f10i-pair-commission-percent", &kCommissionPair, 8},
        {"w6-f10j-pair-commission-per-order", &kCommissionPair, 8},
        {"w6b-p1d-open-pair-per-order", &kOpenPair, 28},
        {"w6b-p2a-open-pair-pyramiding2", &kOpenPair, 28},
        {"w6b-p2b-limit-class-pyramiding2", &kLimitClass, 16},
    };
    for (const PairTape& pt : pair_tapes) {
        const Tape tape = tape_trades(pt.tape, end_ms);
        std::map<std::int64_t, std::vector<std::size_t>> slots;
        for (std::size_t i = 0; i < tape.trades.size(); ++i) {
            const std::int64_t rel = std::get<0>(tape.trades[i]) - at(8, 0, 0);
            if (rel >= 0) slots[rel / kStepMs].push_back(i);
        }
        std::size_t both = 0;
        for (const auto& [slot, rows] : slots) {
            const Cell& cell = (*pt.cells)[static_cast<std::size_t>(slot) % pt.cells->size()];
            bool first_traded = false, second_traded = false;
            long long second_lots = 0;
            for (const std::size_t i : rows) {
                for (const std::string* signal : {&tape.entries[i], &tape.exits[i]}) {
                    first_traded = first_traded || *signal == cell.first.comment;
                    second_traded = second_traded || *signal == cell.second.comment;
                }
                if (tape.entries[i] == cell.second.comment) second_lots += std::get<3>(tape.trades[i]);
            }
            if (!first_traded || !second_traded) continue;
            ++both;
            const bool second_leads = rank(cell.second) < rank(cell.first);
            const Leg& lead = second_leads ? cell.second : cell.first;
            CHECK(tape.entries[rows.front()] == lead.comment);
            if (second_leads && cell.first.is_long != cell.second.is_long) {
                const double pending = cell.first.kind == 'M' ? cell.first.qty : 0.0;
                CHECK(second_lots == lots(cell.second.qty + pending));
            }
        }
        std::printf("   %s: %zu cells with both legs traded\n", pt.tape, both);
        CHECK(both == pt.both_traded);
    }

    // Three same-side legs under pyramiding 3 (w6b-p2c): the rows follow the
    // legs' rank -- market, stop, limit -- whatever order they were placed in.
    {
        const Tape tape = tape_trades("w6b-p2c-same-side-pyramiding3", end_ms);
        std::map<std::int64_t, std::vector<std::size_t>> slots;
        for (std::size_t i = 0; i < tape.trades.size(); ++i) {
            const std::int64_t rel = std::get<0>(tape.trades[i]) - at(8, 0, 0);
            if (rel >= 0) slots[rel / kStepMs].push_back(i);
        }
        std::size_t ordered = 0;
        for (const auto& [slot, rows] : slots) {
            const Cell& cell = kSameSideTriple[static_cast<std::size_t>(slot) % kSameSideTriple.size()];
            std::vector<Leg> legs = {cell.first, cell.second, cell.more.front()};
            std::stable_sort(legs.begin(), legs.end(),
                             [&](const Leg& a, const Leg& b) { return rank(a) < rank(b); });
            bool same = rows.size() == legs.size();
            for (std::size_t k = 0; same && k < legs.size(); ++k)
                same = tape.entries[rows[k]] == legs[k].comment;
            CHECK(same);
            if (same) ++ordered;
        }
        std::printf("   w6b-p2c-same-side-pyramiding3: %zu cells in rank order\n", ordered);
        CHECK(ordered == 24);
    }

    // The gross-admission bands, on TradingView's own rows: the later call
    // trades whenever its transaction at the signal close is within the
    // equity, however little of it the commission would leave (U), and not
    // past it (O), with or without a commission.
    std::printf("-- the admission, on the tapes\n");
    for (const char* name : {"w6b-p1a-pair-gross-commission-off", "w6b-p1b-pair-gross-per-order",
                             "w6b-p1c-pair-gross-percent"}) {
        const Tape tape = tape_trades(name, end_ms);
        std::map<int, std::pair<bool, bool>> traded;  // slot -> (first, second) traded
        for (std::size_t i = 0; i < tape.trades.size(); ++i) {
            const int slot = pair_cell(std::get<0>(tape.trades[i]), 18, 1, kHourMs);
            if (slot < 0) continue;
            const Cell& cell = kGrossBands[static_cast<std::size_t>(slot) % kGrossBands.size()];
            for (const std::string* signal : {&tape.entries[i], &tape.exits[i]}) {
                traded[slot].first = traded[slot].first || *signal == cell.first.comment;
                traded[slot].second = traded[slot].second || *signal == cell.second.comment;
            }
        }
        std::size_t kept = 0, dropped = 0;
        for (int slot = 0; slot < 18; ++slot) {
            const bool over = slot % 3 == 2;
            CHECK(traded[slot].first);
            CHECK(traded[slot].second == !over);
            (over ? dropped : kept) += traded[slot].second == !over ? 1 : 0;
        }
        std::printf("   %s: %zu later calls kept (W, U), %zu dropped (O)\n", name, kept, dropped);
        CHECK(kept == 12 && dropped == 6);
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
