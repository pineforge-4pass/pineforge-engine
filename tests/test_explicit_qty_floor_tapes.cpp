// Explicit strategy.entry quantities and the script-visible equity they are
// sized from, against TradingView
// (tests/fixtures/explicit_qty_floor/README.md).
//   explicit_qty_decimal_floor  an explicit quantity the lot-grid floor would
//        snap UP is floored on its shortest decimal: (E/100)/0.3200000000000003
//        = 3124.9999999999973 trades 3124 on NYSE:F (lot 1), not 0 shares.
//   equity_tick_mark  strategy.equity / strategy.openprofit mark an open
//        position at the tick-built close floor(close / mintick + 0.5) * mintick
//        (10.155 marks 10.15, 10.265 marks 10.27): the order sized from it matches.
// Each host below is a hand port of the control's codegen lowering (codegen
// ee04fc6: strategy_entry / strategy_close arguments, config fields,
// `current_equity() + open_profit(current_bar_.close)` for strategy.equity).
// main() runs every fixture with all switches on (every row must match TV),
// then once per switch with that switch off, where the fixtures that pin the
// switch must fail again.
#include "order_print_tape_fixture.hpp"

#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace explicit_qty_floor {
// Stand-in for an engine without ScriptRuleSwitches (one that predates
// them): the same TU then builds against it, runs every row with the
// engine's own behaviour and skips the switch-off passes. Where the engine
// declares script_rule_switches(), its non-template function wins overload
// resolution over this template.
struct AbsentRuleSwitches {
    bool explicit_qty_decimal_floor = true;
    bool equity_tick_mark = true;
    bool pooc_bracket_skips_inert_exits = true;
};
}  // namespace explicit_qty_floor

namespace pineforge::source::detail {
template <class... Unused>
explicit_qty_floor::AbsentRuleSwitches& script_rule_switches(Unused...) noexcept {
    static explicit_qty_floor::AbsentRuleSwitches absent;
    return absent;
}
}  // namespace pineforge::source::detail

namespace explicit_qty_floor {

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

// "YYYY-MM-DD HH:MM" UTC (the controls' timestamp("UTC", ...)) as epoch ms.
std::int64_t utc(const char* stamp) {
    return order_print_tape::timestamp(stamp) + 8LL * 3600 * 1000;
}

// The symbol facts the tapes ran under (NYSE:F), applied in
// scripts/run_strategy.py's order.
void apply_nyse_f(pineforge::source::PineStrategyHost& host) {
    host.set_syminfo_timezone("America/New_York");
    host.set_syminfo_session("0930-1600");
    host.set_syminfo_type("stock");
    host.set_syminfo_string("ticker", "F");
    host.set_syminfo_string("tickerid", "NYSE:F");
    host.set_syminfo_metadata("qty_step", 1.0);
    host.set_syminfo_mintick(0.01);
    host.set_syminfo_pointvalue(1.0);
}

// str.tostring(x, "#.##########"): ten decimals, trailing zeros dropped.
std::string ten_decimals(double value) {
    char text[64];
    std::snprintf(text, sizeof text, "%.10f", value);
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    if (out == "-0") out = "0";
    return out;
}

struct Readout {
    bool taken = false;
    double equity = missing;
    double open_profit = missing;
    double net_profit = missing;
    double position = missing;
};

// equity-mark-*: NYSE:F 15m, one explicit position, then a read bar whose order is
// sized from strategy.equity (qty = e * 0.01 / risk) and prints
// E=<equity>|O=<openprofit>|N=<netprofit>|P=<position_size> as its comment.
struct EquityMarkPlan {
    const char* open_time;
    const char* open_id;
    bool open_long;
    double open_qty;
    const char* read_time;
    const char* read_id;
    double risk;
    const char* close_time;
    double commission_percent;
};

class EquityMarkHost final : public pineforge::source::PineStrategyHost {
public:
    explicit EquityMarkHost(const EquityMarkPlan& plan) : plan_(plan) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig cfg{};
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.commission_type = static_cast<int>(pineforge::CommissionType::PERCENT);
        cfg.commission_value = plan.commission_percent;
        cfg.slippage = 1;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        configure_pine_strategy(cfg);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (current_bar_.timestamp == utc(plan_.open_time)) {
            strategy_entry(plan_.open_id, plan_.open_long, missing, missing, plan_.open_qty,
                           "", "", 0, -1);
        }
        if (current_bar_.timestamp == utc(plan_.read_time)) {
            e_ = current_equity() + open_profit(current_bar_.close);
            read_.taken = true;
            read_.equity = e_;
            read_.open_profit = open_profit(current_bar_.close);
            read_.net_profit = net_profit();
            read_.position = signed_position_size();
            strategy_entry(plan_.read_id, !plan_.open_long, missing, missing,
                           (e_ * 0.01) / plan_.risk,
                           "E=" + ten_decimals(e_) + "|O="
                               + ten_decimals(open_profit(current_bar_.close)) + "|N="
                               + ten_decimals(net_profit()) + "|P="
                               + ten_decimals(signed_position_size()),
                           "", 0, -1);
        }
        if (current_bar_.timestamp == utc(plan_.close_time))
            strategy_close("", "X", missing, missing, false);
    }

    const Readout& readout() const { return read_; }

private:
    const EquityMarkPlan plan_;
    double e_ = 0.0;
    Readout read_;
};

// computed-risk-* and literal-*: NYSE:F 1D, process_orders_on_close, one short
// at 2025-09-05 sized by a risk formula or a literal, optionally split into TP1 (50%) and
// MAIN (stop 12.06) legs.
struct RiskFloorPlan {
    bool computed;
    double literal_qty;
    bool split;
    const char* close_time;
};

class RiskFloorHost final : public pineforge::source::PineStrategyHost {
public:
    explicit RiskFloorHost(const RiskFloorPlan& plan) : plan_(plan) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig cfg{};
        cfg.process_orders_on_close = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = 1;
        cfg.margin_long = 1.0;
        cfg.margin_short = 1.0;
        configure_pine_strategy(cfg);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (current_bar_.timestamp == utc("2025-09-05 13:30")) {
            r_ = (11.91 + (syminfo_.mintick * 15)) - 11.74;
            const double qty = plan_.computed
                ? ((current_equity() + open_profit(current_bar_.close)) * 1.0 / 100.0) / r_
                : plan_.literal_qty;
            strategy_entry("S", false, missing, missing, qty, "", "", 0, -1);
            if (plan_.split) {
                strategy_exit("TP1", "S", 10.0, missing, missing, missing, missing, 50, "",
                              missing, "", missing, missing);
                strategy_exit("MAIN", "S", missing, 12.06, missing, missing, missing, 100.0, "",
                              missing, "", missing, missing);
            }
        }
        if (current_bar_.timestamp == utc(plan_.close_time))
            strategy_close("", "X", missing, missing, false);
    }

private:
    const RiskFloorPlan plan_;
    double r_ = 0.0;
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

std::string fixture_path(const std::string& fixture, const char* file) {
    return std::string(PINEFORGE_ORDER_PRINT_FIXTURE_ROOT) + "/explicit_qty_floor/" + fixture
        + "/" + file;
}

std::vector<TvRow> tv_rows(const std::string& fixture) {
    std::ifstream input(fixture_path(fixture, "tv_trades.csv"));
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

std::vector<pineforge::Bar> fixture_bars(const std::string& fixture) {
    return order_print_tape::bars("explicit_qty_floor/" + fixture);
}

// TradingView's ws report prints PnL at float32 precision.
bool pnl_matches(double actual, double tv) {
    return std::abs(actual - tv) <= std::max(1e-4, 1.2e-7 * std::abs(tv));
}

int compare_rows(const pineforge::source::PineStrategyHost& host, const std::string& fixture,
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
                std::printf("  %s row %zu: engine %s %lld @%.8f -> %lld @%.8f qty %.8f pnl %.8f;"
                            " TV %s %lld @%.8f -> %lld @%.8f qty %.8f pnl %.8f\n",
                            fixture.c_str(), i + 1, actual.is_long ? "long" : "short",
                            static_cast<long long>(actual.entry_time), actual.entry_price,
                            static_cast<long long>(actual.exit_time), actual.exit_price,
                            actual.qty, actual.pnl, tv.is_long ? "long" : "short",
                            static_cast<long long>(tv.entry_time), tv.entry_price,
                            static_cast<long long>(tv.exit_time), tv.exit_price, tv.qty, tv.pnl);
            }
        }
    }
    return failures;
}

// The TV Signal of the read bar's entry: E=..|O=..|N=..|P=.. (ten decimals).
int compare_readout(const Readout& readout, const std::string& fixture,
                    const std::vector<TvRow>& expected, bool verbose) {
    std::string signal;
    for (const auto& row : expected)
        if (row.entry_signal.rfind("E=", 0) == 0) signal = row.entry_signal;
    const std::string actual = readout.taken
        ? "E=" + ten_decimals(readout.equity) + "|O=" + ten_decimals(readout.open_profit)
              + "|N=" + ten_decimals(readout.net_profit) + "|P="
              + ten_decimals(readout.position)
        : std::string("(no read bar)");
    if (!signal.empty() && actual == signal) return 0;
    if (verbose) {
        std::printf("  %s readout: engine %s; TV %s\n", fixture.c_str(), actual.c_str(),
                    signal.empty() ? "(missing)" : signal.c_str());
    }
    return 1;
}

struct EquityCase {
    const char* fixture;
    EquityMarkPlan plan;
};

struct RiskCase {
    const char* fixture;
    RiskFloorPlan plan;
};

const std::vector<EquityCase>& equity_cases() {
    static const std::vector<EquityCase> cases = {
        {"equity-mark-long-fee", {"2025-04-01 19:45", "L", true, 9619, "2025-04-02 17:30", "S",
                                  0.11287437863754, "2025-04-04 15:00", 0.05}},
        {"equity-mark-long-nofee", {"2025-04-01 19:45", "L", true, 9619, "2025-04-02 17:30", "S",
                                    0.11287437863754, "2025-04-04 15:00", 0.0}},
        {"equity-mark-short-fee", {"2025-04-02 17:30", "S", false, 9110, "2025-04-04 15:00", "L",
                                   0.29858127052438, "2025-04-07 13:30", 0.05}},
        {"equity-mark-long-fee-round-up", {"2025-04-01 19:45", "L", true, 9619,
                                           "2025-04-02 17:00", "S", 0.12, "2025-04-04 15:00",
                                           0.05}},
    };
    return cases;
}

const std::vector<RiskCase>& risk_cases() {
    static const std::vector<RiskCase> cases = {
        {"computed-risk-1d", {true, 0.0, false, "2025-09-26 13:30"}},
        {"literal-below-5e-7-1d", {false, 3124.9999995, false, "2025-09-26 13:30"}},
        {"literal-below-1e-5-1d", {false, 3124.99999, false, "2025-09-26 13:30"}},
        {"literal-above-1d", {false, 3125.0000000000005, false, "2025-09-26 13:30"}},
        {"computed-risk-split-1d", {true, 0.0, true, "2025-10-15 13:30"}},
    };
    return cases;
}

// Failures per fixture (rows + readout), in equity_cases() then risk_cases()
// order.
std::vector<int> run_all(bool verbose) {
    std::vector<int> failures;
    for (const auto& item : equity_cases()) {
        EquityMarkHost host(item.plan);
        apply_nyse_f(host);
        const auto feed = fixture_bars(item.fixture);
        host.run(feed.data(), static_cast<int>(feed.size()), "15", "15", false, 4,
                 static_cast<pineforge::MagnifierDistribution>(3));
        const auto expected = tv_rows(item.fixture);
        const int rows = compare_rows(host, item.fixture, expected, verbose);
        const int read = compare_readout(host.readout(), item.fixture, expected, verbose);
        if (verbose) {
            std::printf("%s: %zu TV trades, %d engine trades, %d row failures, readout %s\n",
                        item.fixture, expected.size(), host.trade_count(), rows,
                        read == 0 ? "match" : "DIFFERS");
        }
        failures.push_back(rows + read);
    }
    for (const auto& item : risk_cases()) {
        RiskFloorHost host(item.plan);
        apply_nyse_f(host);
        const auto feed = fixture_bars(item.fixture);
        host.run(feed.data(), static_cast<int>(feed.size()), "1D", "1D", false, 4,
                 static_cast<pineforge::MagnifierDistribution>(3));
        const auto expected = tv_rows(item.fixture);
        const int rows = compare_rows(host, item.fixture, expected, verbose);
        if (verbose) {
            std::printf("%s: %zu TV trades, %d engine trades, %d row failures\n", item.fixture,
                        expected.size(), host.trade_count(), rows);
        }
        failures.push_back(rows);
    }
    return failures;
}

std::vector<std::string> fixture_names() {
    std::vector<std::string> names;
    for (const auto& item : equity_cases()) names.emplace_back(item.fixture);
    for (const auto& item : risk_cases()) names.emplace_back(item.fixture);
    return names;
}

}  // namespace explicit_qty_floor

int main() {
    using namespace explicit_qty_floor;
    const auto names = fixture_names();
    std::printf("== all rules on%s\n",
                kEngineHasRuleSwitches ? "" : " (engine without ScriptRuleSwitches)");
    const auto baseline = run_all(true);
    int failures = 0;
    for (const int count : baseline) failures += count;
    if (!kEngineHasRuleSwitches) {
        std::printf("RESULT: %d failures (no switch passes: the engine has no switches)\n",
                    failures);
        return failures == 0 ? 0 : 1;
    }
    // Each switch off in turn: the fixtures it pins must fail again, the
    // others must keep matching (the rules are independent on these tapes).
    struct SwitchPass {
        const char* name;
        bool RuleSwitches::*flag;
        std::vector<std::string> pinned;
    };
    const std::vector<SwitchPass> passes = {
        {"explicit_qty_decimal_floor",
         &RuleSwitches::explicit_qty_decimal_floor,
         {"literal-below-5e-7-1d", "computed-risk-1d", "computed-risk-split-1d"}},
        {"equity_tick_mark", &RuleSwitches::equity_tick_mark,
         {"equity-mark-long-fee", "equity-mark-long-nofee", "equity-mark-short-fee",
          "equity-mark-long-fee-round-up"}},
    };
    auto& switches = pineforge::source::detail::script_rule_switches();
    for (const auto& pass : passes) {
        switches.*(pass.flag) = false;
        std::printf("== %s off\n", pass.name);
        const auto off = run_all(true);
        switches.*(pass.flag) = true;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool pinned = std::find(pass.pinned.begin(), pass.pinned.end(), names[i])
                != pass.pinned.end();
            if (pinned && off[i] == 0) {
                ++failures;
                std::printf("SWITCH NOT PINNED: %s still matches with %s off\n",
                            names[i].c_str(), pass.name);
            }
            if (!pinned && off[i] != 0) {
                ++failures;
                std::printf("UNEXPECTED: %s fails with %s off\n", names[i].c_str(), pass.name);
            }
        }
    }
    std::printf("RESULT: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
