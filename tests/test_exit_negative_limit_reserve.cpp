/*
 * OUTPUT FOR: native Pine strategy.exit placement and quantity reservation:
 * an unreachable short buy limit still holds its share for later exits.
 * WRONG LOOKS LIKE: TP2 takes its full percentage because TP1's negative
 * limit vanished, or a limit/stop pair reserves the same share twice.
 * CHECKED BY: PineStrategyHost commands reaching PineExecutionAdapter::exit,
 * synthetic bars, scenario-derived quantities, and complete control trades.
 * No Pine script, corpus fixture, or reference tape is interpreted here.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEntryUnits = 10.0;
constexpr double kFirstPercent = 60.0;
constexpr double kSecondPercent = 60.0;
constexpr double kFullPercent = 100.0;
constexpr double kEntryPrice = 100.0;
constexpr double kShortTarget = 99.0;
constexpr double kLongTarget = 101.0;
constexpr double kShortStop = 101.0;
constexpr double kNegativeLimit = -20.0;
constexpr double kAverageMultiplier = -0.5;
constexpr double kShortControl = 1.0;
constexpr double kLongControl = 1e9;
constexpr int kEntrySignalBar = 0;
constexpr int kEntryFillBar = kEntrySignalBar + 1;

enum class FirstLimit {
    NegativeConstant, NegativeAverage, Control, NoLevel, Zero,
    Omitted
};

struct Scenario {
    FirstLimit first_limit = FirstLimit::NegativeConstant;
    int hit_bar = 3;
    double first_percent = kFirstPercent;
    bool is_long = false;
    bool with_stop = false;
    bool same_bar = false;
    bool all_entries = false;
};

class ExitProbe final : public source::PineStrategyHost {
public:
    explicit ExitProbe(const Scenario& scenario) : scenario_(scenario) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kEntryUnits;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.001);
    }

    double position() const { return signed_position_size(); }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == kEntrySignalBar) {
            strategy_entry("E", scenario_.is_long, kNaN, kNaN, kEntryUnits);
            if (scenario_.same_bar) issue_exits();
        }
        if (signed_position_size() != 0.0) issue_exits();
    }

private:
    double first_limit() const {
        switch (scenario_.first_limit) {
        case FirstLimit::NegativeConstant: return kNegativeLimit;
        case FirstLimit::NegativeAverage:
            return position_avg_price() * kAverageMultiplier;
        case FirstLimit::Control:
            return scenario_.is_long ? kLongControl : kShortControl;
        case FirstLimit::Zero: return 0.0;
        case FirstLimit::NoLevel:
        case FirstLimit::Omitted: return kNaN;
        }
        return kNaN;
    }

    void issue_exits() {
        const std::string from_entry = scenario_.all_entries ? "" : "E";
        if (scenario_.first_limit != FirstLimit::Omitted) {
            strategy_exit("TP1", from_entry, first_limit(),
                          scenario_.with_stop ? kShortStop : kNaN,
                          kNaN, kNaN, kNaN, scenario_.first_percent,
                          "tp1", kNaN, "");
        }
        strategy_exit("TP2", from_entry,
                      scenario_.is_long ? kLongTarget : kShortTarget, kNaN,
                      kNaN, kNaN, kNaN, kSecondPercent, "tp2", kNaN, "");
    }

    Scenario scenario_;
};

struct Result {
    std::vector<Trade> trades;
    double position = 0.0;
};

Result run(const Scenario& scenario) {
    std::vector<Bar> bars;
    // Leave a flat bar after the sole touch to observe the remaining position.
    for (int index = kEntrySignalBar; index <= scenario.hit_bar + 1; ++index) {
        Bar bar{};
        bar.open = bar.high = bar.low = bar.close = kEntryPrice;
        bar.volume = 1000.0;
        bar.timestamp = static_cast<std::int64_t>(index + 1) * 60000;
        if (index == scenario.hit_bar) {
            if (scenario.is_long) bar.high = kLongTarget;
            else bar.low = kShortTarget;
        }
        bars.push_back(bar);
    }
    ExitProbe probe(scenario);
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    Result result;
    for (int index = 0; index < probe.trade_count(); ++index)
        result.trades.push_back(probe.get_trade(index));
    result.position = probe.position();
    return result;
}

void dump(const char* name, const Result& result) {
    std::printf("    %s: trades=%zu position=%.17g\n",
                name, result.trades.size(), result.position);
    for (std::size_t index = 0; index < result.trades.size(); ++index) {
        const auto& trade = result.trades[index];
        std::printf("      [%zu] %s -> %s qty=%.17g entry=%.17g@%d "
                    "exit=%.17g@%d time=%lld->%lld pnl=%.17g long=%d\n",
                    index, trade.entry_id.c_str(), trade.exit_id.c_str(),
                    trade.qty, trade.entry_price, trade.entry_bar_index,
                    trade.exit_price, trade.exit_bar_index,
                    static_cast<long long>(trade.entry_time),
                    static_cast<long long>(trade.exit_time), trade.pnl,
                    static_cast<int>(trade.is_long));
    }
}

int failures = 0;

struct Checks {
    const std::string label;
    const Result& actual;
    const Result* control = nullptr;
    bool dumped = false;

    void check(bool passed, int line, const char* expression) {
        if (passed) return;
        ++failures;
        std::printf("FAIL [%s] line %d: %s\n", label.c_str(), line, expression);
        if (!dumped) {
            dump("actual", actual);
            if (control) dump("control", *control);
            dumped = true;
        }
    }
};

#define CHECK(expr) checks.check((expr), __LINE__, #expr)

bool near(double left, double right) {
    return std::abs(left - right) <= 1e-9;
}

void check_partial(const std::string& label, const Scenario& scenario,
                   const Result& result, double reserved_units) {
    Checks checks{label, result};
    const double requested = kEntryUnits * kSecondPercent / kFullPercent;
    const double closed = std::min(requested, kEntryUnits - reserved_units);
    const double remaining = kEntryUnits - closed;
    CHECK(result.trades.size() == 1);
    if (result.trades.size() == 1) {
        const auto& trade = result.trades.front();
        CHECK(trade.entry_id == "E");
        CHECK(trade.exit_id == "TP2");
        CHECK(trade.is_long == scenario.is_long);
        CHECK(near(trade.qty, closed));
        CHECK(near(trade.entry_price, kEntryPrice));
        CHECK(near(trade.exit_price, scenario.is_long ? kLongTarget : kShortTarget));
        CHECK(trade.entry_bar_index == kEntryFillBar);
        CHECK(trade.exit_bar_index == scenario.hit_bar);
    }
    CHECK(near(result.position, scenario.is_long ? remaining : -remaining));
}

void check_equal(const std::string& label, const Result& actual,
                 const Result& control) {
    Checks checks{label, actual, &control};
    CHECK(actual.trades.size() == control.trades.size());
    for (std::size_t index = 0;
         index < std::min(actual.trades.size(), control.trades.size()); ++index) {
        const auto& a = actual.trades[index];
        const auto& c = control.trades[index];
        // Exact equality across every Trade field, including provenance,
        // excursions, comments and close cause as well as the fill facts.
        CHECK(a.entry_time == c.entry_time);
        CHECK(a.exit_time == c.exit_time);
        CHECK(a.entry_price == c.entry_price);
        CHECK(a.exit_price == c.exit_price);
        CHECK(a.qty == c.qty);
        CHECK(a.pnl == c.pnl);
        CHECK(a.pnl_pct == c.pnl_pct);
        CHECK(a.is_long == c.is_long);
        CHECK(a.entry_bar_index == c.entry_bar_index);
        CHECK(a.exit_bar_index == c.exit_bar_index);
        CHECK(a.entry_id == c.entry_id);
        CHECK(a.entry_comment == c.entry_comment);
        CHECK(a.exit_comment == c.exit_comment);
        CHECK(a.exit_id == c.exit_id);
        CHECK(a.exit_from_bracket == c.exit_from_bracket);
        CHECK(a.max_runup == c.max_runup);
        CHECK(a.max_drawdown == c.max_drawdown);
        CHECK(a.commission == c.commission);
        CHECK(a.entry_incarnation == c.entry_incarnation);
        CHECK(a.open_at_end == c.open_at_end);
        CHECK(a.close_cause == c.close_cause);
    }
    CHECK(actual.position == control.position);
}

void check_against_control(const std::string& label, const Scenario& scenario,
                           bool check_absolute = false) {
    const Result actual = run(scenario);
    Scenario control_scenario = scenario;
    control_scenario.first_limit = FirstLimit::Control;
    const Result control = run(control_scenario);
    check_equal(label, actual, control);
    if (check_absolute) {
        const double reserved = kEntryUnits * scenario.first_percent / kFullPercent;
        check_partial(label + "/quantity", scenario, actual, reserved);
        check_partial(label + "/control-quantity", control_scenario, control, reserved);
    }
}
} // namespace

int main() {
    const double first_reserved = kEntryUnits * kFirstPercent / kFullPercent;
    for (const int hit_bar : {2, 3, 5}) {
        Scenario scenario;
        scenario.hit_bar = hit_bar;
        const std::string suffix = "/H" + std::to_string(hit_bar);
        scenario.first_limit = FirstLimit::Control;
        check_partial("2/control" + suffix, scenario, run(scenario), first_reserved);
        for (const auto limit : {FirstLimit::NegativeConstant, FirstLimit::NegativeAverage}) {
            scenario.first_limit = limit;
            const std::string label = std::string("1/short/")
                + (limit == FirstLimit::NegativeConstant ? "constant" : "average") + suffix;
            const Result actual = run(scenario);
            check_partial(label, scenario, actual, first_reserved);
            Scenario control = scenario;
            control.first_limit = FirstLimit::Control;
            check_equal("3/equality/" + label, actual, run(control));
        }
    }

    Scenario scenario;
    scenario.first_percent = kFullPercent;
    check_against_control("4/full-percent", scenario);

    scenario = Scenario{};
    scenario.with_stop = true;
    // GREEN at the base too: the stop alone already reserves TP1's share.
    check_against_control("5/limit-and-stop", scenario, true);

    scenario = Scenario{};
    scenario.same_bar = true;
    check_against_control("6/entry-bar-and-reissued", scenario);

    scenario = Scenario{};
    scenario.all_entries = true;
    check_against_control("7/empty-from-entry", scenario);

    scenario = Scenario{};
    scenario.is_long = true;
    scenario.first_limit = FirstLimit::Control;
    check_partial("8/long-mirror", scenario, run(scenario), first_reserved);

    const struct {
        FirstLimit limit;
        const char* label;
    } absent_levels[] = {
        {FirstLimit::NoLevel, "9/NaN"},
    };
    for (const auto& row : absent_levels) {
        scenario = Scenario{};
        scenario.first_limit = FirstLimit::Omitted;
        const Result omitted = run(scenario);
        scenario.first_limit = row.limit;
        const Result actual = run(scenario);
        check_equal(row.label, actual, omitted);
        check_partial(std::string(row.label) + "/unreserved", scenario, actual, 0.0);
    }

    scenario = Scenario{};
    scenario.first_limit = FirstLimit::Zero;
    check_against_control("10/zero-limit", scenario);

    std::printf("exit-negative-limit-reserve: %d failed checks\n", failures);
    return failures == 0 ? 0 : 1;
}
