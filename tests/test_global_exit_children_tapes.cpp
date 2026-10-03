/*
 * test_global_exit_children_tapes.cpp -- the N5 pin (global exit children).
 *
 * TradingView keeps one child of a strategy.exit that names no entry for
 * every open entry. When one path point triggers several, it fills them in
 * the order of the entry ids in a table that iterates as a
 * java.util.HashMap<String, ?> (String.hashCode spread as h ^ (h >>> 16)
 * over 16 buckets, 32 once the keys pass 13, the newest key first in a
 * bucket; keys are entry ids from their first fill on, plus one per waiting
 * strategy.close / close_all). Each child is a transaction of its entry's
 * quantity: settled against its own entry under close_entries_rule = "ANY",
 * against the oldest lots first otherwise, one row per settled slice with
 * its own share of the fees. A named stop whose limit entry fills at a point
 * the stop is already through fills at once at the entry's fill with the
 * stop's slippage, not at the level the bar crossed before the entry.
 *
 * The tapes (tests/fixtures/global_exit_children, README.md) are all 127 of
 * the pin's own synthetic scripts, each exported twice byte-identical with
 * `lab tv --no-note`; schedules.inc is the schedule each strategy.pine
 * renders, which the host below replays through the Pine adapter over the
 * tape's bars (shared per feed window under bars/). On each of the 93
 * asserted tapes every row must be the engine's: ids, times, side, prices in
 * ticks, quantity, net profit at the report's precision and commission at
 * TradingView's ten significant digits. The 34 others are known divergences
 * (known_divergences.inc: the rule each needs and its first departure); they
 * run and report, and assert nothing.
 *
 * The pinned rows hold scripts the pin does not reach -- a partial global
 * exit, a named and a global child at one point, a table past 32 buckets, a
 * tie in a grown table, a waiting strategy.order, calc_on_order_fills -- at
 * the rows the engine booked before this change, bit for bit.
 *
 * Fail-before: on 700c5d24, 53 of the 93 asserted tapes fail (the change's
 * rules 2, 3 and 4); the pinned rows are 700c5d24's own.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
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

#ifndef PINEFORGE_GLOBAL_EXIT_CHILDREN_FIXTURE_DIR
#error "PINEFORGE_GLOBAL_EXIT_CHILDREN_FIXTURE_DIR must name tests/fixtures/global_exit_children"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kNoTime = std::numeric_limits<std::int64_t>::min();
constexpr int kPercent = static_cast<int>(CommissionType::PERCENT);
constexpr int kCashPerContract = static_cast<int>(CommissionType::CASH_PER_CONTRACT);

enum class Kind { Entry, Exit, Close, CloseAll, CancelAll, Order };

// One strategy.* call as generated code makes it; `token` is a close's call
// site ((line << 32) | column of the Pine call).
struct Op {
    Kind kind;
    const char* id;
    const char* from;
    bool is_long;
    double qty;
    double percent;
    double limit;
    double stop;
    std::uint64_t token;
};

// A block runs on a bar when each of its set conditions holds: the bar's
// open time `at`, the position side `cond` (1 long, -1 short, 2 either) and
// the window [since, until).
struct Block {
    std::int64_t at;
    int cond;
    std::int64_t since;
    std::int64_t until;
    std::vector<Op> ops;
};

struct Tape {
    const char* name;
    const char* bars;
    bool asserted;
    double tick;
    double step;
    int pyramiding;
    double capital;
    double margin;
    bool any;
    bool pooc;
    int slippage;
    int commission_type;
    double commission_value;
    std::vector<Block> blocks;
};

struct KnownDivergence {
    const char* name;
    const char* rule;
    int tv_rows;
    int engine_rows;
    int row;
    const char* field;
    const char* tradingview;
    const char* engine;
};

#include "fixtures/global_exit_children/schedules.inc"
#include "fixtures/global_exit_children/known_divergences.inc"

class ScheduleHost final : public source::PineStrategyHost {
public:
    explicit ScheduleHost(const Tape& tape, bool calc_on_order_fills = false) : tape_(tape) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig c;
        c.initial_capital = tape.capital;
        c.pyramiding = tape.pyramiding;
        c.margin_long = tape.margin;
        c.margin_short = tape.margin;
        c.close_entries_rule_any = tape.any;
        c.process_orders_on_close = tape.pooc;
        c.calc_on_order_fills = calc_on_order_fills;
        c.slippage = tape.slippage;
        c.commission_type = tape.commission_type;
        c.commission_value = tape.commission_value;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", tape.step);
        set_syminfo_timezone("Etc/UTC");
        syminfo_mintick_ = tape.tick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double held = signed_position_size();
        for (const Block& block : tape_.blocks) {
            if (block.at != kNoTime && t != block.at) continue;
            if (block.cond == 1 && !(held > 0.0)) continue;
            if (block.cond == -1 && !(held < 0.0)) continue;
            if (block.cond == 2 && held == 0.0) continue;
            if (block.since != kNoTime && t < block.since) continue;
            if (block.until != kNoTime && t >= block.until) continue;
            for (const Op& op : block.ops) {
                switch (op.kind) {
                case Kind::Entry:
                    strategy_entry(op.id, op.is_long, op.limit, kNaN, op.qty);
                    break;
                case Kind::Exit:
                    strategy_exit(op.id, op.from, op.limit, op.stop, kNaN, kNaN, kNaN,
                                  std::isnan(op.percent) ? 100.0 : op.percent, {}, op.qty);
                    break;
                case Kind::Close:
                    strategy_close(op.id, "", kNaN, kNaN, false, op.token);
                    break;
                case Kind::CloseAll:
                    // strategy.close_all() as generated code calls it.
                    strategy_close("", "", kNaN, kNaN, false);
                    break;
                case Kind::CancelAll:
                    strategy_cancel_all();
                    break;
                case Kind::Order:
                    strategy_order(op.id, op.is_long, op.qty, op.limit, kNaN, "", 0);
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

private:
    const Tape& tape_;
};

std::string fixture(const std::string& name, const char* file) {
    return std::string(PINEFORGE_GLOBAL_EXIT_CHILDREN_FIXTURE_DIR) + "/" + name + "/" + file;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) out.push_back(field);
    return out;
}

std::vector<Bar> bars(const std::string& window) {
    std::ifstream in(std::string(PINEFORGE_GLOBAL_EXIT_CHILDREN_FIXTURE_DIR) + "/bars/" + window
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

// The engine's close labels as the tape writes them.
std::string tape_label(const std::string& exit_id) {
    if (exit_id == "__pine_close_all") return "Close position order";
    if (exit_id.rfind("__close__", 0) == 0) return "Close entry(s) order " + exit_id.substr(9);
    return exit_id;
}

std::vector<Trade> run(const Tape& tape, bool coof = false) {
    const std::vector<Bar> feed = bars(tape.bars);
    ScheduleHost host(tape, coof);
    host.set_trade_start_time(feed.front().timestamp);
    host.run(feed.data(), static_cast<int>(feed.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    return host.report_rows();
}

bool same_ticks(double a, double b, double tick) {
    return std::llround(a / tick) == std::llround(b / tick);
}

bool same_row(const Row& w, const Trade& g, double tick) {
    return g.entry_id == w.entry_id && tape_label(g.exit_id) == w.exit_id
        && g.entry_time == w.entry_time && g.exit_time == w.exit_time
        && g.is_long == w.is_long && same_ticks(g.entry_price, w.entry_price, tick)
        && same_ticks(g.exit_price, w.exit_price, tick)
        && std::abs(g.qty - w.qty) < 1e-8
        && std::abs(g.pnl - w.pnl) <= 1e-6 * std::max(1.0, std::abs(w.pnl))
        && std::abs(g.commission - w.commission)
            <= 1e-9 * std::max(1e-9, std::abs(w.commission));
}

const KnownDivergence* known_divergence(const char* name) {
    for (const KnownDivergence& known : kKnownDivergences)
        if (std::string(known.name) == name) return &known;
    return nullptr;
}

int known_reported = 0;
int known_now_matching = 0;

void replay(const Tape& tape) {
    const std::vector<Row> want = tape_rows(tape.name);
    const std::vector<Trade> got = run(tape);
    if (!tape.asserted) {
        // Recorded, not asserted: report where the engine still departs.
        const KnownDivergence* known = known_divergence(tape.name);
        CHECK(known != nullptr);
        std::size_t first = 0;
        while (first < want.size() && first < got.size()
               && same_row(want[first], got[first], tape.tick)) {
            ++first;
        }
        const bool matches = first == want.size() && got.size() == want.size();
        ++known_reported;
        if (matches) ++known_now_matching;
        std::printf("-- %s (known divergence: %s; recorded %d/%d rows, row %d %s TV %s engine %s; "
                    "now %zu/%zu rows, %s)\n", tape.name, known ? known->rule : "?",
                    known ? known->tv_rows : 0, known ? known->engine_rows : 0,
                    known ? known->row : 0, known ? known->field : "?",
                    known ? known->tradingview : "?", known ? known->engine : "?", want.size(),
                    got.size(), matches ? "now matches" : "still departs");
        if (!matches && first < want.size())
            std::printf("    first departing row %zu\n", first + 1);
        return;
    }
    std::printf("-- %s\n", tape.name);
    CHECK(!want.empty());
    CHECK(got.size() == want.size());
    for (std::size_t i = 0; i < want.size() && i < got.size(); ++i) {
        const Row& w = want[i];
        const Trade& g = got[i];
        const bool ok = same_row(w, g, tape.tick);
        CHECK(ok);
        if (!ok) {
            std::printf("    row %zu tape   %s->%s %" PRId64 "->%" PRId64 " %.10g->%.10g q=%.10g "
                        "pnl=%.10g c=%.10g\n", i + 1, w.entry_id.c_str(), w.exit_id.c_str(),
                        w.entry_time, w.exit_time, w.entry_price, w.exit_price, w.qty, w.pnl,
                        w.commission);
            std::printf("    row %zu engine %s->%s %" PRId64 "->%" PRId64 " %.10g->%.10g q=%.10g "
                        "pnl=%.10g c=%.11g\n", i + 1, g.entry_id.c_str(),
                        tape_label(g.exit_id).c_str(), g.entry_time, g.exit_time, g.entry_price,
                        g.exit_price, g.qty, g.pnl, g.commission);
        }
    }
}

// ── Scripts the pin does not reach: the engine keeps the rows it booked
//    before this change (pinned_rows.inc), bit for bit. ────────────────────

const Tape* tape_named(const char* name) {
    for (const Tape& tape : kTapes)
        if (std::string(tape.name) == name) return &tape;
    return nullptr;
}

constexpr std::int64_t kJul30_0600 = 1753855200000LL;
constexpr std::int64_t kJul31_1030 = 1753957800000LL;
constexpr std::int64_t kAug01_1000 = 1754042400000LL;
constexpr std::int64_t kAug01_1230 = 1754051400000LL;
constexpr std::int64_t kAug01_1600 = 1754064000000LL;

constexpr const char* kEurusdBars = "eurusd-2025-07-30-2025-08-04";

Tape eurusd(const char* name, bool any, std::vector<Block> blocks) {
    return Tape{name, kEurusdBars, true, 1e-05, 0.01, 40, 500000.0, 0.0, any, false, 0, kPercent,
                0.0, std::move(blocks)};
}

Op exit_op(const char* id, const char* from, double qty, double limit, double stop) {
    return {Kind::Exit, id, from, true, qty, kNaN, limit, stop, 0};
}

Op entry_op(const char* id, bool is_long, double qty) {
    return {Kind::Entry, id, "", is_long, qty, kNaN, kNaN, kNaN, 0};
}

Block wick_while_long() {
    return {kNoTime, 1, kNoTime, kNoTime, {exit_op("Wick", "", kNaN, 1.3, 0.9)}};
}

Block wick_final() {
    return {kAug01_1230, 0, kNoTime, kNoTime, {exit_op("Wick", "", kNaN, 1.15439, 0.9)}};
}

Block flat_close() {
    return {kAug01_1600, 0, kNoTime, kNoTime,
            {{Kind::CloseAll, "", "", true, kNaN, kNaN, kNaN, kNaN, 0}}};
}

// One entry of quantity 1 per id, one bar apart from 2025-07-31 10:30 UTC.
std::vector<Block> ladder(const std::vector<const char*>& ids) {
    std::vector<Block> blocks;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        blocks.push_back({kJul31_1030 + static_cast<std::int64_t>(i) * 900000LL, 0, kNoTime,
                          kNoTime, {entry_op(ids[i], true, 1.0)}});
    }
    return blocks;
}

std::vector<std::pair<std::string, std::vector<Trade>>> unreached_runs() {
    std::vector<std::pair<std::string, std::vector<Trade>>> out;
    // A global exit of an explicit quantity: its fill leaves the position open.
    {
        Tape tape = eurusd("partial-global", false, {
            {kJul30_0600, 0, kNoTime, kNoTime, {entry_op("A", true, 3.0)}},
            {kNoTime, 1, kNoTime, kNoTime, {exit_op("Wick", "", 2.0, 1.3, 0.9)}},
            {kJul31_1030, 0, kNoTime, kNoTime, {entry_op("ZZ", true, 7.0)}},
            {kAug01_1230, 0, kNoTime, kNoTime, {exit_op("Wick", "", 2.0, 1.15439, 0.9)}},
            flat_close()});
        out.emplace_back(tape.name, run(tape));
    }
    // A named child and a global child at one point.
    {
        Tape tape = eurusd("named-and-global", true, {
            {kJul30_0600, 0, kNoTime, kNoTime, {entry_op("A", true, 3.0)}},
            wick_while_long(),
            {kJul31_1030, 0, kNoTime, kNoTime, {entry_op("ZZ", true, 7.0)}},
            {kAug01_1230, 0, kNoTime, kNoTime, {exit_op("Wick", "", kNaN, 1.15439, 0.9),
                                                exit_op("XA", "A", kNaN, 1.15439, 0.9)}},
            flat_close()});
        out.emplace_back(tape.name, run(tape));
    }
    // 27 entry ids: a table past 32 buckets.
    {
        std::vector<const char*> ids = {"K00", "K01", "K02", "K03", "K04", "K05", "K06", "K07",
                                        "K08", "K09", "K10", "K11", "K12", "K13", "K14", "K15",
                                        "K16", "K17", "K18", "K19", "K20", "K21", "K22", "K23",
                                        "K24", "K25", "K26"};
        std::vector<Block> blocks = {wick_while_long()};
        for (auto& block : ladder(ids)) blocks.push_back(std::move(block));
        blocks.push_back(wick_final());
        blocks.push_back(flat_close());
        Tape tape = eurusd("past-32", true, std::move(blocks));
        out.emplace_back(tape.name, run(tape));
    }
    // 14 entry ids, "A" and "a" in one bucket of the grown table.
    {
        std::vector<const char*> ids = {"a", "G", "U", "E", "T", "D", "S", "C", "R", "B", "Q",
                                        "P", "F", "A"};
        std::vector<Block> blocks = {wick_while_long()};
        for (auto& block : ladder(ids)) blocks.push_back(std::move(block));
        blocks.push_back(wick_final());
        blocks.push_back(flat_close());
        Tape tape = eurusd("grown-tie", true, std::move(blocks));
        out.emplace_back(tape.name, run(tape));
    }
    // 13 entry ids and a strategy.order market order waiting: is it a key?
    {
        std::vector<const char*> ids = {"G", "F", "U", "E", "T", "D", "S", "C", "R", "B", "Q",
                                        "A", "P"};
        std::vector<Block> blocks = {wick_while_long()};
        for (auto& block : ladder(ids)) blocks.push_back(std::move(block));
        blocks.push_back({kAug01_1000, 0, kNoTime, kNoTime,
                          {{Kind::Order, "O", "", false, 1.0, kNaN, kNaN, kNaN, 0}}});
        blocks.push_back(wick_final());
        blocks.push_back(flat_close());
        Tape tape = eurusd("order-waiting", true, std::move(blocks));
        out.emplace_back(tape.name, run(tape));
    }
    // The newborn stop under calc_on_order_fills.
    if (const Tape* eth = tape_named("eth-fresh")) {
        out.emplace_back("eth-fresh-coof", run(*eth, true));
    }
    return out;
}

struct PinnedRow {
    const char* run;
    const char* entry_id;
    const char* exit_id;
    std::int64_t entry_time, exit_time;
    double entry_price, exit_price, qty, pnl, commission;
};

#if !defined(PINEFORGE_GLOBAL_EXIT_CHILDREN_HARVEST)
#include "fixtures/global_exit_children/pinned_rows.inc"

void unreached_keeps_its_rows() {
    for (const auto& [name, got] : unreached_runs()) {
        std::printf("-- %s (pinned)\n", name.c_str());
        std::vector<const PinnedRow*> want;
        for (const PinnedRow& row : kPinnedRows)
            if (name == row.run) want.push_back(&row);
        CHECK(!want.empty());
        CHECK(got.size() == want.size());
        for (std::size_t i = 0; i < want.size() && i < got.size(); ++i) {
            const PinnedRow& w = *want[i];
            const Trade& g = got[i];
            const bool ok = g.entry_id == w.entry_id && g.exit_id == w.exit_id
                && g.entry_time == w.entry_time && g.exit_time == w.exit_time
                && g.entry_price == w.entry_price && g.exit_price == w.exit_price
                && g.qty == w.qty && g.pnl == w.pnl && g.commission == w.commission;
            CHECK(ok);
            if (!ok) {
                std::printf("    row %zu pinned %s->%s %a %a q=%a pnl=%a\n", i + 1, w.entry_id,
                            w.exit_id, w.entry_price, w.exit_price, w.qty, w.pnl);
                std::printf("    row %zu engine %s->%s %a %a q=%a pnl=%a\n", i + 1,
                            g.entry_id.c_str(), g.exit_id.c_str(), g.entry_price, g.exit_price,
                            g.qty, g.pnl);
            }
        }
    }
}
#else
void harvest() {
    std::printf("// The rows the engine booked on 700c5d24 for the scripts the N5 pin does not\n"
                "// reach (test_global_exit_children_tapes.cpp, unreached_runs). Harvested with\n"
                "// -DPINEFORGE_GLOBAL_EXIT_CHILDREN_HARVEST. Generated -- never edit by hand.\n");
    std::printf("const PinnedRow kPinnedRows[] = {\n");
    for (const auto& [name, got] : unreached_runs()) {
        for (const Trade& g : got) {
            std::printf("    {\"%s\", \"%s\", \"%s\", %" PRId64 "LL, %" PRId64 "LL, %a, %a, %a, %a, %a},\n",
                        name.c_str(), g.entry_id.c_str(), g.exit_id.c_str(), g.entry_time,
                        g.exit_time, g.entry_price, g.exit_price, g.qty, g.pnl, g.commission);
        }
    }
    std::printf("};\n");
}
#endif

}  // namespace

int main() {
#if defined(PINEFORGE_GLOBAL_EXIT_CHILDREN_HARVEST)
    harvest();
    return 0;
#else
    for (const Tape& tape : kTapes) replay(tape);
    unreached_keeps_its_rows();
    std::printf("\n%d known divergences reported (%d now match TradingView)\n", known_reported,
                known_now_matching);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
#endif
}
