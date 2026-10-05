/*
 * test_margin_schedule_rules_tapes.cpp -- the margin-call schedule rules on
 * TradingView's own tapes, in a backtest and in a forward (stream) run.
 *
 * tests/fixtures/margin_schedule_rules holds 39 `lab tv` tapes of synthetic
 * controls (README.md there names the groups), each exported twice, both
 * exports byte-identical: a short's call gate at its slipped print and the
 * lagged follow-up after a call, with and without process_orders_on_close
 * (gate/, lag/), a vetoed deficit pending at an
 * open ahead of a resting stop (veto/), a reversal placed at a close where a
 * margin call is booked after the script (frozen/), a long's one-unit call
 * after a trade history (money/), a same-side add judged at the signal close
 * (add/), a long's follow-up on whole shares (follow/) and four adds at an
 * open on a margin-1 % book, whose call at that open is taken after the add's
 * fill (add-fee/; MarginOpeningSwitches::open_check_after_add_fill). Each tape directory holds the script
 * TradingView ran (strategy.pine), its trade list (tv_trades.csv), its export
 * summary (metrics.json) and spec.txt, the replay this test drives:
 *
 *   config <capital> <mintick> <qty_step> <slippage> <commission %> <margin %>
 *          <process_orders_on_close 0/1> <pyramiding>
 *   bars <file under bars/>
 *   session <time zone> <session>   (NYSE:F: the exchange's zone and regular session)
 *   ev <utc ms> entry <id> <long|short> <qty> <any|flat|notflat>
 *   ev <utc ms> close_all - - - <cond>
 *   ev <utc ms> close <id> - - <cond>
 *   ev <utc ms> exit <id> <from_entry> <stop> <cond>
 *   ev <utc ms> stopentry <id> <long|short> <qty>@<stop> <cond>
 *
 * An event fires on the bar whose open time it names, in file order, when its
 * condition holds on the book the script sees (flat, not flat, or any) -- the
 * script's `if time == timestamp(...)` blocks, a money/ tape's earlier trades
 * replayed from its own tape. Every row TradingView reports must be the
 * engine's report row at the same index: side, quantity in lots, entry and
 * exit price in ticks, entry and exit time, and the row count.
 *
 * kKnownDivergences names the tapes the engine does not reproduce yet; each
 * must still differ. Every switch of MarginScheduleSwitches ships on, which
 * this test checks; turned off alone, a switch pinned here costs at least one
 * tape. And every tape is replayed forward too -- stream_begin() over its
 * first bar, stream_push_bar() for every later one -- whose closed trades
 * must be the backtest's, every field bit for bit.
 *
 * Three mechanism cases pin the engine's own rows (not TradingView's) on
 * synthetic or patched bars, for branches no tape reaches: the walk's close
 * call owing the next open (which also separates the gate, the path points
 * and the lagged follow-up), the pending veto on the open's segment, and the
 * pending veto standing down beside another resting order.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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
#include <tuple>
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

#ifndef PINEFORGE_MARGIN_SCHEDULE_RULES_FIXTURE_DIR
#error "PINEFORGE_MARGIN_SCHEDULE_RULES_FIXTURE_DIR must name tests/fixtures/margin_schedule_rules"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kTapes = 39;
constexpr std::size_t kTvRows = 670;

using pineforge::source::detail::MarginScheduleSwitches;

// Tapes the engine does not reproduce yet: path and a one-line reason. A
// listed tape that starts matching fails the test until it is removed.
const std::vector<std::pair<std::string, std::string>> kKnownDivergences = {
    {"frozen/close-id",
     "a strategy.close(id) placed before an after-script margin call keeps its size "
     "(not implemented: the close waits as a same-bar command there)"},
};

std::string fixture(const std::string& relative) {
    return std::string(PINEFORGE_MARGIN_SCHEDULE_RULES_FIXTURE_DIR) + "/" + relative;
}

struct Event {
    std::int64_t ms = 0;
    std::string kind, a, b, c, cond;
};

struct Spec {
    std::string path;
    double capital = 0.0, tick = 0.0, step = 0.0, fee = 0.0, margin = 100.0;
    int slippage = 0, pyramiding = 1;
    bool pooc = false;
    std::string bars;
    std::string timezone, session;
    std::vector<Event> events;
};

Spec parse_spec(const std::string& path, std::istream& in) {
    Spec spec;
    spec.path = path;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "config") {
            int pooc = 0;
            words >> spec.capital >> spec.tick >> spec.step >> spec.slippage >> spec.fee
                >> spec.margin >> pooc >> spec.pyramiding;
            spec.pooc = pooc != 0;
        } else if (word == "bars") {
            words >> spec.bars;
        } else if (word == "session") {
            words >> spec.timezone >> spec.session;
        } else if (word == "ev") {
            Event e;
            words >> e.ms >> e.kind >> e.a >> e.b >> e.c >> e.cond;
            CHECK(e.kind == "entry" || e.kind == "close_all" || e.kind == "close"
                  || e.kind == "exit" || e.kind == "stopentry");
            spec.events.push_back(e);
        } else if (!word.empty()) {
            CHECK(!"unknown spec line");
        }
    }
    CHECK(spec.tick > 0.0 && spec.step > 0.0 && !spec.bars.empty());
    return spec;
}

Spec load_spec(const std::string& path) {
    std::ifstream in(fixture(path + "/spec.txt"));
    CHECK(in.good());
    return parse_spec(path, in);
}

std::vector<Bar> load_bars(const std::string& name) {
    std::ifstream in(fixture("bars/" + name));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    CHECK(line == "timestamp,open,high,low,close,volume");
    std::vector<Bar> bars;
    while (std::getline(in, line)) {
        std::string cell[6];
        std::istringstream fields(line);
        for (auto& value : cell) std::getline(fields, value, ',');
        bars.push_back({std::strtod(cell[1].c_str(), nullptr), std::strtod(cell[2].c_str(), nullptr),
                        std::strtod(cell[3].c_str(), nullptr), std::strtod(cell[4].c_str(), nullptr),
                        std::strtod(cell[5].c_str(), nullptr),
                        static_cast<std::int64_t>(std::strtoll(cell[0].c_str(), nullptr, 10))});
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

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct Row {
    bool is_long = true;
    long long qty_lots = 0, entry_ticks = 0, exit_ticks = 0;
    std::int64_t entry_ms = 0, exit_ms = 0;
    bool operator==(const Row& o) const {
        return is_long == o.is_long && qty_lots == o.qty_lots && entry_ticks == o.entry_ticks
            && exit_ticks == o.exit_ticks && entry_ms == o.entry_ms && exit_ms == o.exit_ms;
    }
};

// TradingView's rows: each Exit row paired with the Entry row of its trade
// number, in trade-number order (tv_trades.csv starts with a UTF-8 BOM).
std::vector<Row> tape_rows(const Spec& spec) {
    std::ifstream in(fixture(spec.path + "/tv_trades.csv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    CHECK(line.rfind("Trade number,Type,Date and time,Signal,Price ", 0) == 0);
    struct Side { bool seen = false, is_long = true; double price = 0.0, qty = 0.0; std::int64_t ms = 0; };
    std::map<int, std::pair<Side, Side>> by_number;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> cell;
        std::istringstream fields(line);
        std::string value;
        while (std::getline(fields, value, ',')) cell.push_back(value);
        CHECK(cell.size() >= 6);
        if (cell.size() < 6) continue;
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        Side& side = entry ? by_number[std::atoi(cell[0].c_str())].first
                           : by_number[std::atoi(cell[0].c_str())].second;
        CHECK(!side.seen);
        side.seen = true;
        side.is_long = cell[1].size() >= 4 && cell[1].compare(cell[1].size() - 4, 4, "long") == 0;
        side.ms = tape_ms(cell[2]);
        side.price = std::strtod(cell[4].c_str(), nullptr);
        side.qty = std::strtod(cell[5].c_str(), nullptr);
    }
    std::vector<Row> rows;
    for (const auto& [number, pair] : by_number) {
        CHECK(pair.first.seen && pair.second.seen);
        rows.push_back({pair.second.is_long, std::llround(pair.second.qty / spec.step),
                        std::llround(pair.first.price / spec.tick),
                        std::llround(pair.second.price / spec.tick), pair.first.ms, pair.second.ms});
    }
    return rows;
}

// The control's script: its strategy.* calls on the bars they name.
class ScheduleHost final : public source::PineStrategyHost {
public:
    explicit ScheduleHost(const Spec& spec) : spec_(spec) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig configuration;
        configuration.initial_capital = spec.capital;
        configuration.pyramiding = spec.pyramiding;
        configuration.margin_long = spec.margin;
        configuration.margin_short = spec.margin;
        configuration.commission_type = static_cast<int>(CommissionType::PERCENT);
        configuration.commission_value = spec.fee;
        configuration.slippage = spec.slippage;
        configuration.process_orders_on_close = spec.pooc;
        configure_pine_strategy(configuration);
        set_syminfo_mintick(spec.tick);
        set_syminfo_metadata("qty_step", spec.step);
        if (!spec.timezone.empty()) {
            set_syminfo_timezone(spec.timezone);
            set_syminfo_session(spec.session);
        }
    }

    void on_source_bar(const Bar& bar) override {
        for (const Event& e : spec_.events) {
            if (e.ms != bar.timestamp) continue;
            const double position = signed_position_size();
            if (e.cond == "flat" && position != 0.0) continue;
            if (e.cond == "notflat" && position == 0.0) continue;
            if (e.kind == "entry")
                strategy_entry(e.a, e.b == "long", kNaN, kNaN, std::strtod(e.c.c_str(), nullptr));
            else if (e.kind == "close_all")
                strategy_close_all();
            else if (e.kind == "close")
                strategy_close(e.a);
            else if (e.kind == "exit")
                strategy_exit(e.a, e.b, kNaN, std::strtod(e.c.c_str(), nullptr));
            else if (e.kind == "stopentry") {
                // <qty>@<stop>: a stop entry resting at that level.
                const auto at = e.c.find('@');
                strategy_entry(e.a, e.b == "long", kNaN,
                               std::strtod(e.c.substr(at + 1).c_str(), nullptr),
                               std::strtod(e.c.substr(0, at).c_str(), nullptr));
            }
        }
    }

private:
    Spec spec_;
};

struct Outcome {
    std::vector<Row> rows;
    std::vector<Trade> closed;
    std::string error;
};

Row row_of(const Trade& t, const Spec& spec) {
    return {t.is_long, std::llround(t.qty / spec.step), std::llround(t.entry_price / spec.tick),
            std::llround(t.exit_price / spec.tick), t.entry_time, t.exit_time};
}

Outcome replay(const Spec& spec, const std::vector<Bar>& bars) {
    ScheduleHost host(spec);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    Outcome outcome;
    outcome.error = host.last_error();
    for (int i = 0; i < host.report_trade_count(); ++i)
        outcome.rows.push_back(row_of(host.get_report_trade(i), spec));
    for (int i = 0; i < host.trade_count(); ++i) outcome.closed.push_back(host.get_trade(i));
    return outcome;
}

// stream_begin() over the first bar, stream_push_bar() for every later one:
// the closed trades before stream_end().
Outcome forward(const Spec& spec, const std::vector<Bar>& bars) {
    ScheduleHost host(spec);
    Outcome outcome;
    if (!host.stream_begin(bars.data(), 1, "15", "15") || !host.stream_is_realtime()) {
        outcome.error = "stream_begin refused: " + host.last_error();
        return outcome;
    }
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (!host.stream_push_bar(bars[i])) {
            outcome.error = "stream_push_bar refused: " + host.last_error();
            return outcome;
        }
    }
    for (int i = 0; i < host.trade_count(); ++i) outcome.closed.push_back(host.get_trade(i));
    (void)host.stream_end();
    return outcome;
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::string trade_difference(const Trade& a, const Trade& b) {
    if (a.entry_time != b.entry_time || a.exit_time != b.exit_time) return "time";
    if (!same_bits(a.entry_price, b.entry_price) || !same_bits(a.exit_price, b.exit_price))
        return "price";
    if (!same_bits(a.qty, b.qty)) return "qty";
    if (!same_bits(a.pnl, b.pnl) || !same_bits(a.pnl_pct, b.pnl_pct)) return "pnl";
    if (!same_bits(a.commission, b.commission)) return "commission";
    if (a.is_long != b.is_long || a.entry_id != b.entry_id || a.exit_id != b.exit_id
        || a.entry_comment != b.entry_comment || a.exit_comment != b.exit_comment)
        return "ids";
    if (a.entry_bar_index != b.entry_bar_index || a.exit_bar_index != b.exit_bar_index)
        return "bar index";
    return {};
}

std::string first_difference(const std::vector<Row>& tv, const Outcome& engine) {
    if (!engine.error.empty()) return "engine error: " + engine.error;
    for (std::size_t i = 0; i < tv.size() && i < engine.rows.size(); ++i) {
        if (!(tv[i] == engine.rows[i])) {
            const Row& t = tv[i];
            const Row& e = engine.rows[i];
            char text[256];
            std::snprintf(text, sizeof text,
                          "row %zu tv %s %lld lots %lld->%lld ticks %lld->%lld ms, engine %s %lld "
                          "lots %lld->%lld ticks %lld->%lld ms", i + 1, t.is_long ? "long" : "short",
                          t.qty_lots, t.entry_ticks, t.exit_ticks, static_cast<long long>(t.entry_ms),
                          static_cast<long long>(t.exit_ms), e.is_long ? "long" : "short", e.qty_lots,
                          e.entry_ticks, e.exit_ticks, static_cast<long long>(e.entry_ms),
                          static_cast<long long>(e.exit_ms));
            return text;
        }
    }
    if (tv.size() != engine.rows.size())
        return "rows tv=" + std::to_string(tv.size()) + " engine=" + std::to_string(engine.rows.size());
    return {};
}

MarginScheduleSwitches all_on() {
    static_assert(sizeof(MarginScheduleSwitches) == 9 * sizeof(bool),
                  "MarginScheduleSwitches changed: name its new field here and in the ablation table");
    MarginScheduleSwitches on;
    on.short_call_gate = true;
    on.short_path_points = true;
    on.lagged_short_follow_up = true;
    on.pending_veto_first = true;
    on.frozen_reversal_close = true;
    on.add_signal_close = true;
    on.whole_share_lagged_follow_up = true;
    on.long_call_gain_loss = true;
    on.close_call_follow_up_at_open = true;
    return on;
}

struct Ablation {
    const char* name;
    bool MarginScheduleSwitches::*flag;
    bool load_bearing = true;
};

}  // namespace

int main() {
    std::printf("=== margin_schedule_rules: TradingView tapes of the margin-call schedule rules ===\n");
    std::vector<std::string> paths;
    {
        std::ifstream in(fixture("tapes.txt"));
        CHECK(in.good());
        std::string line;
        while (std::getline(in, line))
            if (!line.empty()) paths.push_back(line);
    }
    CHECK(paths.size() == kTapes);
    auto& switches = pineforge::source::detail::margin_schedule_switches();
    const MarginScheduleSwitches shipped = switches;
    const MarginScheduleSwitches on = all_on();
    CHECK(std::memcmp(&shipped, &on, sizeof(MarginScheduleSwitches)) == 0);

    std::map<std::string, std::string> known(kKnownDivergences.begin(), kKnownDivergences.end());
    CHECK(known.size() == kKnownDivergences.size());
    std::vector<Spec> specs;
    std::map<std::string, std::vector<Bar>> feeds;
    std::map<std::string, std::vector<Row>> tv;
    std::size_t tv_rows = 0;
    for (const std::string& path : paths) {
        specs.push_back(load_spec(path));
        const Spec& spec = specs.back();
        if (!feeds.count(spec.bars)) feeds.emplace(spec.bars, load_bars(spec.bars));
        tv[path] = tape_rows(spec);
        tv_rows += tv[path].size();
    }
    CHECK(tv_rows == kTvRows);
    for (const auto& [path, reason] : known) {
        CHECK(!reason.empty());
        CHECK(tv.count(path) == 1);
    }

    const auto matching = [&]() {
        std::set<std::string> matched;
        for (const Spec& spec : specs)
            if (first_difference(tv[spec.path], replay(spec, feeds.at(spec.bars))).empty())
                matched.insert(spec.path);
        return matched;
    };

    std::size_t matched = 0;
    std::size_t forward_checked = 0;
    for (const Spec& spec : specs) {
        const std::vector<Bar>& bars = feeds.at(spec.bars);
        const Outcome batch = replay(spec, bars);
        const std::string difference = first_difference(tv[spec.path], batch);
        const bool match = difference.empty();
        std::printf("%s %s %s\n", match ? "MATCH" : "DIFF", spec.path.c_str(), difference.c_str());
        if (known.count(spec.path)) {
            CHECK(!match);
            if (match) std::printf("  now matches, remove from kKnownDivergences: %s\n", spec.path.c_str());
        } else {
            CHECK(match);
            ++matched;
        }
        // Forward: the closed trades of a stream are the backtest's.
        const Outcome stream = forward(spec, bars);
        if (!stream.error.empty()) std::printf("  FORWARD %s %s\n", spec.path.c_str(), stream.error.c_str());
        CHECK(stream.error.empty());
        CHECK(stream.closed.size() == batch.closed.size());
        bool same = stream.error.empty() && stream.closed.size() == batch.closed.size();
        for (std::size_t i = 0; same && i < batch.closed.size(); ++i) {
            const std::string field = trade_difference(batch.closed[i], stream.closed[i]);
            if (!field.empty()) {
                std::printf("  FORWARD %s closed trade %zu differs in %s\n", spec.path.c_str(), i + 1,
                            field.c_str());
                same = false;
            }
        }
        CHECK(same);
        forward_checked += same;
    }
    std::printf("margin_schedule_rules: %zu/%zu tapes match (%zu known divergences), %zu TV rows; "
                "forward == backtest on %zu/%zu\n", matched, specs.size(), known.size(), tv_rows,
                forward_checked, specs.size());

    // Mechanism cases: engine rows, not TradingView's, on synthetic or patched
    // bars, for the branches no tape reaches. Each names the rows with every
    // switch on and with the switches it separates turned off.
    {
        using M = MarginScheduleSwitches;
        struct Expect {
            bool M::*off;  // nullptr: every switch on
            std::vector<std::tuple<std::int64_t, double, double>> rows;  // exit ms, price, qty
        };
        struct Mechanism {
            const char* name;
            std::string spec;
            std::vector<Bar> bars;  // inline bars, or empty: the spec's bars file
            std::int64_t patch_ms;  // a bar of the bars file whose low is replaced, or 0
            double patch_low;
            std::vector<Expect> expect;
        };
        const std::int64_t t0 = 1744934400000;  // 2025-04-18 00:00 UTC
        const std::int64_t q = 900000;          // one 15-minute bar
        const std::string veto_spec =
            "config 5894.868194999993 0.01 1.0 1 0.04 100.0 0 1\n"
            "bars f-2026-03-20-2026-03-21.csv\n"
            "session America/New_York 0930-1600:23456\n"
            "ev 1774034100000 entry Short short 516 any\n"
            "ev 1774035000000 exit SE Short 11.49014940701492 notflat\n"
            "ev 1774036800000 close_all - - - any\n";
        const std::int64_t v30 = 1774035000000, v45 = 1774035900000;
        const std::vector<Mechanism> mechanisms = {
            // A carried process_orders_on_close short at slippage 10: the high's
            // call (4 lots) fails the gate, the close's own check (the walk's
            // close call) passes it, and its lagged follow-up is owed to the
            // next bar's open. Separates the gate, the path points and the
            // lagged follow-up.
            {"walk-close-call-owes-next-open",
             "config 302.618 1e-05 0.01 10 0.0 100.0 1 1\nbars -\n"
             "ev 1744934400000 entry S short 275 any\n"
             "ev 1744937100000 close_all - - - any\n",
             {{1.1, 1.1, 1.1, 1.1, 100, t0}, {1.1, 1.1002, 1.0999, 1.10019, 100, t0 + q},
              {1.10019, 1.10019, 1.10019, 1.10019, 100, t0 + 2 * q},
              {1.10019, 1.10019, 1.10019, 1.10019, 100, t0 + 3 * q},
              {1.10019, 1.10019, 1.10019, 1.10019, 100, t0 + 4 * q}},
             0, 0.0,
             {{nullptr, {{t0 + q, 1.10029, 0.04}, {t0 + 2 * q, 1.10029, 0.08},
                         {t0 + 3 * q, 1.10029, 274.88}}},
              {&M::short_path_points, {{t0 + 2 * q, 1.10029, 0.04}, {t0 + 2 * q, 1.10029, 0.08},
                                       {t0 + 3 * q, 1.10029, 274.88}}},
              {&M::lagged_short_follow_up, {{t0 + q, 1.10029, 0.04}, {t0 + 3 * q, 1.10029, 274.96}}},
              {&M::short_call_gate, {{t0 + q, 1.1003, 0.04}, {t0 + q, 1.10029, 0.08},
                                     {t0 + 3 * q, 1.10029, 274.88}}}}},
            // veto/pending-call-before-stop with its 19:45 bar made high-first
            // (low 11.30): the stop is touched on the open's segment to the
            // high, and the pending veto still checks the high first.
            {"veto-open-to-first-extreme", veto_spec, {}, v45, 11.30,
             {{nullptr, {{v30, 11.42, 1}, {v45, 11.53, 32}, {v45, 11.53, 483}}},
              {&M::pending_veto_first, {{v30, 11.42, 1}, {v45, 11.51, 515}}}}},
            // The same with a long stop entry resting at 12.50: another order
            // the path could reach, so the pending veto stands down.
            {"veto-fenced-by-resting-entry",
             veto_spec + "ev 1774035000000 stopentry L long 10@12.5 notflat\n", {}, 0, 0.0,
             {{nullptr, {{v30, 11.42, 1}, {v45, 11.51, 515}}},
              {&M::pending_veto_first, {{v30, 11.42, 1}, {v45, 11.51, 515}}}}},
        };
        auto& schedule_switches = pineforge::source::detail::margin_schedule_switches();
        for (const Mechanism& m : mechanisms) {
            std::istringstream text(m.spec);
            Spec spec = parse_spec(m.name, text);
            std::vector<Bar> bars = m.bars.empty() ? load_bars(spec.bars) : m.bars;
            for (Bar& bar : bars)
                if (bar.timestamp == m.patch_ms) bar.low = m.patch_low;
            for (const Expect& expect : m.expect) {
                schedule_switches = on;
                if (expect.off) schedule_switches.*(expect.off) = false;
                const Outcome got = replay(spec, bars);
                bool same = got.error.empty() && got.rows.size() == expect.rows.size();
                for (std::size_t i = 0; same && i < got.rows.size(); ++i) {
                    const Row& r = got.rows[i];
                    same = r.exit_ms == std::get<0>(expect.rows[i])
                        && r.exit_ticks == std::llround(std::get<1>(expect.rows[i]) / spec.tick)
                        && r.qty_lots == std::llround(std::get<2>(expect.rows[i]) / spec.step);
                }
                std::printf("%s %s%s\n", same ? "MECH-MATCH" : "MECH-DIFF", m.name,
                            expect.off ? " (one switch off)" : "");
                CHECK(same);
            }
        }
        schedule_switches = shipped;
    }

    // Each switch off alone: one pinned here costs at least one tape that
    // matches with every switch on; one pinned in another fixture
    // (load_bearing false: short_call_gate, short_path_points and
    // lagged_short_follow_up by margin_call_rules' short-cutoff-gate tapes
    // as well, close_call_follow_up_at_open by its literal-21) must not gain
    // one.
    const std::set<std::string> all_matched = matching();
    using M = MarginScheduleSwitches;
    const std::vector<Ablation> ablations = {
        {"short_call_gate", &M::short_call_gate},
        {"short_path_points", &M::short_path_points},
        {"lagged_short_follow_up", &M::lagged_short_follow_up},
        {"pending_veto_first", &M::pending_veto_first},
        {"frozen_reversal_close", &M::frozen_reversal_close},
        {"add_signal_close", &M::add_signal_close},
        {"whole_share_lagged_follow_up", &M::whole_share_lagged_follow_up},
        {"long_call_gain_loss", &M::long_call_gain_loss},
        {"close_call_follow_up_at_open", &M::close_call_follow_up_at_open, false},
    };
    CHECK(ablations.size() == sizeof(MarginScheduleSwitches) / sizeof(bool));
    for (const Ablation& ablation : ablations) {
        switches = on;
        switches.*(ablation.flag) = false;
        const std::set<std::string> off = matching();
        std::size_t lost = 0;
        for (const std::string& path : all_matched) lost += off.count(path) == 0;
        for (const std::string& path : off)
            if (!all_matched.count(path))
                std::printf("  ablation %s off gains %s\n", ablation.name, path.c_str());
        std::printf("ablation %s off: %zu/%zu tapes match (%zu lost)\n", ablation.name, off.size(),
                    specs.size(), lost);
        if (ablation.load_bearing) CHECK(lost > 0);
        else CHECK(off.size() <= all_matched.size());
    }
    switches = shipped;
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
