/*
 * test_admission_rules_tapes.cpp -- the admission's fill half, the close-first
 * shape, the point order and the book rules on TradingView's own tapes.
 *
 * tests/fixtures/admission_rules holds `lab tv --no-note` tapes of synthetic
 * probes, each a script whose strategy.* calls fire at fixed bar times:
 *
 * - gap_admission/ (19 sources, 3 byte-identical exports each): a
 *   short seed, then on one bar a close-first entry (strategy.close of the
 *   seed or strategy.close_all(), then the opposite entry), an entry then a
 *   close of the seed, or a flat entry, at a session-open gap of one ulp, 3
 *   or 7 ticks, default 100 % of equity or an explicit quantity.
 *   TradingView fills every close-first entry flat at the gapped open and
 *   checks it only on the money scale (rule 2, cf7 / cf7b / cf7c), and drops
 *   every other entry whose sig10(sig10(E) / Q) is under the fill.
 * - point_order_and_book/ (11 sources, 3 exports each): the point order -- a
 *   protective stop the open gaps through fills before the margin check (c1,
 *   c2, c4), a close the script placed before its reversal is unconditional
 *   (c3b; c3a, the entry first, is the control) -- and the exit tombstones
 *   under process_orders_on_close (c5, c5b); the pyramiding cap on open
 *   close-ledger records (pyr1, pyr2) and the whole-book add (add1, add2).
 * - stop_priority/ (39 stop-priority tapes and 3 coupled close + reversal
 *   tapes, one export each).
 * - explicit_short/ (one source, 3 exports): an explicit short from flat at
 *   the offset-0 equity, filled by TradingView (prediction.json).
 *
 * plan.tsv is each script's calls in statement order, keyed by the bar time
 * that fires them ("held_long" = while strategy.position_size > 0);
 * cases.tsv the strategy() settings; bars/ the chart windows, 00:00 UTC to
 * 00:00 UTC inclusive (provenance.json: export shas, chart ranges).
 *
 * Every trade row TradingView reports -- its closed trades and a position
 * still open at the range end -- must be the engine's report row at the same
 * index: side, quantity in lots, entry and exit price in ticks, entry and
 * exit time. kKnownDivergences names the tapes the engine does not reproduce
 * yet; each must still differ. Then every rule this fixture pins is turned
 * off alone (MarginRuleSwitches) and must cost at least one tape.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

#ifndef PINEFORGE_ADMISSION_RULES_FIXTURE_DIR
#error "PINEFORGE_ADMISSION_RULES_FIXTURE_DIR must name tests/fixtures/admission_rules"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;
constexpr std::size_t kSources = 73;
constexpr std::size_t kTvRows = 152;
constexpr std::size_t kForwardSampled = 56;

using pineforge::source::detail::MarginRuleSwitches;

// Tapes the engine does not reproduce yet: fixture path and a one-line reason.
const std::vector<std::pair<std::string, std::string>> kKnownDivergences = {
};

// Tapes kept out of the backtest == forward pass, with the reason.
const std::vector<std::pair<std::string, std::string>> kForwardExcluded = {
    {"gap_admission/cf10-cf-exp-long-ulp-d0829", "the 2025-09-01 holiday is an in-session gap to a stream"},
    {"gap_admission/cf11-rev-exp-long-ulp-d0829", "the 2025-09-01 holiday is an in-session gap to a stream"},
    {"gap_admission/cf12-flat-exp-long-ulp-d0829", "the 2025-09-01 holiday is an in-session gap to a stream"},
};

std::string fixture(const std::string& relative) {
    return std::string(PINEFORGE_ADMISSION_RULES_FIXTURE_DIR) + "/" + relative;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream fields(text);
    while (std::getline(fields, cell, separator)) cells.push_back(cell);
    if (!text.empty() && text.back() == separator) cells.emplace_back();
    return cells;
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
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

double number_or_nan(const std::string& text) {
    return text == "na" || text.empty() ? kNaN : std::strtod(text.c_str(), nullptr);
}

// One strategy.* call of a plan.tsv row.
struct Call {
    std::string when;  // bar open ms, or "held_long"
    std::int64_t when_ms = -1;
    std::string op;
    std::vector<std::string> args;
};

struct Case {
    std::string path;
    std::string bars;
    double tick = 0.0;
    double step = 0.0;
    source::PineStrategyConfig config;
    std::int64_t from_ms = 0;
    std::int64_t to_ms = 0;
    std::vector<Call> plan;
};

std::vector<Call> load_plan(const std::string& path) {
    std::ifstream in(fixture(path + "/plan.tsv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    CHECK(line == "when\tcall\targs");
    std::vector<Call> plan;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cell = split(line, '\t');
        CHECK(cell.size() >= 2);
        if (cell.size() < 2) continue;
        Call call;
        call.when = cell[0];
        if (call.when != "held_long")
            call.when_ms = static_cast<std::int64_t>(std::strtoll(call.when.c_str(), nullptr, 10));
        call.op = cell[1];
        call.args.assign(cell.begin() + 2, cell.end());
        const std::size_t want = call.op == "entry" ? 5 : call.op == "close" ? 1
                               : call.op == "close_all" ? 0 : call.op == "exit" ? 5 : 99;
        CHECK(call.args.size() == want);
        plan.push_back(std::move(call));
    }
    return plan;
}

std::vector<Case> load_cases() {
    std::ifstream in(fixture("cases.tsv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    std::map<std::string, std::size_t> column;
    const std::vector<std::string> header = split(line, '\t');
    for (std::size_t i = 0; i < header.size(); ++i) column[header[i]] = i;
    for (const char* name : {"path", "bars", "tick", "step", "capital", "qty_type", "qty_value",
                             "margin_long", "margin_short", "fee_type", "fee", "slippage", "pooc",
                             "coof", "pyramiding", "from_ms", "to_ms"}) {
        CHECK(column.count(name) == 1);
        if (!column.count(name)) return {};
    }
    std::vector<Case> cases;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cell = split(line, '\t');
        CHECK(cell.size() == header.size());
        if (cell.size() != header.size()) continue;
        auto at = [&](const char* name) -> const std::string& { return cell[column[name]]; };
        auto number = [&](const char* name) { return std::strtod(at(name).c_str(), nullptr); };
        Case c;
        c.path = at("path");
        c.bars = at("bars");
        c.tick = number("tick");
        c.step = number("step");
        c.config.initial_capital = number("capital");
        CHECK(at("qty_type") == "percent" || at("qty_type") == "fixed");
        c.config.default_qty_type = static_cast<int>(
            at("qty_type") == "percent" ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        c.config.default_qty_value = number("qty_value");
        c.config.margin_long = number("margin_long");
        c.config.margin_short = number("margin_short");
        CHECK(at("fee_type") == "percent");
        c.config.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.config.commission_value = number("fee");
        c.config.slippage = std::atoi(at("slippage").c_str());
        c.config.process_orders_on_close = at("pooc") == "1";
        c.config.calc_on_order_fills = at("coof") == "1";
        c.config.pyramiding = std::atoi(at("pyramiding").c_str());
        c.from_ms = static_cast<std::int64_t>(std::strtoll(at("from_ms").c_str(), nullptr, 10));
        c.to_ms = static_cast<std::int64_t>(std::strtoll(at("to_ms").c_str(), nullptr, 10));
        c.plan = load_plan(c.path);
        cases.push_back(std::move(c));
    }
    return cases;
}

std::vector<Bar> load_bars(const std::string& name) {
    std::ifstream in(fixture("bars/" + name));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    CHECK(line == "timestamp,open,high,low,close,volume");
    std::vector<Bar> bars;
    while (std::getline(in, line)) {
        const std::vector<std::string> cell = split(line, ',');
        CHECK(cell.size() == 6);
        if (cell.size() != 6) continue;
        bars.push_back({std::strtod(cell[1].c_str(), nullptr), std::strtod(cell[2].c_str(), nullptr),
                        std::strtod(cell[3].c_str(), nullptr), std::strtod(cell[4].c_str(), nullptr),
                        std::strtod(cell[5].c_str(), nullptr),
                        static_cast<std::int64_t>(std::strtoll(cell[0].c_str(), nullptr, 10))});
    }
    return bars;
}

// One trade row as both sides report it: prices in ticks, quantity in lots.
struct Row {
    bool is_long = true;
    long long qty_lots = 0;
    long long entry_ticks = 0;
    long long exit_ticks = 0;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
};

Row make_row(bool is_long, double qty, double entry_price, double exit_price,
             std::int64_t entry_ms, std::int64_t exit_ms, const Case& c) {
    return {is_long, std::llround(qty / c.step), std::llround(entry_price / c.tick),
            std::llround(exit_price / c.tick), entry_ms, exit_ms};
}

// TradingView's rows: each Exit row paired with the Entry row of its trade
// number, in trade-number order (tv_trades.csv starts with a UTF-8 BOM).
std::vector<Row> tape_rows(const Case& c) {
    std::ifstream in(fixture(c.path + "/tv_trades.csv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    CHECK(line.rfind("Trade number,Type,Date and time,Signal,Price ", 0) == 0);
    struct Side { bool seen = false; bool is_long = true; double price = 0.0; double qty = 0.0; std::int64_t ms = 0; };
    std::map<int, std::pair<Side, Side>> by_number;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> cell = split(line, ',');
        // The Signal column may hold a comment with commas: the fields after
        // it are fixed, so read them from the right.
        CHECK(cell.size() >= 17);
        if (cell.size() < 17) continue;
        const std::size_t price_at = cell.size() - 13;
        const int number = std::atoi(cell[0].c_str());
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        CHECK(entry || cell[1].rfind("Exit", 0) == 0);
        Side& side = entry ? by_number[number].first : by_number[number].second;
        CHECK(!side.seen);
        side.seen = true;
        side.is_long = cell[1].size() >= 4 && cell[1].compare(cell[1].size() - 4, 4, "long") == 0;
        side.ms = tape_ms(cell[2]);
        side.price = std::strtod(cell[price_at].c_str(), nullptr);
        side.qty = std::strtod(cell[price_at + 1].c_str(), nullptr);
    }
    std::vector<Row> rows;
    for (const auto& [number, pair] : by_number) {
        const Side& entry = pair.first;
        const Side& exit = pair.second;
        CHECK(entry.seen && exit.seen);
        if (!entry.seen || !exit.seen) continue;
        CHECK(entry.is_long == exit.is_long);
        rows.push_back(make_row(exit.is_long, exit.qty, entry.price, exit.price, entry.ms,
                                exit.ms, c));
    }
    return rows;
}

// The script: its plan's calls on the bar opening at their time, in
// statement order; "held_long" rows while the position is long.
class PlanHost final : public source::PineStrategyHost {
public:
    explicit PlanHost(const Case& c) : c_(c) {
        attach_pine_execution_adapter();
        configure_pine_strategy(c.config);
        set_syminfo_mintick(c.tick);
        set_syminfo_metadata("qty_step", c.step);
        // NYSE:F: the exchange's time zone and regular session, so a stream
        // takes the overnight gap as the session break it is.
        if (stock(c)) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
        }
    }

    static bool stock(const Case& c) { return c.bars.rfind("f-", 0) == 0; }

    void on_source_bar(const Bar& bar) override {
        const bool held_long = signed_position_size() > 0.0;
        for (const Call& call : c_.plan) {
            if (call.when == "held_long" ? !held_long : call.when_ms != bar.timestamp) continue;
            const auto& a = call.args;
            if (call.op == "entry") {
                strategy_entry(a[0], a[1] == "long", number_or_nan(a[3]), number_or_nan(a[4]),
                               number_or_nan(a[2]));
            } else if (call.op == "close") {
                strategy_close(a[0]);
            } else if (call.op == "close_all") {
                strategy_close_all();
            } else if (call.op == "exit") {
                strategy_exit(a[0], a[1], number_or_nan(a[2]), number_or_nan(a[3]), kNaN, kNaN,
                              kNaN, number_or_nan(a[4]));
            }
        }
    }

    double position() const { return signed_position_size(); }

private:
    Case c_;
};

struct Outcome {
    std::vector<Row> rows;
    std::string error;
};

Outcome replay(const Case& c, const std::vector<Bar>& bars) {
    PlanHost host(c);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    Outcome outcome;
    outcome.error = host.last_error();
    for (int index = 0; index < host.report_trade_count(); ++index) {
        const Trade& trade = host.get_report_trade(index);
        outcome.rows.push_back(make_row(trade.is_long, trade.qty, trade.entry_price,
                                        trade.exit_price, trade.entry_time, trade.exit_time, c));
    }
    return outcome;
}

std::string first_difference(const std::vector<Row>& tv, const Outcome& engine) {
    if (!engine.error.empty()) return "engine error: " + engine.error;
    const std::size_t common = std::min(tv.size(), engine.rows.size());
    std::string count;
    if (tv.size() != engine.rows.size())
        count = "rows tv=" + std::to_string(tv.size()) + " engine=" + std::to_string(engine.rows.size());
    for (std::size_t i = 0; i < common; ++i) {
        const Row& t = tv[i];
        const Row& e = engine.rows[i];
        auto field = [&](const char* name, long long expected, long long actual) {
            const std::string text = "row " + std::to_string(i + 1) + " " + name + " tv="
                + std::to_string(expected) + " engine=" + std::to_string(actual);
            return count.empty() ? text : text + " (" + count + ")";
        };
        if (t.is_long != e.is_long) return field("long", t.is_long, e.is_long);
        if (t.qty_lots != e.qty_lots) return field("qty_lots", t.qty_lots, e.qty_lots);
        if (t.entry_ticks != e.entry_ticks) return field("entry_ticks", t.entry_ticks, e.entry_ticks);
        if (t.exit_ticks != e.exit_ticks) return field("exit_ticks", t.exit_ticks, e.exit_ticks);
        if (t.entry_ms != e.entry_ms) return field("entry_ms", t.entry_ms, e.entry_ms);
        if (t.exit_ms != e.exit_ms) return field("exit_ms", t.exit_ms, e.exit_ms);
    }
    return count;
}

void dump(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-6s %s q=%lld lots %lld -> %lld ticks, %lld -> %lld\n", tag,
                    r.is_long ? "long" : "short", r.qty_lots, r.entry_ticks, r.exit_ticks,
                    static_cast<long long>(r.entry_ms), static_cast<long long>(r.exit_ms));
}

// One engine, two modes: the closed trades and the position a batch run
// books equal, bit for bit, those of a forward run that streams every bar
// after the first (stream_begin, stream_push_bar), and stream_end books no
// closed trade.
struct Closed {
    bool is_long = true;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double qty = 0.0;
};

std::vector<Closed> closed_of(PlanHost& host) {
    std::vector<Closed> rows;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        rows.push_back({t.is_long, t.entry_time, t.exit_time, t.entry_price, t.exit_price, t.qty});
    }
    return rows;
}

bool same_closed(const std::vector<Closed>& a, const std::vector<Closed>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].is_long != b[i].is_long || a[i].entry_ms != b[i].entry_ms
            || a[i].exit_ms != b[i].exit_ms
            || std::memcmp(&a[i].entry_price, &b[i].entry_price, sizeof(double)) != 0
            || std::memcmp(&a[i].exit_price, &b[i].exit_price, sizeof(double)) != 0
            || std::memcmp(&a[i].qty, &b[i].qty, sizeof(double)) != 0) {
            return false;
        }
    }
    return true;
}

std::string forward_difference(const Case& c, const std::vector<Bar>& bars) {
    PlanHost batch(c);
    batch.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    if (!batch.last_error().empty()) return "batch error: " + batch.last_error();
    PlanHost forward(c);
    if (!forward.stream_begin(bars.data(), 1, "15", "15"))
        return "stream_begin refused: " + forward.last_error();
    if (!forward.stream_is_realtime()) return "stream_begin did not reach realtime";
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (!forward.stream_push_bar(bars[i]))
            return "stream_push_bar(" + std::to_string(i) + ") refused: " + forward.last_error();
    }
    const std::vector<Closed> streamed = closed_of(forward);
    const double streamed_position = forward.position();
    if (!forward.stream_end()) return "stream_end refused: " + forward.last_error();
    if (!same_closed(streamed, closed_of(forward))) return "stream_end changed the closed trades";
    if (!same_closed(closed_of(batch), streamed)) return "closed trades differ";
    const double batch_position = batch.position();
    if (std::memcmp(&batch_position, &streamed_position, sizeof(double)) != 0)
        return "position differs";
    return {};
}

std::set<std::string> matching(const std::vector<Case>& cases,
                               std::map<std::string, std::vector<Bar>>& feeds) {
    std::set<std::string> matched;
    for (const Case& c : cases) {
        if (first_difference(tape_rows(c), replay(c, feeds.at(c.bars))).empty())
            matched.insert(c.path);
    }
    return matched;
}

}  // namespace

int main() {
    std::printf("=== admission_rules: TradingView tapes of the admission's fill half, the point order and the book rules ===\n");
    const std::vector<Case> cases = load_cases();
    CHECK(cases.size() == kSources);
    auto& switches = pineforge::source::detail::margin_rule_switches();
    const MarginRuleSwitches tree_defaults = switches;

    std::map<std::string, std::string> known;
    for (const auto& [path, reason] : kKnownDivergences) {
        CHECK(!reason.empty());
        CHECK(known.emplace(path, reason).second);
    }
    std::set<std::string> listed;
    std::map<std::string, std::vector<Bar>> feeds;
    std::size_t matched = 0;
    std::size_t tv_rows = 0;
    for (const Case& c : cases) {
        listed.insert(c.path);
        auto feed = feeds.find(c.bars);
        if (feed == feeds.end()) feed = feeds.emplace(c.bars, load_bars(c.bars)).first;
        const std::vector<Bar>& bars = feed->second;
        CHECK(!bars.empty() && bars.front().timestamp >= c.from_ms
              && bars.back().timestamp <= c.to_ms);
        const std::vector<Row> tv = tape_rows(c);
        tv_rows += tv.size();
        const Outcome engine = replay(c, bars);
        const std::string difference = first_difference(tv, engine);
        const bool match = difference.empty();
        if (match) {
            ++matched;
            std::printf("MATCH %s\n", c.path.c_str());
        } else {
            std::printf("DIFF %s %s\n", c.path.c_str(), difference.c_str());
            dump("tv", tv);
            dump("engine", engine.rows);
        }
        if (known.count(c.path) == 0) {
            CHECK(match);
        } else {
            CHECK(!match);
            if (match) std::printf("  now matches, remove from kKnownDivergences: %s\n", c.path.c_str());
        }
    }
    for (const auto& [path, reason] : known) CHECK(listed.count(path) == 1);
    CHECK(tv_rows == kTvRows);
    std::printf("admission_rules: %zu/%zu tapes match, %zu TV rows\n", matched, cases.size(), tv_rows);

    // Backtest == forward on every NYSE:F tape of the fixture (none of them
    // recalculates on order fills, which a stream refuses). The OANDA:XAUUSD
    // tapes stay out: their chart has no regular session here, so a stream
    // refuses the weekend gap as an in-session gap.
    std::size_t forward_identical = 0;
    std::size_t forward_sampled = 0;
    for (const Case& c : cases) {
        CHECK(!c.config.calc_on_order_fills);
        if (!PlanHost::stock(c)) continue;
        if (std::any_of(kForwardExcluded.begin(), kForwardExcluded.end(),
                        [&](const auto& row) { return row.first == c.path; })) {
            std::printf("FORWARD-EXCLUDED %s\n", c.path.c_str());
            continue;
        }
        ++forward_sampled;
        const std::string difference = forward_difference(c, feeds.at(c.bars));
        if (difference.empty()) {
            ++forward_identical;
        } else {
            std::printf("FORWARD-DIFF %s %s\n", c.path.c_str(), difference.c_str());
        }
    }
    CHECK(forward_identical == forward_sampled);
    CHECK(forward_sampled == kForwardSampled);
    std::printf("forward == batch: %zu/%zu NYSE:F tapes\n", forward_identical, forward_sampled);

#ifndef PINEFORGE_ADMISSION_RULES_NO_ABLATION
    // Every rule this fixture pins, off alone, costs at least one tape. The
    // fill re-check is pinned on margin_call_rules and margin_ledger_rules:
    // here the engine's earlier reversal decline already drops cf3, cf3b and
    // cf4 at their fill without it, so it only must not gain a tape.
    using S = MarginRuleSwitches;
    struct Pinned {
        const char* name;
        bool S::*flag;
        bool load_bearing;
    };
    const Pinned pinned[] = {
        {"unified_placement", &S::unified_placement, true},
        {"fill_price_recheck", &S::fill_price_recheck, false},
        {"close_first_admission", &S::close_first_admission, true},
        {"point_fills_before_margin", &S::point_fills_before_margin, true},
        {"pyramiding_ledger_records", &S::pyramiding_ledger_records, true},
        {"exit_child_tombstones", &S::exit_child_tombstones, true},
    };
    const std::set<std::string> on = matching(cases, feeds);
    for (const auto& [name, flag, load_bearing] : pinned) {
        switches = tree_defaults;
        switches.*flag = false;
        const std::set<std::string> off = matching(cases, feeds);
        std::size_t lost = 0;
        for (const std::string& path : on) {
            if (off.count(path) == 0) {
                ++lost;
                std::printf("  ablation %s off loses %s\n", name, path.c_str());
            }
        }
        for (const std::string& path : off)
            if (on.count(path) == 0) std::printf("  ablation %s off gains %s\n", name, path.c_str());
        std::printf("ablation %s off: %zu/%zu tapes match\n", name, off.size(), cases.size());
        if (load_bearing) CHECK(lost > 0);
        else CHECK(off.size() <= on.size());
    }
#endif
    switches = tree_defaults;
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
