/*
 * test_int28_rule_compositions.cpp — INT28, the wave-J integration.
 *
 * Git merged every wave-J lane into src/source/pine_adapter.cpp without a
 * textual conflict, but several lanes changed the same functions, and their
 * rules can act in one run -- and, in on_applied, at one fill:
 *
 * - on_applied: TVDEF-DROPS R3 (a reversal voids the reversed side's
 *   from_entry="" exits), W3-ENG-EXIT-ALLOC rules B and C (exits for every
 *   entry armed before an opening queue for it; a reversal voids the reversed
 *   side's global exit off the book too), W5-ENG-MARGIN-V6 M2, C2 and MK (the
 *   opening margin calls of process_orders_on_close and slipped market
 *   positions) and W8E-EXITS C1 (the KI-62 same-id cover);
 * - the exit-reservation functions (W3 rule A: exits reserve an entry's
 *   quantity in the order they were created) beside W5's MR (a global exit is
 *   the held position's under a declined reversal) and W8E C1;
 * - entry: W5 M1 (a default percent add judged at placement), W6-ENG-FILL-
 *   ORDER F10 (a flat opposite pair at one fill point) and W8D-NOTRADES (an
 *   entry a mid-leg calc_on_order_fills recalculation places). Their scopes
 *   exclude one another: M1 needs a percent_of_equity default and a held
 *   position of the add's side, outside process_orders_on_close and
 *   calc_on_order_fills; F10 needs a flat book in same_bar_market_tx_scope(),
 *   which admits a percent default only beside a held short seed and never
 *   calc_on_order_fills; W8D needs a calc_on_order_fills recalculation. No
 *   script reaches two of them, so no tape here combines them.
 *
 * Each row replays TradingView's own tape of a synthetic probe -- c1 to c3
 * written for INT28 (tests/fixtures/int28_compositions, lab tv --no-note
 * exports on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-17), c4 lane
 * R1-CONSOLIDATE's w8a-dca-open (tests/fixtures/open_fill_order, the same
 * chart) -- through the Pine host under the configuration the generated
 * constructor declares for it, over the corpus 15m bars of
 * tests/fixtures/margin_v6/bars.inc, and requires every trade the tape closes
 * to be the engine's: entry and exit time, side, price and quantity. An
 * instrumented copy of the adapter (INT28's census, scratch only) names the
 * rules that act in each row's run:
 *
 * - c1 (W3's r1 cells at v6's defaults): W3 rule C 9 times, W5 MR 24 times
 *   and W4 F19a 10 times -- where the next open cannot fund the 100% reversal
 *   it is declined and the long keeps its global exit; where it can, the
 *   reversal voids it;
 * - c2: W3 rule B and W5 C2 at each of the eight openings, at the same fill;
 * - c3, a control: the KI-62 cover stands behind a queued explicit exit pair
 *   (neither W3's nor W8E's change acts; main books the tape too);
 * - c4: W8A R-B admits the entries a pending limit no longer blocks (E3's P3,
 *   E4b's HM) and R1-CONSOLIDATE's R-A fills the market orders at an opening
 *   price before the buy limits the open reached, lowest limit first (E1,
 *   E2a, E4b) -- in E4b both act on one fill point. Each lane's own test
 *   reads that cell off TradingView's rows only: R-A's tree does not carry
 *   R-B, and W8A's test compares the trades in any order.
 *
 * c1 and c2 fail on main 962960b3 and after TVDEF-DROPS, and pass from W3's
 * picks on; W5's rules act in them without moving a trade TradingView books.
 * c4 fails on main and after W8A's picks (the engine books E1 as B, A, C,
 * closes E2a's L1 at once and fills E4b's HL before HM), and passes from
 * R-A's pick on.
 *
 * The late picks meet the same functions: W4-ENG-POOC-SAMEPASS (on_applied's
 * close-pass rerun, exit(), entry(), on_bar_open and on_bar_close),
 * W6B-ENG-PAIRS (on_applied's opening margin checkpoint and entry()) and
 * W8A-SIGSTATE-1 (on_applied's intraday-loss check, entry() and
 * on_bar_open). Every lane's own tape suite passes on the integrated tree,
 * and the census finds two lanes' rules acting in one run of those tapes:
 * W4 MCSIZE with W5 M2 (pooc_margin_call_sizing w4-mcs-pooc), W4 F08 with
 * W5 M2 (w4-f19c-short-pooc, w4-rex-pooc), W4 F19a with W5 MR and
 * TVDEF-DROPS R3 (margin_v6 w5-mr-global, w5-mr-once-global), W4 F08 and DORM
 * with W5 M2 and TVDEF-DROPS R3 (pooc_samepass w4-f08b-pooc), W8A R2 with W5
 * M2 (pooc_same_pass_close w8a-pooc-sameside-closeall) and W6B's checkpoint
 * with W6 F10 (same_point_entries w6-f10a, w6-f10b). W6B's checkpoint is
 * scope-disjoint from W5's margin rules at that fill: same_point_pair_scope()
 * excludes process_orders_on_close and slippage, which W5 C2 and M2, and MK,
 * require. W8A R3 and W5 M1, inserted at one place in entry(), are too: R3
 * checks a default FIXED LIMIT entry from flat, M1 a default percent MARKET
 * add to a held position.
 *
 * R1-CONSOLIDATE's R1 (entry()'s same-bar batch, flush_pending_same_bar_
 * commands() and resolve_terms()) acts beside W4 DORM on its own tapes
 * (same_bar_reissue w8a-dbl-p10, w8a-dbl-scope3, w8b-flip-double-bracket)
 * and beside W4 F08 and W5 M2 (w8b-flip-double-pooc), all green; it only
 * reverses a held position, while W6 F10's and W6B's pair rules, which share
 * the batch's same_bar_market_tx_scope(), act from flat. R-A (on_bar_open)
 * leaves a flat book holding MARKET entries of both sides to the pair route,
 * so on its r1c-ra-prior-limit tape W6B's checkpoint acts in the pair cells
 * and R-A in the others.
 *
 * W3B-ENG-GRID, picked right after W3, meets two later rules, each composed
 * in the later rule's pick and pinned by W3B's own tapes in
 * tests/test_exit_queue_tapes.cpp, which fail on the uncomposed merge:
 * - W4 F19a sizes a from_entry "" exit without a quantity when it fills, if
 *   it is the book's only exit; W3B's rule E gives a full global exit what a
 *   partial one ahead of it leaves. On w3bf05-g8, once target B filled its
 *   half, stop A (50%) was the only exit and F19a sized it at half the half;
 *   TradingView fills the whole other half. A partial exit sized at its fill
 *   keeps its placement reservation (the larger of the two), so an add still
 *   counts (w4-f19a cell E) and a filled sibling no longer shrinks it.
 * - W5 MR holds a from_entry "" exit under a declined reversal. On
 *   w3bf05-r1c (calc_on_order_fills) S1 reverses the long and S2, rejected
 *   at the same open, reads as a declined reversal of that long; MR held the
 *   short's own exit XS, placed in the recalculation after S1's fill, past
 *   its entry-bar limit. A global exit placed under the side the rejected
 *   entry would open is not the held position's, as a named one of that side
 *   already was not.
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

#ifndef PINEFORGE_INT28_COMPOSITIONS_FIXTURE_DIR
#error "PINEFORGE_INT28_COMPOSITIONS_FIXTURE_DIR must name tests/fixtures/int28_compositions"
#endif
#ifndef PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR
#error "PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR must name tests/fixtures/open_fill_order"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// BINANCE:ETHUSDT.P 15m, 2025-04-01 00:00 .. 2025-04-17 00:00 UTC.
#include "fixtures/margin_v6/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kLot = 0.0001;  // TradingView's BINANCE:ETHUSDT.P quantity step
constexpr std::int64_t kMinute = 60'000;

// (entry ms, long, entry price, quantity, exit ms, exit price), prices in
// ticks and quantity in lots.
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

// timestamp("UTC", 2025, 4, day, hour, minute)
std::int64_t at(unsigned day, int hour, int minute) {
    return ((days_from_civil(2025, 4, day) * 24 + hour) * 60 + minute) * kMinute;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct Tape {
    std::vector<Row> trades;  // in trade-number order
    std::vector<std::string> entries;
    std::vector<std::string> exits;
};

Tape read_tape(const std::string& dir, const std::string& tape) {
    std::ifstream in(dir + "/" + tape + "/tv_trades.csv");
    CHECK(in.good());
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
        if (cell.size() < 6) continue;
        const int number = std::stoi(cell[0]);
        Row& row = by_number[number];
        if (cell[1].rfind("Entry", 0) == 0) {
            std::get<0>(row) = tape_ms(cell[2]);
            std::get<1>(row) = cell[1] == "Entry long";
            std::get<2>(row) = ticks(std::stod(cell[4]));
            std::get<3>(row) = lots(std::stod(cell[5]));
            entry_signal[number] = cell[3];
        } else {
            std::get<4>(row) = tape_ms(cell[2]);
            std::get<5>(row) = ticks(std::stod(cell[4]));
            exit_signal[number] = cell[3];
        }
    }
    Tape out;
    for (const auto& [number, row] : by_number) {
        out.trades.push_back(row);
        out.entries.push_back(entry_signal[number]);
        out.exits.push_back(exit_signal[number]);
    }
    return out;
}

enum class Probe { TwoParentReversal, PoocGlobalPairOpening, ExitPairSameIdAdd, DcaOpen };

// The probes, as their generated TUs lower them
// (tests/fixtures/int28_compositions/*/strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config) : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        switch (probe_) {
        case Probe::TwoParentReversal: two_parent_reversal(t); break;
        case Probe::PoocGlobalPairOpening: pooc_global_pair_opening(t); break;
        case Probe::ExitPairSameIdAdd: exit_pair_same_id_add(t); break;
        case Probe::DcaOpen: dca_open(t); break;
        }
    }

private:
    void close_all(const char* comment) { strategy_close("", comment, kNaN, kNaN, false); }

    // int28-c1-two-parent-reversal-v6
    void two_parent_reversal(std::int64_t t) {
        const std::int64_t minute_of_day = (t / kMinute) % (24 * 60);
        // strategy.position_avg_price: na while flat.
        const double avg = signed_position_size() == 0.0 ? kNaN : position_entry_price_;
        const double close = current_bar_.close;
        if (minute_of_day == 0) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (minute_of_day == 30) {
            strategy_entry("S1", false, kNaN, kNaN, kNaN, "S1");
            strategy_entry("S2", false, kNaN, kNaN, kNaN, "S2");
        }
        if (signed_position_size() > 0.0)
            strategy_exit("XL", "", close + 20.0, close - 20.0, kNaN, kNaN, kNaN, 100.0,
                          "long exit");
        if (signed_position_size() < 0.0)
            strategy_exit("XS", "", avg - 15.0, avg + 15.0, kNaN, kNaN, kNaN, 100.0,
                          "short exit");
        if (minute_of_day == 120) {
            strategy_cancel_all();
            close_all("cleanup");
        }
    }

    // int28-c2-pooc-global-pair-opening
    void pooc_global_pair_opening(std::int64_t t) {
        static constexpr unsigned kDay[] = {2, 2, 3, 4, 8, 9, 10, 14};
        static constexpr int kHour[] = {0, 9, 21, 12, 0, 0, 0, 0};
        bool entry = false, cleanup = false;
        for (std::size_t i = 0; i < std::size(kDay); ++i) {
            entry = entry || t == at(kDay[i], kHour[i], 0);
            cleanup = cleanup || t == at(kDay[i], kHour[i], 0) + 120 * kMinute;
        }
        if (entry) {
            strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
            ref_ = current_bar_.close;
        }
        if (!std::isnan(ref_) && !cleanup) {
            strategy_exit("A", "", ref_ + 40.0, ref_ - 30.0, kNaN, kNaN, kNaN, 100.0, "A");
            strategy_exit("B", "", ref_ + 6.0, kNaN, kNaN, kNaN, kNaN, 100.0, "B");
        }
        if (cleanup) {
            close_all("cleanup");
            ref_ = kNaN;
        }
    }

    // int28-c3-exit-queue-explicit-pair-add
    void exit_pair_same_id_add(std::int64_t t) {
        if (t == at(1, 11, 30)) {
            strategy_entry("A", true, kNaN, kNaN, 1);
            strategy_exit("AX1", "A", 1869.55, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
            strategy_exit("AX2", "A", 1868.0, 1800.0, kNaN, kNaN, kNaN, 100.0, {}, 1);
        }
        if (t == at(1, 12, 0)) strategy_entry("A", true, kNaN, kNaN, 1);
        if (t == at(1, 13, 30)) strategy_close("");
    }

    // w8a-dca-open (tests/fixtures/open_fill_order), as its generated TU
    // lowers it: explicit quantities, cells placed at one close and filled
    // at the next open.
    void dca_open(std::int64_t t) {
        const double close = current_bar_.close;
        const auto entry = [&](const char* id, double qty, double limit) {
            strategy_entry(id, true, limit, kNaN, qty, "", "", 0, -1);
        };
        const auto cleanup = [&] {
            strategy_cancel_all();
            close_all("");
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
            close_all("");
        }
        if (t == at(15, 8, 0)) cleanup();
        if (t == at(15, 10, 0)) entry("X2", 1, kNaN);
        if (t == at(15, 11, 0)) {
            close_all("");
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

    Probe probe_;
    double ref_ = kNaN;  // var float ref (int28-c2)
};

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

std::vector<Row> run(Probe probe, const source::PineStrategyConfig& config,
                     const std::vector<Bar>& bars, std::string* error,
                     std::vector<std::string>* ids) {
    ProbeHost host(probe, config);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    *error = host.last_error();
    std::vector<Row> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= bars.back().timestamp) continue;  // open at the range end
        out.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                         t.exit_time, ticks(t.exit_price));
        ids->push_back(t.entry_id);
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

// What a probe's strategy() declares, over Pine v6's defaults (capital
// 100000, 100 % of equity, pyramiding 1, margin 100 both ways).
source::PineStrategyConfig v6(bool pooc = false, double commission = 0.0) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = pooc;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 1;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission;
    return c;
}

source::PineStrategyConfig fixed_pyramiding2() {
    source::PineStrategyConfig c{};
    c.initial_capital = 1000000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 2;
    return c;
}

// w8a-dca-open's strategy(): a cash default, pyramiding 3 (every call names
// its quantity).
source::PineStrategyConfig cash_pyramiding3() {
    source::PineStrategyConfig c{};
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::CASH);
    c.default_qty_value = 100.0;
    c.pyramiding = 3;
    return c;
}

std::size_t count_exits(const Tape& tape, const std::string& signal) {
    return static_cast<std::size_t>(std::count(tape.exits.begin(), tape.exits.end(), signal));
}

}  // namespace

int main() {
    const std::vector<Bar> bars = feed();
    const char* int28 = PINEFORGE_INT28_COMPOSITIONS_FIXTURE_DIR;
    struct Case {
        const char* rules;
        const char* dir;
        const char* tape;
        Probe probe;
        source::PineStrategyConfig lane;
        std::size_t trades;
    };
    const Case cases[] = {
        {"W3 rule C + W5 MR, two-parent reversals of a long holding a global exit", int28,
         "int28-c1-two-parent-reversal-v6", Probe::TwoParentReversal, v6(), 40},
        {"W3 rule B + W5 C2 at one commissioned process_orders_on_close opening", int28,
         "int28-c2-pooc-global-pair-opening", Probe::PoocGlobalPairOpening, v6(true, 0.1), 8},
        {"W3 rule A + W8E C1, an explicit exit pair and a same-bar same-id add", int28,
         "int28-c3-exit-queue-explicit-pair-add", Probe::ExitPairSameIdAdd, fixed_pyramiding2(),
         2},
        {"R1-CONSOLIDATE R-A + W8A R-B, the orders one open fills under pyramiding 3",
         PINEFORGE_OPEN_FILL_ORDER_FIXTURE_DIR, "w8a-dca-open", Probe::DcaOpen,
         cash_pyramiding3(), 18},
    };
    std::map<std::string, Tape> tapes;
    std::map<std::string, std::vector<std::string>> engine_ids;
    for (const Case& c : cases) {
        std::printf("-- %s: %s\n", c.rules, c.tape);
        const Tape tape = read_tape(c.dir, c.tape);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.trades);
        std::string error;
        const std::vector<Row> engine = run(c.probe, c.lane, bars, &error, &engine_ids[c.tape]);
        CHECK(error.empty());
        CHECK(engine == tape.trades);
        if (engine != tape.trades) {
            show("tape", tape.trades);
            show("engine", engine);
        }
    }

    std::printf("-- the compositions, on the tapes\n");
    {
        // c1: every cell's long either reverses into S1 (the second parent S2
        // never fills at pyramiding 1) or holds, its global exit XL alive, where
        // the 100% reversal is declined; the shorts are margin-called, and XL
        // and XS fill beside them.
        const Tape& c1 = tapes["int28-c1-two-parent-reversal-v6"];
        CHECK(count_exits(c1, "S1") == 9 && count_exits(c1, "long exit") == 2);
        CHECK(count_exits(c1, "short exit") == 8 && count_exits(c1, "Margin call") == 16);
        CHECK(count_exits(c1, "cleanup") == 5);
        CHECK(std::count(c1.entries.begin(), c1.entries.end(), std::string("S2")) == 0);
        // c2: B, a nearer full target armed behind A before each opening, is
        // left nothing; A fills at its stop or target, and no margin call is
        // booked at any opening fill.
        const Tape& c2 = tapes["int28-c2-pooc-global-pair-opening"];
        CHECK(count_exits(c2, "B") == 0 && count_exits(c2, "A") == 2);
        CHECK(count_exits(c2, "Margin call") == 0);
        // c3: AX1 (qty 1, created first) closes the older A lot and covers the
        // same-bar add at its own price; AX2 (qty 1, created after it, a nearer
        // target) never fills.
        const Tape& c3 = tapes["int28-c3-exit-queue-explicit-pair-add"];
        if (c3.trades.size() == 2) {
            CHECK(c3.exits[0] == "AX1" && c3.exits[1] == "AX1");
            CHECK(std::get<5>(c3.trades[0]) == std::get<5>(c3.trades[1]));
            CHECK(std::get<0>(c3.trades[1]) == std::get<4>(c3.trades[1]));
        }
        // c4: in E4b, with two lots open at pyramiding 3, W8A R-B admits HM, a
        // market entry placed after the limit HL (the pending limit does not
        // count), and R-A fills every market order at the opening price before
        // the buy limits the open reached: HM, then HL. In E3 R-B admits P3
        // beside P1 and P2 (four lots); in E1 R-A fills the limits lowest
        // first, B, C, A. The engine books every trade under its entry id.
        const Tape& c4 = tapes["w8a-dca-open"];
        CHECK(engine_ids["w8a-dca-open"] == c4.entries);
        const auto index = [&](const char* id) {
            return static_cast<std::size_t>(
                std::find(c4.entries.begin(), c4.entries.end(), std::string(id))
                - c4.entries.begin());
        };
        CHECK(index("HL") < c4.trades.size() && index("HM") + 1 == index("HL")
              && std::get<0>(c4.trades[index("HM")]) == std::get<0>(c4.trades[index("HL")]));
        CHECK(index("P3") < c4.trades.size() && index("P1") + 2 == index("P3")
              && std::get<0>(c4.trades[index("P1")]) == std::get<0>(c4.trades[index("P3")]));
        CHECK(index("A") < c4.trades.size() && index("B") + 1 == index("C")
              && index("C") + 1 == index("A"));
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
