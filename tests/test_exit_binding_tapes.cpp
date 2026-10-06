/*
 * test_exit_binding_tapes.cpp -- TradingView binds a strategy.exit when it is
 * called.
 *
 * A strategy.exit(id, from_entry = X) looks at X when the script calls it:
 * - X holds open lots: the exit is bound to those lots and ends with them
 *   (a close, close_all, its own fill or a reversal), flat or not; a later
 *   fill of X never wakes it;
 * - X has no lot but a working entry order: the exit is bound to the id X,
 *   waits for X's next fill and survives everything before it -- close_all,
 *   a close of a sibling, the flat, a reversal, a strategy.cancel(X) and a new
 *   strategy.entry(X); a stop already through at X's fill fills at once, at
 *   the fill with the stop's slippage;
 * - neither: the call is ignored. It creates nothing, moves no level of a
 *   standing exit and detaches nothing.
 * A global exit binds the same way to the open position's lots, else to the
 * working entry orders. Beside the binding, an entry order of the position's
 * side resting from an earlier bar survives a close that flattens the book,
 * a stop as a limit does, and an add called while the position already holds
 * `pyramiding` entries is never placed: it does not fill after the position
 * shrinks or goes flat. Every tape runs with margin requirements off, and
 * the engine applies the rule only there (README.md, "Scope").
 *
 * The tapes (tests/fixtures/exit_binding, README.md) are the pin's 36
 * synthetic controls, each exported twice byte-identical with
 * `lab tv --no-note`; schedules.inc is the schedule each strategy.pine
 * renders, which the host below replays through the Pine adapter over the
 * tape's bars. Every row must be the engine's: ids, times, side, prices in
 * ticks, quantity, net profit at the report's precision and commission at
 * TradingView's ten significant digits.
 *
 * Each rule part has its own switch (ExitBindingRuleSwitches). With one
 * switch off the tapes that need its part must depart from TradingView
 * (parts()), so every part is live and pinned by its tapes; with every switch
 * off the engine books what it booked before these rules (kBeforeTheRules).
 * Every tape also runs as a forward stream (stream_begin() over the first
 * bar, stream_push_bar() for every later bar) and must close the same trades
 * as the backtest, bit for bit, with every switch on and with every switch
 * off.
 *
 * One mechanism case pins the engine's own rows (not TradingView's) on the
 * tapes' bars, for a branch no tape reaches: the cap part judges an add once,
 * at the opening after its call. An add kept there beside a close of its bar
 * still rests when a later entry refills the cap, and fills.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#ifndef PINEFORGE_EXIT_BINDING_FIXTURE_DIR
#error "PINEFORGE_EXIT_BINDING_FIXTURE_DIR must name tests/fixtures/exit_binding"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr double kStep = 0.0001;
constexpr const char* kBars = "ethusdtp-2025-10-30-2025-11-05";

enum class Kind { Entry, Exit, Close, CloseAll, Cancel };

// One strategy.* call as generated code makes it; `token` is a close's call
// site ((line << 32) | column of the Pine call).
struct Op {
    Kind kind;
    const char* id;
    const char* from;
    bool is_long;
    double qty;
    double limit;
    double stop;
    const char* comment;
    std::uint64_t token;
};

// The calls a script makes on the bar that opens at `at`, in script order.
struct Block {
    std::int64_t at;
    std::vector<Op> ops;
};

struct Tape {
    const char* name;
    double capital;
    int pyramiding;
    double margin;
    bool pooc;
    int slippage;
    double commission_percent;
    std::vector<Block> blocks;
};

#include "fixtures/exit_binding/schedules.inc"

class ScheduleHost : public source::PineStrategyHost {
public:
    explicit ScheduleHost(const Tape& tape) : tape_(tape) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.initial_capital = tape.capital;
        c.pyramiding = tape.pyramiding;
        c.margin_long = tape.margin;
        c.margin_short = tape.margin;
        c.close_entries_rule_any = false;
        c.process_orders_on_close = tape.pooc;
        c.slippage = tape.slippage;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = tape.commission_percent;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", kStep);
        set_syminfo_timezone("Etc/UTC");
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        for (const Block& block : tape_.blocks) {
            if (t != block.at) continue;
            for (const Op& op : block.ops) {
                switch (op.kind) {
                case Kind::Entry:
                    strategy_entry(op.id, op.is_long, op.limit, op.stop, op.qty);
                    break;
                case Kind::Exit:
                    strategy_exit(op.id, op.from, op.limit, op.stop, kNaN, kNaN, kNaN, 100.0, {},
                                  kNaN);
                    break;
                case Kind::Close:
                    strategy_close(op.id, "", kNaN, kNaN, false, op.token);
                    break;
                case Kind::CloseAll:
                    // strategy.close_all(comment = ...) as generated code calls it.
                    strategy_close("", op.comment, kNaN, kNaN, false);
                    break;
                case Kind::Cancel:
                    strategy_cancel(op.id);
                    break;
                }
            }
        }
    }

    // The report's rows: the closed trades, then a position still open at
    // the range's end.
    std::vector<Trade> report_rows() const {
        std::vector<Trade> out(trades_.begin(), trades_.end());
        out.insert(out.end(), range_end_trades_.begin(), range_end_trades_.end());
        return out;
    }

    std::vector<Trade> closed() const {
        std::vector<Trade> out;
        for (std::size_t i = 0; i < closed_trade_count(); ++i) out.push_back(closed_trade(i));
        return out;
    }

    double position() const { return signed_position_size(); }

private:
    const Tape& tape_;
};

std::string fixture(const std::string& name, const char* file) {
    return std::string(PINEFORGE_EXIT_BINDING_FIXTURE_DIR) + "/" + name + "/" + file;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) out.push_back(field);
    return out;
}

const std::vector<Bar>& bars() {
    static const std::vector<Bar> feed = [] {
        std::ifstream in(std::string(PINEFORGE_EXIT_BINDING_FIXTURE_DIR) + "/bars/" + kBars
                         + ".csv");
        std::string line;
        std::getline(in, line);
        std::vector<Bar> out;
        while (std::getline(in, line)) {
            const auto row = fields(line);
            if (row.size() != 6) continue;
            Bar b{};
            b.timestamp = std::stoll(row[0]);
            b.open = std::stod(row[1]);
            b.high = std::stod(row[2]);
            b.low = std::stod(row[3]);
            b.close = std::stod(row[4]);
            b.volume = std::stod(row[5]);
            out.push_back(b);
        }
        return out;
    }();
    return feed;
}

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
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Row {
    std::string entry_id, exit_id;
    std::int64_t entry_time = 0, exit_time = 0;
    bool is_long = true;
    double entry_price = kNaN, exit_price = kNaN, qty = kNaN, pnl = kNaN, commission = kNaN;
};

std::vector<Row> tape_rows(const std::string& name) {
    std::ifstream in(fixture(name, "tv_trades.csv"));
    std::string line;
    std::getline(in, line);
    std::map<int, Row> by_number;
    while (std::getline(in, line)) {
        const auto cell = fields(line);
        if (cell.size() < 10) continue;
        Row& row = by_number[std::stoi(cell[0])];
        if (cell[1].rfind("Entry", 0) == 0) {
            row.entry_time = tape_ms(cell[2]);
            row.entry_id = cell[3];
            row.entry_price = std::stod(cell[4]);
            row.is_long = cell[1] == "Entry long";
        } else {
            row.exit_time = tape_ms(cell[2]);
            row.exit_id = cell[3];
            row.exit_price = std::stod(cell[4]);
        }
        row.qty = std::stod(cell[5]);
        row.pnl = std::stod(cell[7]);
        row.commission = std::stod(cell[9]);
    }
    std::vector<Row> out;
    for (auto& item : by_number) out.push_back(item.second);
    return out;
}

// The engine's close labels as the tape writes them: a close's comment, else
// TradingView's name for the order.
std::string tape_label(const Trade& t) {
    if (!t.exit_comment.empty()) return t.exit_comment;
    if (t.exit_id == "__pine_close_all") return "Close position order";
    if (t.exit_id.rfind("__close__", 0) == 0) return "Close entry(s) order " + t.exit_id.substr(9);
    return t.exit_id;
}

std::vector<Trade> run(const Tape& tape) {
    const std::vector<Bar>& feed = bars();
    ScheduleHost host(tape);
    host.set_trade_start_time(feed.front().timestamp);
    host.run(feed.data(), static_cast<int>(feed.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    return host.report_rows();
}

bool same_ticks(double a, double b) {
    return std::llround(a / kTick) == std::llround(b / kTick);
}

bool same_row(const Row& w, const Trade& g) {
    return g.entry_id == w.entry_id && tape_label(g) == w.exit_id
        && g.entry_time == w.entry_time && g.exit_time == w.exit_time
        && g.is_long == w.is_long && same_ticks(g.entry_price, w.entry_price)
        && same_ticks(g.exit_price, w.exit_price)
        && std::abs(g.qty - w.qty) < 1e-8
        && std::abs(g.pnl - w.pnl) <= 1e-6 * std::max(1.0, std::abs(w.pnl))
        && std::abs(g.commission - w.commission)
            <= 1e-9 * std::max(1e-9, std::abs(w.commission));
}

// Every row of the engine's report against the rows wanted (`label` says
// whose rows they are): the first departing row (1-based), or 0 when the
// engine books exactly those rows.
std::size_t first_departure(const std::vector<Row>& want, const char* label,
                            const std::vector<Trade>& got, bool print) {
    CHECK(!want.empty());
    std::size_t departs = 0;
    for (std::size_t i = 0; i < want.size() || i < got.size(); ++i) {
        if (i < want.size() && i < got.size() && same_row(want[i], got[i])) continue;
        departs = i + 1;
        if (print) {
            if (i < want.size()) {
                const Row& w = want[i];
                std::printf("    row %zu %-6s %s->%s %" PRId64 "->%" PRId64 " %.10g->%.10g "
                            "q=%.10g pnl=%.10g c=%.10g\n", i + 1, label, w.entry_id.c_str(),
                            w.exit_id.c_str(), w.entry_time, w.exit_time, w.entry_price,
                            w.exit_price, w.qty, w.pnl, w.commission);
            }
            if (i < got.size()) {
                const Trade& g = got[i];
                std::printf("    row %zu engine %s->%s %" PRId64 "->%" PRId64 " %.10g->%.10g "
                            "q=%.10g pnl=%.10g c=%.11g\n", i + 1, g.entry_id.c_str(),
                            tape_label(g).c_str(), g.entry_time, g.exit_time, g.entry_price,
                            g.exit_price, g.qty, g.pnl, g.commission);
            }
        }
        break;
    }
    return departs;
}

// Every row of the engine's report against the tape: the first departing
// row (1-based), or 0 when the engine reproduces the tape.
std::size_t first_departure(const Tape& tape, bool print) {
    const std::vector<Row> want = tape_rows(tape.name);
    return first_departure(want, "tape", run(tape), print);
}

using source::detail::ExitBindingRuleSwitches;
using source::detail::exit_binding_rule_switches;

// Every part's switch set to `on`. A new ExitBindingRuleSwitches field must be
// set here too: the static_assert stops the build until it is.
ExitBindingRuleSwitches every_part(bool on) {
    static_assert(sizeof(ExitBindingRuleSwitches) == 5,
                  "ExitBindingRuleSwitches gained a field: set it in every_part()");
    ExitBindingRuleSwitches switches;
    switches.pending_bound_exit_survives_flat = on;
    switches.global_exit_binds_working_entries = on;
    switches.resting_stop_entry_survives_close = on;
    switches.priced_add_at_cap_not_placed = on;
    switches.global_exit_binds_held_position = on;
    return switches;
}

// The tapes that depart from TradingView with one part's switch off and every
// other on. A part whose tapes are elsewhere names them in the comment.
struct Part {
    const char* name;
    bool ExitBindingRuleSwitches::*on;
    std::vector<const char*> tapes;
};

const std::vector<Part>& parts() {
    static const std::vector<Part> list = {
        {"pending_bound_exit_survives_flat",
         &ExitBindingRuleSwitches::pending_bound_exit_survives_flat,
         {"base", "reissued", "close-sibling", "nonpooc", "close-pending-id",
          "samebar-before-close", "member-replica", "nonpooc-reissued", "rebind",
          "short-member-shape", "stop-entry-parent", "entry-recalled", "cancel-replace",
          "cancel-flat-replace", "recall-after-cancel", "ignored-call-keeps-levels"}},
        // Pinned by tests/fixtures/global_exit_children pending-89
        // (test_global_exit_children_tapes).
        {"global_exit_binds_working_entries",
         &ExitBindingRuleSwitches::global_exit_binds_working_entries, {}},
        {"resting_stop_entry_survives_close",
         &ExitBindingRuleSwitches::resting_stop_entry_survives_close, {"stop-entry-parent"}},
        {"priced_add_at_cap_not_placed", &ExitBindingRuleSwitches::priced_add_at_cap_not_placed,
         {"pyr1", "pyr1-close-sibling"}},
        // Pinned by tests/fixtures/cross_side_exit (test_cross_side_exit_tapes)
        // and tests/fixtures/same_side_exit (test_same_side_exit_tapes); no
        // tape here has a global exit called in position beside an entry order.
        {"global_exit_binds_held_position",
         &ExitBindingRuleSwitches::global_exit_binds_held_position, {}},
    };
    return list;
}

// With every switch off the engine books what it booked before these rules:
// the tapes that departed from TradingView then.
const std::vector<const char*> kBeforeTheRules = {
    "base", "reissued", "close-sibling", "pyr1", "nonpooc", "close-pending-id",
    "samebar-before-close", "member-replica", "pyr1-close-sibling", "nonpooc-reissued", "rebind",
    "short-member-shape", "stop-entry-parent", "entry-recalled", "cancel-replace",
    "cancel-flat-replace", "recall-after-cancel", "ignored-call-keeps-levels"};

// The departing tapes under the switches set now.
std::set<std::string> departing() {
    std::set<std::string> out;
    for (const Tape& tape : kTapes)
        if (first_departure(tape, false) != 0) out.insert(tape.name);
    return out;
}

void expect_departing(const char* label, const std::vector<const char*>& want) {
    const std::set<std::string> got = departing();
    const std::set<std::string> expected(want.begin(), want.end());
    std::printf("-- %s: %zu tapes depart\n", label, got.size());
    CHECK(got == expected);
    for (const auto& name : got)
        if (!expected.count(name)) std::printf("    departs, not listed: %s\n", name.c_str());
    for (const auto& name : expected)
        if (!got.count(name)) std::printf("    listed, reproduces: %s\n", name.c_str());
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

bool same_trade(const Trade& a, const Trade& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
        && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl) && same_bits(a.pnl_pct, b.pnl_pct)
        && a.is_long == b.is_long && a.entry_bar_index == b.entry_bar_index
        && a.exit_bar_index == b.exit_bar_index && a.entry_id == b.entry_id
        && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
        && a.exit_id == b.exit_id && a.exit_from_bracket == b.exit_from_bracket
        && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown)
        && same_bits(a.commission, b.commission) && a.entry_incarnation == b.entry_incarnation
        && a.open_at_end == b.open_at_end && a.close_cause == b.close_cause;
}

struct Replay {
    std::string error;
    std::vector<Trade> closed;
    double position = 0.0;
};

Replay backtest(const Tape& tape) {
    const std::vector<Bar>& feed = bars();
    ScheduleHost host(tape);
    host.set_trade_start_time(feed.front().timestamp);
    host.run(feed.data(), static_cast<int>(feed.size()), "15", "15", false);
    return {host.last_error(), host.closed(), host.position()};
}

// stream_begin() over the first bar -- the least warmup a stream accepts --
// then stream_push_bar() for every later bar; read after the last push.
Replay forward(const Tape& tape) {
    const std::vector<Bar>& feed = bars();
    ScheduleHost host(tape);
    host.set_trade_start_time(feed.front().timestamp);
    Replay out;
    if (!host.stream_begin(feed.data(), 1, "15", "15") || !host.stream_is_realtime()) {
        out.error = "stream_begin: " + host.last_error();
        return out;
    }
    for (std::size_t i = 1; i < feed.size(); ++i) {
        if (!host.stream_push_bar(feed[i])) {
            out.error = "stream_push_bar(" + std::to_string(i) + "): " + host.last_error();
            return out;
        }
    }
    out.closed = host.closed();
    out.position = host.position();
    if (!host.stream_end()) out.error = "stream_end: " + host.last_error();
    return out;
}

// One engine, two modes: the backtest and the forward stream of `tape` close
// the same trades, bit for bit, and leave the same position.
bool forward_matches(const Tape& tape) {
    const Replay a = backtest(tape);
    const Replay b = forward(tape);
    bool same = a.error.empty() && b.error.empty() && a.closed.size() == b.closed.size()
        && same_bits(a.position, b.position);
    for (std::size_t i = 0; same && i < a.closed.size(); ++i)
        same = same_trade(a.closed[i], b.closed[i]);
    if (!same) {
        std::printf("    %s: backtest %zu trades%s, forward %zu trades%s\n", tape.name,
                    a.closed.size(), a.error.empty() ? "" : (" " + a.error).c_str(),
                    b.closed.size(), b.error.empty() ? "" : (" " + b.error).c_str());
    }
    return same;
}

void backtest_equals_forward(const char* label) {
    int identical = 0;
    for (const Tape& tape : kTapes) {
        const bool same = forward_matches(tape);
        CHECK(same);
        if (same) ++identical;
    }
    std::printf("-- forward == backtest, %s: %d of %zu tapes\n", label, identical,
                sizeof(kTapes) / sizeof(kTapes[0]));
}

// Mechanism case: engine rows, not TradingView's, on the tapes' bars, for a
// branch no tape reaches. At pyramiding 1, with L1 held, the add L2 is called
// at the cap and then L1 is closed, on the 17:15 bar. At the next opening the
// position is flat, so the cap part keeps L2. A market L3 refills the cap at
// 18:00; the cap part judges an add once, at the opening after its call, so
// L2 still rests and fills at its limit on 2025-11-03 02:30 UTC. With the cap
// part off the engine books the same rows.
const Tape kCapAddJudgedOnce = {
    "cap-add-judged-once", 100000.0, 1, 0.0, true, 0, 0.0, {
        {1761871500000LL, {{Kind::Entry, "L1", "", true, 1.5217, kNaN, kNaN, "", 0ULL}}},
        {1761930900000LL, {{Kind::Entry, "L2", "", true, 1.5213, 3804.3448485, kNaN, "", 0ULL},
                           {Kind::Close, "L1", "", true, kNaN, kNaN, kNaN, "", 38654705683ULL}}},
        {1761933600000LL, {{Kind::Entry, "L3", "", true, 1.0, kNaN, kNaN, "", 0ULL}}},
        {1761940800000LL, {{Kind::CloseAll, "", "", true, kNaN, kNaN, kNaN, "", 0ULL}}},
        {1762200000000LL, {{Kind::CloseAll, "", "", true, kNaN, kNaN, kNaN, "", 0ULL}}}}};

const std::vector<Row> kCapAddJudgedOnceRows = {
    {"L1", "Close entry(s) order L1", 1761871500000LL, 1761930900000LL, true, 3829.9, 3820.74,
     1.5217, -13.938772, 0.0},
    {"L3", "Close position order", 1761933600000LL, 1761940800000LL, true, 3827.08, 3886.86, 1.0,
     59.78, 0.0},
    {"L2", "Close position order", 1762137000000LL, 1762200000000LL, true, 3804.34, 3607.58,
     1.5213, -299.330988, 0.0},
};

// The mechanism case under the switches set now: the engine books its rows,
// and the forward stream closes the backtest's trades.
void expect_cap_add_judged_once(const char* label) {
    const std::vector<Trade> got = run(kCapAddJudgedOnce);
    std::printf("-- %s, %s: %zu rows (engine rows, not a tape)\n", kCapAddJudgedOnce.name, label,
                got.size());
    CHECK(first_departure(kCapAddJudgedOnceRows, "pinned", got, true) == 0);
    CHECK(forward_matches(kCapAddJudgedOnce));
}

}  // namespace

int main() {
    ExitBindingRuleSwitches& switches = exit_binding_rule_switches();
    switches = every_part(true);
    for (const Tape& tape : kTapes) {
        std::printf("-- %s\n", tape.name);
        CHECK(first_departure(tape, true) == 0);
    }
    backtest_equals_forward("every part on");

    // Each part is live and pinned: with its switch off its tapes depart.
    for (const Part& part : parts()) {
        switches = every_part(true);
        switches.*part.on = false;
        expect_departing((std::string(part.name) + " off").c_str(), part.tapes);
    }
    switches = every_part(false);
    expect_departing("every part off", kBeforeTheRules);
    backtest_equals_forward("every part off");

    // The cap part judges an add once: with it on, and with it off.
    switches = every_part(true);
    expect_cap_add_judged_once("every part on");
    switches.priced_add_at_cap_not_placed = false;
    expect_cap_add_judged_once("cap part off");
    switches = ExitBindingRuleSwitches{};

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
