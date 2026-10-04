// process_orders_on_close: the close-time script sees every fill of its bar,
// against TradingView (tests/fixtures/pooc_close_bar_fills/README.md).
//   pooc_bracket_skips_inert_exits  a stop entry and its bracket exit that
//       both fill inside one bar are
//       settled before the close-time script runs, so `position_size == 0`
//       there and a stop re-armed at the close fills at that close (BINANCE:
//       BTCUSDT 15m, 2025-08-03 10:00: stop 113689.29 then limit 113771.41,
//       re-arm stop 113771.47 = close fills 10:00, exits 11:00 @114004.53),
//       even when the book still holds an exit whose entry is neither open
//       nor working (the cancelled 2025-08-02 "Sell" re-arms' "Sell Exit").
// One host runs every control from a per-control block table, a hand port of
// its codegen lowering (codegen ee04fc6: strategy_cancel / strategy_entry /
// strategy_exit / strategy_close arguments and config fields; the guard reads
// signed_position_size() with codegen's 1e-10 equality). Guarded controls
// print flatAtClose|pos=|ct=|ot= or openAtClose|... in the re-arm's Signal; the
// test asserts the engine took TV's branch with TV's readouts. main() runs
// every fixture with all switches on, then with
// pooc_bracket_skips_inert_exits off, where the dormant-exit fixtures must
// fail again.
#include "order_print_tape_fixture.hpp"

#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace pooc_close_bar_fills {
// Stand-in for an engine without ScriptRuleSwitches (one that predates
// them): the same TU then builds against it and skips the switch-off
// pass. Where the engine declares script_rule_switches(), its non-template
// function wins overload resolution over this template.
struct AbsentRuleSwitches {
    bool explicit_qty_decimal_floor = true;
    bool equity_tick_mark = true;
    bool pooc_bracket_skips_inert_exits = true;
};
}  // namespace pooc_close_bar_fills

namespace pineforge::source::detail {
template <class... Unused>
pooc_close_bar_fills::AbsentRuleSwitches& script_rule_switches(Unused...) noexcept {
    static pooc_close_bar_fills::AbsentRuleSwitches absent;
    return absent;
}
}  // namespace pineforge::source::detail

namespace pooc_close_bar_fills {

using order_print_tape::missing;

using RuleSwitches = std::decay_t<decltype(pineforge::source::detail::script_rule_switches())>;
constexpr bool kEngineHasRuleSwitches = !std::is_same_v<RuleSwitches, AbsentRuleSwitches>;
// The fallback above only lets this TU compile against an engine without the
// switches, for the recorded fail-before run (-DPINEFORGE_FAIL_BEFORE_WITHOUT_SWITCHES
// against main 015dd9ac). Everywhere else a renamed or moved switch must not
// quietly turn the switch-off passes into a skip.
#ifndef PINEFORGE_FAIL_BEFORE_WITHOUT_SWITCHES
static_assert(kEngineHasRuleSwitches,
              "pineforge::source::detail::script_rule_switches() must be declared");
#endif

std::int64_t utc(const char* stamp) {
    return order_print_tape::timestamp(stamp) + 8LL * 3600 * 1000;
}

// The symbol facts the tapes ran under (BINANCE:BTCUSDT), applied in
// scripts/run_strategy.py's order.
void apply_binance_btcusdt(pineforge::source::PineStrategyHost& host) {
    host.set_syminfo_timezone("Etc/UTC");
    host.set_syminfo_session("24x7");
    host.set_syminfo_type("crypto");
    host.set_syminfo_string("ticker", "BTCUSDT");
    host.set_syminfo_string("tickerid", "BINANCE:BTCUSDT");
    host.set_syminfo_metadata("qty_step", 0.00001);
    host.set_syminfo_mintick(0.01);
    host.set_syminfo_pointvalue(1.0);
}

enum class Kind { Cancel, Entry, Exit, CloseAll };

struct Action {
    Kind kind;
    std::string id;
    std::string from_entry;  // Exit: its entry; Entry: "L"/"S"
    double limit = missing;
    double stop = missing;
    double qty = missing;
    bool readout = false;  // Entry: comment is the branch readout
};

Action cancel(const char* id) { return {Kind::Cancel, id, "", missing, missing, missing, false}; }
Action entry(const char* id, bool is_long, double stop, double qty = missing,
             bool readout = false) {
    return {Kind::Entry, id, is_long ? "L" : "S", missing, stop, qty, readout};
}
Action exit_bracket(const char* id, const char* from, double stop, double limit) {
    return {Kind::Exit, id, from, limit, stop, missing, false};
}
Action close_all(const char* comment) {
    return {Kind::CloseAll, comment, "", missing, missing, missing, false};
}

enum class Guard { None, FlatElseAlt, FlatOnly };

struct Block {
    const char* time;
    Guard guard;
    std::vector<Action> flat;  // the block, or its `position_size == 0` branch
    std::vector<Action> alt;   // the else branch
};

struct Control {
    const char* fixture;
    double initial_capital;
    bool percent_of_equity;
    double default_qty_value;
    bool pooc;
    std::vector<Block> blocks;
    // Fixtures whose rows need pooc_bracket_skips_inert_exits.
    bool pins_inert_exit_rule;
};

struct Branch {
    bool taken = false;
    std::string name;
    double position = missing;
    int closed = -1;
    int open = -1;
};

class ScheduledBlocksHost final : public pineforge::source::PineStrategyHost {
public:
    explicit ScheduledBlocksHost(const Control& control) : control_(control) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = control.pooc;
        cfg.initial_capital = control.initial_capital;
        cfg.default_qty_type = static_cast<int>(control.percent_of_equity
            ? pineforge::QtyType::PERCENT_OF_EQUITY : pineforge::QtyType::FIXED);
        cfg.default_qty_value = control.default_qty_value;
        cfg.pyramiding = 0;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
    }

    void on_source_bar(const pineforge::Bar&) override {
        for (const auto& block : control_.blocks) {
            if (current_bar_.timestamp != utc(block.time)) continue;
            if (block.guard == Guard::None) {
                perform(block.flat, "");
                continue;
            }
            const double position = signed_position_size();
            const bool flat = position == 0.0
                || (std::isfinite(position) && std::fabs(position) <= 1e-10);
            if (block.guard == Guard::FlatOnly) {
                if (flat) perform(block.flat, "");
                continue;
            }
            branch_.taken = true;
            branch_.name = flat ? "flatAtClose" : "openAtClose";
            branch_.position = position;
            branch_.closed = static_cast<int>(trades_.size());
            branch_.open = static_cast<int>(pyramid_entries_.size());
            perform(flat ? block.flat : block.alt,
                    branch_.name + "|pos=" + number(position) + "|ct="
                        + std::to_string(branch_.closed) + "|ot="
                        + std::to_string(branch_.open));
        }
    }

    const Branch& branch() const { return branch_; }

private:
    static std::string number(double value) {
        char text[64];
        std::snprintf(text, sizeof text, "%.10f", value);
        std::string out(text);
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
        return out == "-0" ? "0" : out;
    }

    void perform(const std::vector<Action>& actions, const std::string& readout) {
        for (const auto& action : actions) {
            switch (action.kind) {
            case Kind::Cancel:
                strategy_cancel(action.id);
                break;
            case Kind::Entry:
                strategy_entry(action.id, action.from_entry == "L", missing, action.stop,
                               action.qty, action.readout ? readout : std::string(), "", 0, -1);
                break;
            case Kind::Exit:
                strategy_exit(action.id, action.from_entry, action.limit, action.stop, missing,
                              missing, missing, 100.0, "", missing, "", missing, missing);
                break;
            case Kind::CloseAll:
                strategy_close("", action.id, missing, missing, false);
                break;
            }
        }
    }

    const Control control_;
    Branch branch_;
};

struct TvRow {
    bool seen_entry = false;
    bool seen_exit = false;
    bool is_long = false;
    std::int64_t entry_time = 0;
    std::int64_t exit_time = 0;
    double entry_price = missing;
    double exit_price = missing;
    double qty = missing;
    double pnl = missing;
    std::string entry_signal;
};

std::vector<TvRow> tv_rows(const std::string& fixture) {
    std::ifstream input(std::string(PINEFORGE_ORDER_PRINT_FIXTURE_ROOT)
                        + "/pooc_close_bar_fills/" + fixture + "/tv_trades.csv");
    if (!input) throw std::runtime_error("missing TradingView tape: " + fixture);
    std::string line;
    std::getline(input, line);
    std::vector<TvRow> rows;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto row = order_print_tape::fields(line);
        if (row.size() < 8) throw std::runtime_error("invalid TradingView row: " + line);
        const auto number = static_cast<std::size_t>(std::stoi(row[0]));
        if (number == 0) throw std::runtime_error("invalid trade number: " + line);
        if (rows.size() < number) rows.resize(number);
        auto& trade = rows[number - 1];
        if (row[1].rfind("Entry", 0) == 0) {
            trade.seen_entry = true;
            trade.is_long = row[1] == "Entry long";
            trade.entry_time = order_print_tape::timestamp(row[2]);
            trade.entry_signal = row[3];
            trade.entry_price = std::stod(row[4]);
        } else {
            trade.seen_exit = true;
            trade.exit_time = order_print_tape::timestamp(row[2]);
            trade.exit_price = std::stod(row[4]);
        }
        trade.qty = std::stod(row[5]);
        trade.pnl = std::stod(row[7]);
    }
    return rows;
}

bool pnl_matches(double actual, double tv) {
    return std::abs(actual - tv) <= std::max(1e-4, 1.2e-7 * std::abs(tv));
}

int compare_rows(const ScheduledBlocksHost& host, const std::string& fixture,
                 const std::vector<TvRow>& expected, bool verbose) {
    int failures = 0;
    if (!host.last_error().empty()) {
        ++failures;
        if (verbose) std::printf("  %s: engine error %s\n", fixture.c_str(), host.last_error().c_str());
    }
    if (host.trade_count() != static_cast<int>(expected.size())) {
        ++failures;
        if (verbose) {
            std::printf("  %s: %d engine trades, %zu TV trades\n", fixture.c_str(),
                        host.trade_count(), expected.size());
        }
    }
    const double tick = 0.01;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& tv = expected[i];
        if (!tv.seen_entry || !tv.seen_exit) {
            ++failures;
            if (verbose) std::printf("  %s: TV trade %zu lacks a leg\n", fixture.c_str(), i + 1);
            continue;
        }
        if (static_cast<int>(i) >= host.trade_count()) continue;
        const auto& actual = host.get_trade(static_cast<int>(i));
        const bool matches = actual.is_long == tv.is_long
            && actual.entry_time == tv.entry_time && actual.exit_time == tv.exit_time
            && std::llround(actual.entry_price / tick) == std::llround(tv.entry_price / tick)
            && std::llround(actual.exit_price / tick) == std::llround(tv.exit_price / tick)
            && std::abs(actual.qty - tv.qty) < 1e-8 && pnl_matches(actual.pnl, tv.pnl);
        if (!matches) {
            ++failures;
            if (verbose) {
                std::printf("  %s row %zu: engine %s %s %lld @%.8f -> %lld @%.8f qty %.8f pnl"
                            " %.8f; TV %s %lld @%.8f -> %lld @%.8f qty %.8f pnl %.8f\n",
                            fixture.c_str(), i + 1, actual.is_long ? "long" : "short",
                            actual.entry_id.c_str(), static_cast<long long>(actual.entry_time),
                            actual.entry_price, static_cast<long long>(actual.exit_time),
                            actual.exit_price, actual.qty, actual.pnl,
                            tv.is_long ? "long" : "short",
                            static_cast<long long>(tv.entry_time), tv.entry_price,
                            static_cast<long long>(tv.exit_time), tv.exit_price, tv.qty, tv.pnl);
            }
        }
    }
    return failures;
}

// TV's re-arm Signal flatAtClose|pos=0|ct=N|ot=0 (or openAtClose|...) names the
// branch the close-time script took and the state it read.
int compare_branch(const Branch& branch, const std::string& fixture,
                   const std::vector<TvRow>& expected, bool verbose) {
    std::string signal;
    for (const auto& row : expected) {
        if (row.entry_signal.rfind("flatAtClose|", 0) == 0
            || row.entry_signal.rfind("openAtClose|", 0) == 0) {
            signal = row.entry_signal;
        }
    }
    std::string tv_name;
    double tv_position = missing;
    int tv_closed = -1;
    int tv_open = -1;
    if (!signal.empty()) {
        const auto bar = signal.find('|');
        tv_name = signal.substr(0, bar);
        char pos[64] = {0};
        if (std::sscanf(signal.c_str() + bar, "|pos=%63[^|]|ct=%d|ot=%d", pos, &tv_closed,
                        &tv_open) == 3) {
            tv_position = std::stod(pos);
        }
    }
    const bool matches = branch.taken && !signal.empty() && branch.name == tv_name
        && branch.position == tv_position && branch.closed == tv_closed
        && branch.open == tv_open;
    if (!matches && verbose) {
        std::printf("  %s branch: engine %s pos=%.10g ct=%d ot=%d; TV %s\n", fixture.c_str(),
                    branch.taken ? branch.name.c_str() : "(not taken)", branch.position,
                    branch.closed, branch.open, signal.empty() ? "(missing)" : signal.c_str());
    }
    return matches ? 0 : 1;
}

// ---- the controls' blocks ----------------------------------------------------

const double kPopCapital = 92861.56;      // vasudev's equity before 2025-08-03
const double kMarginCapital = 92568.37;   // ... before the 2025-08-02 short

Block at_0945(const char* id = "Buy", double stop = 113689.29, double limit = 113771.41) {
    const std::string exit_id = std::string(id) + " Exit";
    return {"2025-08-03 09:45", Guard::None,
            {cancel("Sell"), entry(id, true, stop),
             exit_bracket(exit_id.c_str(), id, 113400.0, limit)},
            {}};
}
Block at_1000(bool cancel_buy = true) {
    std::vector<Action> actions;
    if (cancel_buy) actions.push_back(cancel("Buy"));
    actions.push_back(cancel("Sell"));
    actions.push_back(entry("Buy", true, 113771.47));
    actions.push_back(exit_bracket("Buy Exit", "Buy", 113400.0, 114004.53));
    return {"2025-08-03 10:00", Guard::None, actions, {}};
}
Block at_1000_guarded() {
    return {"2025-08-03 10:00", Guard::FlatElseAlt,
            {cancel("Buy"), cancel("Sell"), entry("Buy", true, 113771.47, missing, true),
             exit_bracket("Buy Exit", "Buy", 113400.0, 114004.53)},
            {entry("Alt", true, 113780.0, missing, true),
             exit_bracket("Alt Exit", "Alt", 113400.0, 114004.53)}};
}
Block at_1200() { return {"2025-08-03 12:00", Guard::None, {close_all("flat")}, {}}; }
// The margin-at-open short of 2025-08-02 13:00 (v11q / v13q: wide stop; v12q:
// 0.5 BTC, no margin call).
Block margin_short(double qty = missing) {
    return {"2025-08-02 13:00", Guard::None,
            {entry("Sell", false, 113450.0, qty),
             exit_bracket("Sell Exit", "Sell", 114500.0, 113090.0)},
            {}};
}
// The member's own 2025-08-02 / 08-03 order history (vasudev, trades 227/228
// and the refused re-arms after them).
std::vector<Block> member_history(bool rearms) {
    std::vector<Block> blocks = {
        {"2025-08-02 13:00", Guard::None,
         {cancel("Buy"), entry("Sell", false, 113450.0),
          exit_bracket("Sell Exit", "Sell", 113796.2450040561, 113090.0)},
         {}},
    };
    if (rearms) {
        blocks.push_back({"2025-08-02 14:30", Guard::None,
                          {cancel("Buy"), entry("Sell", false, 112977.0),
                           exit_bracket("Sell Exit", "Sell", 113722.1751045026, 112405.64)},
                          {}});
        blocks.push_back({"2025-08-02 14:45", Guard::None,
                          {cancel("Sell"), cancel("Buy"), entry("Sell", false, 112939.39),
                           exit_bracket("Sell Exit", "Sell", 113699.2003492775, 112490.17)},
                          {}});
        blocks.push_back({"2025-08-02 15:00", Guard::None,
                          {cancel("Sell"), cancel("Buy"), entry("Sell", false, 113090.01),
                           exit_bracket("Sell Exit", "Sell", 113672.4403318136, 112719.79)},
                          {}});
    }
    blocks.push_back({"2025-08-02 15:15", Guard::None,
                      {cancel("Buy"), cancel("Sell"), close_all("Session Close")}, {}});
    blocks.push_back({"2025-08-03 09:30", Guard::None,
                      {cancel("Sell"), entry("Buy", true, 113692.47),
                       exit_bracket("Buy Exit", "Buy", 113354.5805547498, 113808.37)},
                      {}});
    blocks.push_back({"2025-08-03 09:45", Guard::None,
                      {cancel("Buy"), cancel("Sell"), entry("Buy", true, 113689.29),
                       exit_bracket("Buy Exit", "Buy", 113368.2917347088, 113771.41)},
                      {}});
    return blocks;
}
std::vector<Block> with(std::vector<Block> blocks, std::initializer_list<Block> tail) {
    blocks.insert(blocks.end(), tail.begin(), tail.end());
    return blocks;
}
// The earlier scheduled-stop tapes (initial capital 1e6, explicit qty 1).
std::vector<Block> scheduled_stop(bool cancel_when_flat) {
    std::vector<Block> blocks = {
        {"2025-08-03 10:00", Guard::None,
         {entry("scheduled", true, 113771.47, 1.0),
          exit_bracket("bracket", "scheduled", 113000.0, 114004.53)},
         {}},
    };
    if (cancel_when_flat)
        blocks.push_back({"2025-08-03 10:15", Guard::FlatOnly, {cancel("scheduled")}, {}});
    blocks.push_back({"2025-08-03 12:00", Guard::None, {close_all("")}, {}});
    return blocks;
}

const std::vector<Control>& controls() {
    static const std::vector<Control> all = {
        {"rearm-pop-capital", kPopCapital, true, 100.0, true,
         {at_0945(), at_1000(), at_1200()}, false},
        {"rearm-capital-100k", 100000.0, true, 100.0, true,
         {at_0945(), at_1000(), at_1200()}, false},
        {"rearm-fixed-qty", 100000.0, false, 0.5, true, {at_0945(), at_1000(), at_1200()},
         false},
        {"rearm-no-cancel", kPopCapital, true, 100.0, true,
         {at_0945(), at_1000(false), at_1200()}, false},
        {"rearm-no-prior", kPopCapital, true, 100.0, true, {at_1000(), at_1200()}, false},
        {"rearm-prior-unfilled", kPopCapital, true, 100.0, true,
         {at_0945("Buy", 113800.0, 113900.0), at_1000(), at_1200()}, false},
        {"rearm-pooc-off", kPopCapital, true, 100.0, false, {at_0945(), at_1000(), at_1200()},
         false},
        {"rearm-prior-other-id", kPopCapital, true, 100.0, true,
         {at_0945("Pre"), at_1000(), at_1200()}, false},
        {"guard-prior-fill", kPopCapital, true, 100.0, true,
         {at_0945(), at_1000_guarded(), at_1200()}, false},
        {"guard-pooc-off", kPopCapital, true, 100.0, false,
         {at_0945(), at_1000_guarded(), at_1200()}, false},
        {"guard-no-prior", kPopCapital, true, 100.0, true, {at_1000_guarded(), at_1200()},
         false},
        {"guard-margin-prior", kMarginCapital, true, 100.0, true,
         {margin_short(), at_0945(), at_1000_guarded(), at_1200()}, false},
        {"guard-fixed-prior", kMarginCapital, true, 100.0, true,
         {margin_short(0.5), at_0945(), at_1000_guarded(), at_1200()}, false},
        {"rearm-margin-prior", kMarginCapital, true, 100.0, true,
         {margin_short(), at_0945(), at_1000(), at_1200()}, false},
        {"guard-dormant-exit-history", kMarginCapital, true, 100.0, true,
         with(member_history(true), {at_1000_guarded(), at_1200()}), true},
        {"rearm-dormant-exit-history", kMarginCapital, true, 100.0, true,
         with(member_history(true), {at_1000(), at_1200()}), true},
        {"guard-margin-history-no-rearms", kMarginCapital, true, 100.0, true,
         with(member_history(false), {at_1000_guarded(), at_1200()}), false},
        {"scheduled-stop-pooc-cancel", 1000000.0, true, 100.0, true, scheduled_stop(true),
         false},
        {"scheduled-stop-pooc-keep", 1000000.0, true, 100.0, true, scheduled_stop(false),
         false},
        {"scheduled-stop-next-cancel", 1000000.0, true, 100.0, false, scheduled_stop(true),
         false},
        {"scheduled-stop-next-keep", 1000000.0, true, 100.0, false, scheduled_stop(false),
         false},
    };
    return all;
}

std::vector<int> run_all(bool verbose) {
    std::vector<int> failures;
    for (const auto& control : controls()) {
        ScheduledBlocksHost host(control);
        apply_binance_btcusdt(host);
        const auto feed = order_print_tape::bars(std::string("pooc_close_bar_fills/")
                                                 + control.fixture);
        host.run(feed.data(), static_cast<int>(feed.size()), "15", "15", false, 4,
                 static_cast<pineforge::MagnifierDistribution>(3));
        const auto expected = tv_rows(control.fixture);
        const int rows = compare_rows(host, control.fixture, expected, verbose);
        const bool guarded = std::any_of(control.blocks.begin(), control.blocks.end(),
            [](const Block& block) { return block.guard == Guard::FlatElseAlt; });
        const int branch = guarded ? compare_branch(host.branch(), control.fixture, expected,
                                                    verbose)
                                   : 0;
        if (verbose) {
            std::printf("%s: %zu TV trades, %d engine trades, %d row failures%s\n",
                        control.fixture, expected.size(), host.trade_count(), rows,
                        guarded ? (branch == 0 ? ", branch match" : ", branch DIFFERS") : "");
        }
        failures.push_back(rows + branch);
    }
    return failures;
}

}  // namespace pooc_close_bar_fills

int main() {
    using namespace pooc_close_bar_fills;
    std::printf("== all rules on%s\n",
                kEngineHasRuleSwitches ? "" : " (engine without ScriptRuleSwitches)");
    const auto baseline = run_all(true);
    int failures = 0;
    for (const int count : baseline) failures += count;
    if (!kEngineHasRuleSwitches) {
        std::printf("RESULT: %d failures (no switch pass: the engine has no switches)\n",
                    failures);
        return failures == 0 ? 0 : 1;
    }
    auto& switches = pineforge::source::detail::script_rule_switches();
    switches.*(&RuleSwitches::pooc_bracket_skips_inert_exits) = false;
    std::printf("== pooc_bracket_skips_inert_exits off\n");
    const auto off = run_all(true);
    switches.*(&RuleSwitches::pooc_bracket_skips_inert_exits) = true;
    const auto& all = controls();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].pins_inert_exit_rule && off[i] == 0) {
            ++failures;
            std::printf("SWITCH NOT PINNED: %s still matches with the switch off\n",
                        all[i].fixture);
        }
        if (!all[i].pins_inert_exit_rule && off[i] != 0) {
            ++failures;
            std::printf("UNEXPECTED: %s fails with the switch off\n", all[i].fixture);
        }
    }
    std::printf("RESULT: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
