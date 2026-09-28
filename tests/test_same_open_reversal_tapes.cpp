/*
 * test_same_open_reversal_tapes.cpp — lane W10-DIAG-UNKNOWN, rule SAMEOPEN-REV.
 *
 * A strategy.entry with the default size, placed while flat, can reach its
 * fill after an earlier entry of the same bar has opened a position at the
 * same open, and then reverses that position. TradingView judges it against
 * the equity it was sized from with the position it would close still
 * margined: its own side plus the held side must fit, or TradingView refuses
 * it and the earlier entry's position stays. The adapter admitted such a
 * reversal whenever its own side fitted, so at 100 % of equity the reversal
 * went through and a margin call trimmed it (htanrisevdir-trail-stop-al-sat-
 * stratejisi on NASDAQ:AAPL 1D, NSE:NIFTY 1D, NYSE:F 1D and OANDA:XAUUSD 1D).
 * The same holds for an entry placed on the held side of a position whose
 * reversing entry its bar placed first (job-2614-andrewwieiw-frosty-alerts on
 * BINANCE:BTCUSDT, OANDA:EURUSD and BINANCE:ETHUSDT.P 15).
 *
 * Each row replays TradingView's own tape of a synthetic probe
 * (tests/fixtures/same_open_reversal, lab tv exports on NASDAQ:AAPL 1D)
 * through the Pine adapter under the configuration the generated constructor
 * declares for it, over the lane's daily bars embedded in bars.inc, and
 * requires the trades the tape closes inside those bars to be the engine's:
 * entry and exit time, side, price and quantity. It also reads the rule off
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
#include <set>
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

#ifndef PINEFORGE_SAMEOPEN_FIXTURE_DIR
#error "PINEFORGE_SAMEOPEN_FIXTURE_DIR must name tests/fixtures/same_open_reversal"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/same_open_reversal/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The lane's NASDAQ:AAPL price tick and quantity step.
constexpr double kTick = 0.01;
constexpr double kLot = 1.0;
// The bar that places the first cell's orders (2025-04-14 13:30 UTC); the
// harness trades from here, as run_strategy.py does for these tapes.
constexpr std::int64_t kTradeStartMs = 1744637400000LL;

// One trade as both sides report it, prices in ticks:
// (entry ms, long, entry price, quantity, exit ms, exit price).
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }
long long lots(double qty) { return std::llround(qty / kLot); }
bool on_grid(double value, double step) {
    return std::abs(value / step - static_cast<double>(std::llround(value / step))) < 1e-6;
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kAapl1d) {
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

// <year>-<month>-<day> <hour>:<minute> UTC.
std::int64_t at(int year, unsigned month, unsigned day, int hour, int minute) {
    return ((days_from_civil(year, month, day) * 24 + hour) * 60 + minute) * 60'000;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

// Trades with their entry signal, as the tape or the engine reports them.
struct Book {
    std::vector<Row> trades;           // closed inside the replayed bars, in entry order
    std::vector<std::string> entries;  // each trade's entry signal
    std::vector<std::string> exits;    // each trade's exit signal
};

Book tape_trades(const std::string& tape, std::int64_t end_ms) {
    std::ifstream in(std::string(PINEFORGE_SAMEOPEN_FIXTURE_DIR) + "/" + tape + "/tv_trades.csv");
    std::map<int, Row> by_number;
    std::map<int, std::string> entry_signal, exit_signal;
    std::map<int, bool> closed;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        // Trade number, Type, Date and time, Signal, Price USD, Size (qty), ...
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
            closed[number] = true;
        }
    }
    Book out;
    for (const auto& [number, row] : by_number) {
        if (closed[number] && std::get<4>(row) < end_ms) {
            out.trades.push_back(row);
            out.entries.push_back(entry_signal[number]);
            out.exits.push_back(exit_signal[number]);
        }
    }
    return out;
}

// The probes, as their generated TUs lower them (fixtures/.../strategy.pine).
// p1-*: cells every two daily bars from 2025-04-14, in a 12-bar cycle (a
// 16-bar one with the explicit-quantity cell of p1-c50). w10-sameopen-held-*:
// a held position, then the reversing entry and an entry on the held side on
// one bar, in a 10-bar cycle.
enum class Script { P1, Held, Shapes, Pair };

class ProbeHost final : public source::PineStrategyHost {
public:
    ProbeHost(const source::PineStrategyConfig& config, Script script, int cycle)
        : script_(script), cycle_(cycle) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_mintick(kTick);
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("0930-1600");
        set_syminfo_type("stock");
        set_syminfo_metadata("qty_step", kLot);
        set_syminfo_metadata("margin_long", 100.0);
        set_syminfo_metadata("margin_short", 100.0);
    }

    void on_source_bar(const Bar&) override {
        const bool armed = current_bar_.timestamp >= at(2025, 4, 14, 0, 0);
        if (armed) ++k_;
        if (!armed) return;
        const int c = k_ % cycle_;
        if (script_ == Script::Held) {
            held(c);
            return;
        }
        if (script_ == Script::Shapes) {
            shapes(c);
            return;
        }
        if (script_ == Script::Pair) {
            if (c == 0) {
                strategy_entry("Long", true, kNaN, kNaN, kNaN, "L");
                strategy_entry("Short", false, kNaN, kNaN, kNaN, "S");
            } else if (c == 2) {
                strategy_close("", "X", kNaN, kNaN, false);
            }
            return;
        }
        if (c == 0) {
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L1");
            strategy_close("LONG", "CL1", kNaN, kNaN, false, 42949672979ULL);
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S1");
        } else if (c == 2) {
            strategy_close("", "X1", kNaN, kNaN, false);
        } else if (c == 4) {
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L2");
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S2");
        } else if (c == 6) {
            strategy_close("", "X2", kNaN, kNaN, false);
        } else if (c == 8) {
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S3");
            strategy_close("SHORT", "CS3", kNaN, kNaN, false, 90194313235ULL);
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L3");
        } else if (c == 10) {
            strategy_close("", "X3", kNaN, kNaN, false);
        } else if (c == 12) {
            const double q = std::floor((current_equity() + open_profit(current_bar_.close))
                                        * 0.5 / current_bar_.close);
            strategy_entry("LONG", true, kNaN, kNaN, q, "L4", "", 0, -1);
            strategy_entry("SHORT", false, kNaN, kNaN, q, "S4", "", 0, -1);
        } else if (c == 14) {
            strategy_close("", "X4", kNaN, kNaN, false);
        }
    }

private:
    void held(int c) {
        // H1: hold a short (S0); then L1 and S1 on one bar.
        if (c == 0) {
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S0");
        } else if (c == 2) {
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L1");
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S1");
        } else if (c == 4) {
            strategy_close("", "X1", kNaN, kNaN, false);
        // H2: hold a long (L0); then S2 and L2 on one bar.
        } else if (c == 5) {
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L0");
        } else if (c == 7) {
            strategy_entry("SHORT", false, kNaN, kNaN, kNaN, "S2");
            strategy_entry("LONG", true, kNaN, kNaN, kNaN, "L2");
        } else if (c == 9) {
            strategy_close("", "X2", kNaN, kNaN, false);
        }
    }

    // w10-sameopen-shapes-*: from flat, a Long and a Short plus a third call.
    void shapes(int c) {
        if (c == 0) {
            strategy_entry("Long-1", true, kNaN, kNaN, kNaN, "T-L1");
            strategy_entry("Short-2", false, kNaN, kNaN, kNaN, "T-S2");
            strategy_entry("Long-3", true, kNaN, kNaN, kNaN, "T-L3");
        } else if (c == 3) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN, "R-L");
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "R-S");
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "R-S2");
        } else if (c == 6) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN, "P-L");
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "P-S");
            strategy_entry("Priced", true, kNaN, current_bar_.close * 2, 1.0, "P-P", "", 0, -1);
        } else if (c == 9) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN, "W-L");
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "W-S");
            strategy_order("Raw", true, 1.0, kNaN, kNaN, "", 0);
        } else if (c == 12) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN, "C-L");
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "C-S");
            strategy_entry("Third", true, kNaN, kNaN, kNaN, "C-T");
            strategy_cancel("Third");
        } else if (c == 2 || c == 5 || c == 8 || c == 11 || c == 14) {
            strategy_cancel_all();
            strategy_close("", "X", kNaN, kNaN, false);
        }
    }

    Script script_;
    int cycle_;
    int k_ = -1;
};

// The engine's trades closed inside the bars (a position still open at the
// last bar is closed there by the range end and is not a tape trade).
Book run(const source::PineStrategyConfig& config, Script script, int cycle,
         std::int64_t end_ms, std::string& error) {
    ProbeHost host(config, script, cycle);
    host.set_trade_start_time(kTradeStartMs);
    const std::vector<Bar> bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "1D", false);
    error = host.last_error();
    Book out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        if (t.exit_time >= end_ms) continue;
        CHECK(on_grid(t.entry_price, kTick));
        CHECK(on_grid(t.exit_price, kTick));
        CHECK(on_grid(t.qty, kLot));
        out.trades.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price), lots(t.qty),
                                t.exit_time, ticks(t.exit_price));
        out.entries.push_back(t.entry_comment);
        out.exits.push_back(t.exit_comment);
    }
    return out;
}

// The rows of `book` whose entry signal is in `cells` and that close before
// `before_ms`.
std::vector<Row> rows_of(const Book& book, const std::set<std::string>& cells,
                         std::int64_t before_ms) {
    std::vector<Row> out;
    for (std::size_t i = 0; i < book.trades.size(); ++i) {
        if (cells.count(book.entries[i]) && std::get<4>(book.trades[i]) < before_ms)
            out.push_back(book.trades[i]);
    }
    return out;
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-8s %lld %s @%lld ticks q=%lld -> %lld @%lld ticks\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

// What each probe's generated constructor declares.
source::PineStrategyConfig config(double percent, double commission) {
    source::PineStrategyConfig c{};
    c.initial_capital = 10000000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = percent;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission;
    return c;
}

struct Case {
    const char* tape;
    double percent;
    double commission;
    Script script;
    int cycle;
    std::size_t closed;
    // The cells compared, and the bar their comparison stops at.
    std::set<std::string> cells;
    std::int64_t before_ms;
    const char* scope;
};

}  // namespace

int main() {
    const std::int64_t end_ms = kAapl1d[sizeof(kAapl1d) / sizeof(kAapl1d[0]) - 1].ts;
    const std::set<std::string> all = {"L1", "S1", "L2", "S2", "L3", "S3", "L4", "S4"};
    const std::set<std::string> held = {"S0", "L1", "S1", "L0", "S2", "L2"};
    const std::set<std::string> shapes = {"T-L1", "T-S2", "T-L3", "R-L", "R-S", "R-S2",
                                          "P-L", "P-S", "P-P", "W-L", "W-S", "C-L",
                                          "C-S", "C-T"};
    const Case cases[] = {
        {"p1-a100", 100.0, 0.01, Script::P1, 12, 38, all, end_ms, "every trade"},
        // At 10 % TradingView books each S3-CS3-L3 cell as a long L3 closed by
        // S3 at the open plus the long L3, where the engine books the short S3
        // closed by L3 plus the long L3: the same fills, prices, sizes and
        // money under swapped labels, a report the base engine shares. Only
        // the L-first cells are compared here.
        {"p1-b10", 10.0, 0.01, Script::P1, 12, 131, {"L1", "S1", "L2", "S2"}, end_ms,
         "every trade of the L1-CL1-S1 and L2-S2 cells"},
        // 2025-05-02 is the first explicit-quantity L4-S4 cell: TradingView
        // reverses L4 into S4 there, which the base engine does not either
        // (it closes L4 only); the money diverges from there on.
        {"p1-c50", 50.0, 0.01, Script::P1, 16, 91, all, at(2025, 5, 2, 0, 0),
         "every trade closed before the first L4-S4 cell (2025-05-02)"},
        {"w10-sameopen-held-a100", 100.0, 0.01, Script::Held, 10, 90, held, end_ms,
         "every trade"},
        // At 10 % TradingView reverses L0 into S2 and S2 back into L2 at one
        // open; the engine keeps S2 (its L2 does not fill), a separate
        // residual the rule cannot cause (it only refuses). The money diverges
        // from the first such cell (2025-04-25), so only the trades before it
        // are compared: the H1 cell of 04-17, where S1 must reverse L1 back.
        {"w10-sameopen-held-b10", 10.0, 0.01, Script::Held, 10, 156, {"S0", "L1", "S1", "L0"},
         at(2025, 4, 25, 14, 0), "every trade closed before the first S2-L2 cell (2025-04-25)"},
        // TradingView fills the raw strategy.order of the W cells at the same
        // open; the adapter fills it an open later, as it did before the
        // rule, one share: its rows are left out.
        {"w10-sameopen-shapes-base", 100.0, 0.0, Script::Shapes, 15, 50, shapes,
         end_ms, "every trade but the raw W-R order's"},
        {"w10-sameopen-pair-comm", 100.0, 0.1, Script::Pair, 3, 43, {"L", "S"}, end_ms,
         "every trade"},
    };

    std::map<std::string, Book> tapes;
    for (const Case& c : cases) {
        std::printf("-- %s (declares v6 percent_of_equity %.0f; compares %s)\n", c.tape,
                    c.percent, c.scope);
        const Book tape = tape_trades(c.tape, end_ms);
        tapes[c.tape] = tape;
        CHECK(tape.trades.size() == c.closed);
        std::string error;
        const Book lane = run(config(c.percent, c.commission), c.script, c.cycle, end_ms, error);
        CHECK(error.empty());
        const std::vector<Row> want = rows_of(tape, c.cells, c.before_ms);
        const std::vector<Row> got = rows_of(lane, c.cells, c.before_ms);
        CHECK(!want.empty());
        CHECK(got == want);
        if (got != want) {
            show("tape", want);
            show("engine", got);
        }
    }

    // What the tapes prove, read off TradingView's own rows.
    std::printf("-- the rule, on the tapes\n");
    {
        auto count = [](const Book& book, const char* signal) {
            int n = 0;
            for (const auto& id : book.entries) n += id == signal;
            return n;
        };
        auto closed_by = [](const Book& book, const char* entry, const char* exit) {
            int n = 0;
            for (std::size_t i = 0; i < book.entries.size(); ++i)
                n += book.entries[i] == entry && book.exits[i] == exit;
            return n;
        };
        // 100 %: the long fills, the short that would reverse it needs 200 %
        // and never fills; the long is closed by the cleanup.
        const Book& a100 = tapes["p1-a100"];
        CHECK(count(a100, "S1") == 0);
        CHECK(count(a100, "L1") > 0 && closed_by(a100, "L1", "X1") == count(a100, "L1"));
        // S3-CS3-L3 at 100 %: the short fills (a margin call trims it), the
        // long that would reverse it never does.
        CHECK(count(a100, "L3") == 0 && count(a100, "S3") > 0);
        // 10 %: 20 % fits, so the short reverses the long at the same open.
        const Book& b10 = tapes["p1-b10"];
        CHECK(count(b10, "S1") > 0 && closed_by(b10, "L1", "S1") == count(b10, "L1"));
        CHECK(count(b10, "S2") > 0 && closed_by(b10, "L2", "S2") == count(b10, "L2"));
        // 50 %: the first cycle's L1-CL1-S1 reverses (24686 x 201.86 twice is
        // 9,966,231.92 against 10,000,000), its L2-S2 does not (26175 x 196.12
        // twice is 10,266,882.00 against 10,113,055.02): the long runs to X2.
        const Book& c50 = tapes["p1-c50"];
        CHECK(c50.entries.size() > 3);
        if (c50.entries.size() > 3) {
            CHECK(c50.entries[0] == "L1" && c50.exits[0] == "S1");
            CHECK(c50.entries[1] == "S1");
            CHECK(c50.entries[2] == "L2" && c50.exits[2] == "X2");
            CHECK(std::get<3>(c50.trades[2]) == 26175);
        }
        // A held position, then the reversing entry and an entry on the held
        // side on one bar. At 100 % the second never reverses the position
        // the first opened at that open (no S1 or L2 fills; every L1 runs to
        // X1); at 10 % it does (every L1 is closed by S1, and TradingView books
        // each S2 it reverses as a long L2 closed by S2).
        const Book& ha = tapes["w10-sameopen-held-a100"];
        CHECK(count(ha, "S1") == 0 && count(ha, "L2") == 0);
        CHECK(count(ha, "L1") > 0 && closed_by(ha, "L1", "X1") == count(ha, "L1"));
        const Book& hb = tapes["w10-sameopen-held-b10"];
        CHECK(count(hb, "S1") > 0 && closed_by(hb, "L1", "S1") == count(hb, "L1"));
        CHECK(closed_by(hb, "L2", "S2") > 0);
        // A third call leaves the rule on (no Short fills in any cell), as
        // do a 0.1 % commission and the bar magnifier (the same trades as
        // without it). With slippage TradingView runs the Short close-only
        // instead: the Long closes on it and no short opens, except when the
        // Short was replaced, which is refused as without slippage.
        const Book& sb = tapes["w10-sameopen-shapes-base"];
        for (const char* id : {"T-S2", "T-L3", "R-S", "R-S2", "P-S", "W-S", "C-S"})
            CHECK(count(sb, id) == 0);
        CHECK(closed_by(sb, "W-L", "X") == count(sb, "W-L") && count(sb, "W-L") > 0);
        CHECK(count(tapes["w10-sameopen-pair-comm"], "S") == 0);
        const Book mag = tape_trades("w10-sameopen-shapes-mag", end_ms);
        CHECK(mag.trades == sb.trades && mag.entries == sb.entries && mag.exits == sb.exits);
        const Book slip = tape_trades("w10-sameopen-shapes-slip", end_ms);
        for (const char* id : {"R-S", "R-S2", "P-S", "W-S", "C-S", "T-S2"})
            CHECK(count(slip, id) == 0);
        CHECK(closed_by(slip, "W-L", "W-S") == count(slip, "W-L") && count(slip, "W-L") > 0);
        CHECK(closed_by(slip, "C-L", "C-S") == count(slip, "C-L") && count(slip, "C-L") > 0);
        CHECK(closed_by(slip, "P-L", "P-S") == count(slip, "P-L") && count(slip, "P-L") > 0);
        CHECK(closed_by(slip, "R-L", "X") == count(slip, "R-L") && count(slip, "R-L") > 0);
    }

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
