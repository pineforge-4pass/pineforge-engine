/*
 * test_exit_queue_tapes.cpp — lane W3-ENG-EXIT-ALLOC (family F05).
 *
 * TradingView reserves an entry's quantity for its strategy.exit orders in
 * the order the exits were created, and re-issuing an exit modifies it in
 * place: it keeps its place in that queue. A later exit gets only what the
 * earlier ones leave, so of two exits without qty on one entry only the
 * first-created ever fills.
 *
 * The adapter already refused a new exit its elders had fully reserved, but a
 * re-issued exit counted every other live exit against itself, the ones
 * created after it included. Exits a script issues every bar while flat stay
 * live, unreserved; on the bar an explicit-quantity entry is placed under
 * process_orders_on_close the pending entry gives them a quantity, and the
 * first exit re-issued there lost the whole position to the stale later one,
 * which then filled instead (a stop never fired; its target did).
 *
 * Two companions ride on the same tapes. Exits for every entry
 * (from_entry="") armed before an opening queue for the new position the same
 * way; the adapter had left each reserving all of it (s11). And a reversed
 * side's from_entry="" exit that the reversal's fill parked off the book,
 * beside a second entry of the reversal bar, is void like one still on the
 * book (lane TVDEF-DROPS, R3); the adapter released it at the next open
 * against the new side (r1).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/exit_queue, lab tv exports on BINANCE:ETHUSDT.P 15) through
 * the Pine adapter under the configuration the generated constructor declares
 * for it, over the corpus 15m bars embedded in bars.inc, and requires every
 * trade the tape closes inside those bars to be the engine's: entry and exit
 * time, side, price and quantity. Then it reads the queue rule off
 * TradingView's own rows.
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

#ifndef PINEFORGE_EXIT_QUEUE_FIXTURE_DIR
#error "PINEFORGE_EXIT_QUEUE_FIXTURE_DIR must name tests/fixtures/exit_queue"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/exit_queue/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// TradingView's BINANCE:ETHUSDT.P quantity step and price tick.
constexpr double kLot = 0.0001;
constexpr double kTick = 0.01;
// The first cell (2025-04-08 00:00 UTC) places every probe's first entry; the
// harness trades from there, as run_strategy.py does for a corpus tape.
constexpr std::int64_t kTradeStartMs = 1744070400000LL;

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
    std::vector<Row> trades;           // closed inside the replayed bars, in entry order
    std::vector<std::string> signals;  // each trade's exit signal
};

Tape tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_EXIT_QUEUE_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
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
            out.signals.push_back(signal->second);
        }
    }
    return out;
}

enum class Probe {
    StopThenTarget,         // s1 / s5: far stop A, then near target B, every bar
    TargetThenStop,         // s2 / s6: the same two exits, B issued first
    HalfEach,               // s7: A 50%, then B 50%
    HalfThenFull,           // s8: A 50%, then B without qty
    FullThenHalf,           // s8b: A without qty, then B 50%
    TargetCreatedFirst,     // s10: B alone on the entry bar, then A then B while held
    AnyEntry,               // s11: A then B with from_entry=""
    SharedOca,              // s13: A then B, both oca_name="X"
    StopOnce,               // s14: A once on the entry bar, B every bar held
    CancelThenReissue,      // s15: A cancelled after an hour, issued again after two
    FirstEverStop,          // s16: A alone in the first trade; B first in later ones
    ExitBeforeEntryCall,    // s18 / s21: B once on the entry bar, before the entry
    PendingLimitParent,     // s20: B once while its limit entry rests
    TwoParentReversal,      // r1: two short entries reverse a long that re-issues XL
    ExitBeforeEntryBar,     // s17 / s19: B once, on the bar before each entry
    CancelFirst,            // s12: A then B every bar held; A cancelled after an hour
    StandingExit,           // v1: a void call over an exit issued while its entry was
                            //     resting or open
};

bool entry_cell(std::int64_t t) {
    return t == at(8, 0, 0) || t == at(9, 0, 0) || t == at(10, 0, 0) || t == at(11, 0, 0)
        || t == at(14, 0, 0);
}
bool next_cell(std::int64_t t) {
    return t == at(8, 0, 15) || t == at(9, 0, 15) || t == at(10, 0, 15) || t == at(11, 0, 15)
        || t == at(14, 0, 15);
}
// s17 / s19 issue their exit on the bar before each entry cell.
bool pre_cell(std::int64_t t) {
    return t == at(7, 23, 45) || t == at(8, 23, 45) || t == at(9, 23, 45)
        || t == at(10, 23, 45) || t == at(13, 23, 45);
}
bool cleanup_cell(std::int64_t t) {
    return t == at(8, 6, 0) || t == at(9, 6, 0) || t == at(10, 6, 0) || t == at(11, 6, 0)
        || t == at(14, 6, 0);
}
// r1 reverses at 00:30 and cleans up at 02:00.
bool reversal_cell(std::int64_t t) {
    return t == at(8, 0, 30) || t == at(9, 0, 30) || t == at(10, 0, 30) || t == at(11, 0, 30)
        || t == at(14, 0, 30);
}
bool early_cleanup_cell(std::int64_t t) {
    return t == at(8, 2, 0) || t == at(9, 2, 0) || t == at(10, 2, 0) || t == at(11, 2, 0)
        || t == at(14, 2, 0);
}

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(Probe probe, const source::PineStrategyConfig& config) : probe_(probe) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double close = current_bar_.close;
        const bool cell = entry_cell(t);
        switch (probe_) {
        case Probe::PendingLimitParent: pending_limit_parent(t, close, cell); return;
        case Probe::TwoParentReversal: two_parent_reversal(t, close); return;
        case Probe::StandingExit: standing_exit(t); return;
        case Probe::ExitBeforeEntryCall:
            if (cell) {
                exit("B", "L", close + 6.0, kNaN, 100.0, "B target");
                strategy_entry("L", true, kNaN, kNaN, 1, "L", "", 0, -1);
            }
            if (cleanup_cell(t)) strategy_close("", "cleanup", kNaN, kNaN, false);
            return;
        case Probe::ExitBeforeEntryBar:
            if (pre_cell(t)) exit("B", "L", close + 6.0, kNaN, 100.0, "B target");
            if (cell) strategy_entry("L", true, kNaN, kNaN, 1, "L", "", 0, -1);
            if (cleanup_cell(t)) strategy_close("", "cleanup", kNaN, kNaN, false);
            return;
        default: break;
        }
        if (cell) {
            strategy_entry("L", true, kNaN, kNaN, 1, "L", "", 0, -1);
            ref_ = close;
            entry_bar_ = pine_bar_index();
            ++trades_;
        }
        const bool have_ref = !std::isnan(ref_);
        const bool held = signed_position_size() > 0.0 || cell;
        switch (probe_) {
        case Probe::StopThenTarget:
            if (have_ref) {
                exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
                exit("B", "L", ref_ + 4.0, kNaN, 100.0, "B target");
            }
            break;
        case Probe::TargetThenStop:
            if (have_ref) {
                exit("B", "L", ref_ + 4.0, kNaN, 100.0, "B target");
                exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
            }
            break;
        case Probe::HalfEach:
            if (have_ref) {
                exit("A", "L", kNaN, ref_ - 40.0, 50.0, "A stop");
                exit("B", "L", ref_ + 4.0, kNaN, 50.0, "B target");
            }
            break;
        case Probe::HalfThenFull:
            if (have_ref) {
                exit("A", "L", kNaN, ref_ - 40.0, 50.0, "A stop");
                exit("B", "L", ref_ + 4.0, kNaN, 100.0, "B target");
            }
            break;
        case Probe::FullThenHalf:
            if (have_ref) {
                exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
                exit("B", "L", ref_ + 4.0, kNaN, 50.0, "B target");
            }
            break;
        case Probe::TargetCreatedFirst:
            if (held && pine_bar_index() > entry_bar_)
                exit("A", "L", kNaN, ref_ - 8.0, 100.0, "A stop");
            if (held) exit("B", "L", ref_ + 40.0, kNaN, 100.0, "B target");
            break;
        case Probe::AnyEntry:
            if (have_ref) {
                exit("A", "", kNaN, ref_ - 40.0, 100.0, "A stop");
                exit("B", "", ref_ + 4.0, kNaN, 100.0, "B target");
            }
            break;
        case Probe::SharedOca:
            if (have_ref) {
                exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop", "X");
                exit("B", "L", ref_ + 4.0, kNaN, 100.0, "B target", "X");
            }
            break;
        case Probe::StopOnce:
            if (cell) exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
            if (held) exit("B", "L", ref_ + 4.0, kNaN, 100.0, "B target");
            break;
        case Probe::CancelThenReissue: {
            const int age = pine_bar_index() - entry_bar_;
            if (held && age == 4) strategy_cancel("A");
            if (held && age < 4) exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
            if (held && age >= 8) exit("A", "L", kNaN, close + 50.0, 100.0, "A stop again");
            if (held) exit("B", "L", ref_ + 100.0, kNaN, 100.0, "B target");
            break;
        }
        case Probe::CancelFirst:
            if (cell) cancelled_ = false;
            if (have_ref && !cancelled_ && pine_bar_index() - entry_bar_ >= 4) {
                strategy_cancel("A");
                cancelled_ = true;
            }
            if (have_ref && !cancelled_) exit("A", "L", kNaN, ref_ - 40.0, 100.0, "A stop");
            if (have_ref) exit("B", "L", ref_ + 12.0, kNaN, 100.0, "B target");
            break;
        case Probe::FirstEverStop:
            if (held && trades_ == 1) exit("A", "L", kNaN, ref_ - 8.0, 100.0, "A stop");
            if (held && trades_ > 1 && pine_bar_index() > entry_bar_)
                exit("A", "L", kNaN, ref_ - 8.0, 100.0, "A stop");
            if (held && trades_ > 1) exit("B", "L", ref_ + 40.0, kNaN, 100.0, "B target");
            break;
        default: break;
        }
        if (cleanup_cell(t)) strategy_close("", "cleanup", kNaN, kNaN, false);
    }

private:
    void exit(const char* id, const char* from, double limit, double stop, double percent,
              const char* comment, const char* oca = "") {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, percent, comment, kNaN, oca,
                      kNaN, kNaN);
    }

    void two_parent_reversal(std::int64_t t, double close) {
        if (entry_cell(t)) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (reversal_cell(t)) {
            strategy_entry("S1", false, kNaN, kNaN, kNaN, "S1");
            strategy_entry("S2", false, kNaN, kNaN, kNaN, "S2");
        }
        // strategy.position_avg_price: na while flat.
        const double avg = signed_position_size() == 0.0 ? kNaN : position_entry_price_;
        if (signed_position_size() > 0.0)
            exit("XL", "", close + 20.0, close - 20.0, 100.0, "long exit");
        if (signed_position_size() < 0.0)
            exit("XS", "", avg - 15.0, avg + 15.0, 100.0, "short exit");
        if (early_cleanup_cell(t)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

    // v1, one cell a day: X issued while limit entry L rests, L cancelled, X
    // re-issued while void, L entered at market (04-08); the same without the
    // void call (04-09); X issued behind a market L, the position closed, X
    // re-issued while void, L entered again (04-10); the same without the
    // void call (04-11).
    void standing_exit(std::int64_t t) {
        if (t == at(8, 0, 0)) {
            strategy_entry("L", true, 1500.0, kNaN, kNaN, "L");
            exit("X", "L", 1570.0, kNaN, 100.0, "X first");
        }
        if (t == at(8, 0, 15)) strategy_cancel("L");
        if (t == at(8, 0, 30)) exit("X", "L", 1600.0, kNaN, 100.0, "X void");
        if (t == at(8, 0, 45)) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (t == at(9, 0, 0)) {
            strategy_entry("L", true, 1450.0, kNaN, kNaN, "L");
            exit("X", "L", kNaN, 1440.0, 100.0, "X first");
        }
        if (t == at(9, 0, 15)) strategy_cancel("L");
        if (t == at(9, 0, 45)) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (t == at(10, 0, 0)) {
            strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
            exit("X", "L", kNaN, 1635.0, 100.0, "X first");
        }
        if (t == at(10, 0, 15)) strategy_close("", "flat", kNaN, kNaN, false);
        if (t == at(10, 0, 45)) exit("X", "L", kNaN, 1620.0, 100.0, "X void");
        if (t == at(10, 1, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (t == at(11, 0, 0)) {
            strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
            exit("X", "L", 1545.0, kNaN, 100.0, "X first");
        }
        if (t == at(11, 0, 15)) strategy_close("", "flat", kNaN, kNaN, false);
        if (t == at(11, 1, 0)) strategy_entry("L", true, kNaN, kNaN, kNaN, "L");
        if (t == at(8, 3, 0) || t == at(9, 3, 0) || t == at(10, 4, 0) || t == at(11, 4, 0))
            strategy_close("", "cleanup", kNaN, kNaN, false);
    }

    void pending_limit_parent(std::int64_t t, double close, bool cell) {
        if (cell) {
            level_ = close - 15.0;
            strategy_entry("L", true, level_, kNaN, 1, "L", "", 0, -1);
        }
        if (next_cell(t) && signed_position_size() == 0.0)
            exit("B", "L", level_ + 20.0, kNaN, 100.0, "B target");
        if (cleanup_cell(t)) {
            strategy_cancel_all();
            strategy_close("", "cleanup", kNaN, kNaN, false);
        }
    }

    Probe probe_;
    double ref_ = kNaN;
    double level_ = kNaN;
    int entry_bar_ = 0;
    int trades_ = 0;
    bool cancelled_ = false;
};

struct Run {
    std::vector<Row> trades;
    std::string error;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Run run(Probe probe, const source::PineStrategyConfig& config, std::int64_t end_ms) {
    ProbeHost host(probe, config);
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

// Every probe omits the capital (v6: 100000) and declares strategy.fixed 1,
// but r1, which sizes 40% of equity.
source::PineStrategyConfig config(bool process_orders_on_close, QtyType type, double value) {
    source::PineStrategyConfig c{};
    c.process_orders_on_close = process_orders_on_close;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(type);
    c.default_qty_value = value;
    return c;
}

struct Case {
    const char* tape;
    Probe probe;
    bool pooc;
    std::size_t closed;  // tape trades closed inside the bars
    QtyType type = QtyType::FIXED;
    double value = 1.0;
    // Trades entered before this are TradingView's evidence only, not
    // compared with the engine's (v1: its first two cells are an open
    // finding, see the fixture README).
    std::int64_t compare_from = 0;
};

std::vector<Row> entered_from(const std::vector<Row>& rows, std::int64_t from) {
    std::vector<Row> out;
    for (const Row& r : rows)
        if (std::get<0>(r) >= from) out.push_back(r);
    return out;
}

// Every trade of the tape that exits by `signal`.
std::size_t count_signal(const Tape& tape, const std::string& signal) {
    std::size_t n = 0;
    for (const auto& s : tape.signals) n += s == signal;
    return n;
}

}  // namespace

int main() {
    const std::int64_t end_ms = kEth15[sizeof(kEth15) / sizeof(kEth15[0]) - 1].ts;

    const Case cases[] = {
        {"w3f05-s1-stop-then-target", Probe::StopThenTarget, false, 5},
        {"w3f05-s2-target-then-stop", Probe::TargetThenStop, false, 5},
        {"w3f05-s5-pooc-qty-stop-then-target", Probe::StopThenTarget, true, 5},
        {"w3f05-s6-pooc-qty-target-then-stop", Probe::TargetThenStop, true, 5},
        {"w3f05-s7-pooc-qty-half-each", Probe::HalfEach, true, 10},
        {"w3f05-s8-pooc-qty-half-then-full", Probe::HalfThenFull, true, 10},
        {"w3f05-s8b-pooc-qty-full-then-half", Probe::FullThenHalf, true, 5},
        {"w3f05-s10-pooc-qty-target-created-first", Probe::TargetCreatedFirst, true, 5},
        {"w3f05-s11-pooc-qty-any-entry", Probe::AnyEntry, true, 5},
        {"w3f05-s13-pooc-qty-shared-oca", Probe::SharedOca, true, 5},
        {"w3f05-s14-pooc-qty-stop-once", Probe::StopOnce, true, 5},
        {"w3f05-s15-pooc-qty-cancel-then-redeclare", Probe::CancelThenReissue, true, 5},
        {"w3f05-s16-pooc-qty-first-ever-stop", Probe::FirstEverStop, true, 5},
        {"w3f05-s18-pooc-qty-exit-before-entry-call", Probe::ExitBeforeEntryCall, true, 5},
        {"w3f05-s20-exit-for-pending-limit-entry", Probe::PendingLimitParent, false, 4},
        {"w3f05-s21-exit-before-entry-call-nextopen", Probe::ExitBeforeEntryCall, false, 5},
        {"w3f05-r1-two-parent-reversal-global-exit", Probe::TwoParentReversal, false, 10,
         QtyType::PERCENT_OF_EQUITY, 40.0},
        // Lane W3B-ENG-GRID: an exit whose entry neither exists nor rests is void.
        {"w3f05-s17-pooc-qty-exit-before-entry", Probe::ExitBeforeEntryBar, true, 5},
        {"w3f05-s19-exit-before-entry-nextopen", Probe::ExitBeforeEntryBar, false, 5},
        {"w3f05-s12-pooc-qty-cancel-first", Probe::CancelFirst, true, 5},
        // Lane W3B-ENG-GRID: a void call places nothing and leaves a standing
        // exit as it is; an exit does not outlive its trade.
        {"w3bf05-v1-void-call-over-standing-exit", Probe::StandingExit, false, 6,
         QtyType::FIXED, 1.0, at(10, 0, 0)},
    };

    std::map<std::string, Tape> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s\n", c.tape);
        const Tape tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);

        const Run lane = run(c.probe, config(c.pooc, c.type, c.value), end_ms);
        CHECK(lane.error.empty());
        const std::vector<Row> want = entered_from(tape.trades, c.compare_from);
        const std::vector<Row> got = entered_from(lane.trades, c.compare_from);
        CHECK(!want.empty());
        CHECK(got == want);
        if (got != want) {
            show("tape", want);
            show("engine", got);
        }
    }

    // The queue rule, read off TradingView's own rows.
    std::printf("-- the queue rule, on the tapes\n");
    {
        // Two full exits: only the first created ever fills, whichever level
        // the market reaches first -- in both declaration orders, with and
        // without process_orders_on_close, and when both share an OCA name.
        for (const char* name : {"w3f05-s1-stop-then-target", "w3f05-s5-pooc-qty-stop-then-target",
                                 "w3f05-s11-pooc-qty-any-entry", "w3f05-s13-pooc-qty-shared-oca"}) {
            CHECK(count_signal(tapes[name], "B target") == 0);
            CHECK(count_signal(tapes[name], "A stop") == 2);
        }
        for (const char* name : {"w3f05-s2-target-then-stop", "w3f05-s6-pooc-qty-target-then-stop"}) {
            CHECK(count_signal(tapes[name], "A stop") == 0);
            CHECK(count_signal(tapes[name], "B target") == 4);
        }
        // Partial exits: a later exit gets what the earlier ones leave, and
        // nothing behind a full one.
        CHECK(count_signal(tapes["w3f05-s7-pooc-qty-half-each"], "B target") == 4);
        CHECK(count_signal(tapes["w3f05-s8-pooc-qty-half-then-full"], "B target") == 4);
        CHECK(count_signal(tapes["w3f05-s8b-pooc-qty-full-then-half"], "B target") == 0);
        // Creation, not the order of the calls on a bar: B, created on the
        // entry bar, keeps the position although A is issued before it on
        // every later bar; a cancelled A issued again is created anew, behind
        // B (its marketable stop never fills); an A of an earlier trade does
        // not carry its place into later ones.
        CHECK(count_signal(tapes["w3f05-s10-pooc-qty-target-created-first"], "A stop") == 0);
        CHECK(count_signal(tapes["w3f05-s15-pooc-qty-cancel-then-redeclare"], "A stop again") == 0);
        CHECK(count_signal(tapes["w3f05-s16-pooc-qty-first-ever-stop"], "A stop") == 1);
        CHECK(count_signal(tapes["w3f05-s14-pooc-qty-stop-once"], "B target") == 0);
        // An exit issued before its entry exists is void; one issued while
        // its limit entry rests is the entry's when it fills.
        CHECK(count_signal(tapes["w3f05-s18-pooc-qty-exit-before-entry-call"], "B target") == 0);
        CHECK(count_signal(tapes["w3f05-s21-exit-before-entry-call-nextopen"], "B target") == 0);
        CHECK(count_signal(tapes["w3f05-s20-exit-for-pending-limit-entry"], "B target") == 2);
        // Issued a bar before its entry exists, an exit is void as well: it
        // never waits for the entry (s17, s19). A script that keeps issuing
        // an exit while flat therefore gives the next trade's first-issued
        // exit the queue front (s12: A, issued first on the entry bar, fills
        // its stop inside the hour; B fills once A is cancelled).
        CHECK(count_signal(tapes["w3f05-s17-pooc-qty-exit-before-entry"], "B target") == 0);
        CHECK(count_signal(tapes["w3f05-s19-exit-before-entry-nextopen"], "B target") == 0);
        CHECK(count_signal(tapes["w3f05-s12-pooc-qty-cancel-first"], "A stop") == 1);
        CHECK(count_signal(tapes["w3f05-s12-pooc-qty-cancel-first"], "B target") == 3);
        // A void call places nothing and leaves an exit of the same id,
        // issued while its entry rested, as it is: X's first level (1570)
        // fills after L is cancelled and entered again, not the void call's
        // (1600); without the void call, X outlives L's cancel too (1440).
        // An exit does not outlive its trade: issued behind a market entry,
        // X never fills in the next trade of that entry, re-issued while
        // void (04-10) or not (04-11).
        const Tape& v1 = tapes["w3bf05-v1-void-call-over-standing-exit"];
        CHECK(v1.trades.size() == 6);
        if (v1.trades.size() == 6) {
            CHECK(v1.signals[0] == "X first");
            CHECK(std::get<5>(v1.trades[0]) == ticks(1570.0));
            CHECK(v1.signals[1] == "X first");
            CHECK(std::get<5>(v1.trades[1]) == ticks(1440.0));
            CHECK(v1.signals[2] == "flat");
            CHECK(v1.signals[3] == "cleanup");
            CHECK(v1.signals[4] == "flat");
            CHECK(v1.signals[5] == "cleanup");
        }
        // A from_entry="" exit of the side a reversal ends never closes the
        // new side, even beside a second entry of the reversal bar.
        const Tape& reversal = tapes["w3f05-r1-two-parent-reversal-global-exit"];
        std::size_t shorts = 0;
        for (std::size_t i = 0; i < reversal.trades.size(); ++i) {
            if (std::get<1>(reversal.trades[i])) continue;
            ++shorts;
            CHECK(reversal.signals[i] != "long exit");
        }
        CHECK(shorts == 5);
    }

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
