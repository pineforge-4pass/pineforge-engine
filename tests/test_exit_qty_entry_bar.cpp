// A stop paired with a pending market entry can take part of its opening
// fill on that bar. The next bar crosses the unfilled limit with units left.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double entry_units = 2.0;
constexpr std::int64_t start = 1'735'689'600'000;
constexpr std::int64_t hour = 3'600'000;
constexpr std::array<Bar, 3> bars{{
    {100, 100.4, 99.7, 100, 1, start},            // signal: stop 99, limit 101.2
    {100, 100.5, 98,   100, 1, start + hour},     // entry at open; only stop crossed
    {100, 102,   99.5, 101, 1, start + 2 * hour}  // only limit crossed
}};

enum class Sizing { Omitted, Percent, Units };
enum class Result { Pass, Fail, Invalid };

struct BarClose {
    int index;
    double position;
    int working_pair_legs;
};

class Probe final : public source::PineStrategyHost {
public:
    explicit Probe(Sizing sizing) : sizing_(sizing) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 100'000;
        config.default_qty_value = entry_units;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1);
        set_syminfo_type("crypto");
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        int working = 0;
        for (const auto& row : native_working_requests())
            if (row.definition->request.label == "Pair") ++working;
        closes.push_back({index, physical_position().signed_units, working});
        if (index != 0) return;

        strategy_entry("Long", true, na, na, entry_units);
        const double limit = bar.close * 1.012, stop = bar.close * 0.99;
        switch (sizing_) {
        case Sizing::Omitted: strategy_exit("Pair", "Long", limit, stop); break;
        case Sizing::Percent: strategy_exit("Pair", "Long", limit, stop, na, na, na, 50); break;
        case Sizing::Units:   strategy_exit("Pair", "Long", limit, stop, na, na, na, 100, "", 1); break;
        }
    }

    std::vector<BarClose> closes;

private:
    Sizing sizing_;
};

Result check(const char* arm, Sizing sizing, double expected_closed) {
    Probe probe(sizing);
    probe.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    const auto& closes = probe.closes;
    if (!probe.last_error().empty() || closes.size() != bars.size()
        || closes[0].index != 0 || closes[1].index != 1 || closes[2].index != 2
        || closes[0].position != 0 || closes[0].working_pair_legs != 0) {
        std::cerr << arm << " NOT COMPARED: expected three bars from flat; callbacks="
                  << closes.size() << " error=" << probe.last_error() << '\n';
        return Result::Invalid;
    }

    const auto events = probe.native_events(0);
    const no::ExecutionAppliedEvent* entry = nullptr;
    const no::ExecutionAppliedEvent* exit = nullptr;
    int fills = 0, entries = 0, limit_cancels = 0;
    for (const auto& row : events) {
        if (!row.command) continue;
        if (const auto* fill = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            ++fills;
            if (fill->request().label == "Long") { entry = fill; ++entries; }
            if (fill->request().label == "Pair") exit = fill;
            std::cout << arm << " fill " << fill->request().label << " bar=" << fill->interval_index()
                      << " price=" << fill->resolved_price << " opened=" << fill->opened_units
                      << " closed=" << fill->closed_units << '\n';
        } else if (const auto* cancel = std::get_if<no::CancelledEvent>(&*row.command)) {
            // A re-arm at the parent's open can retire an earlier limit. Count
            // only a sibling that was on the book across the actual exit fill.
            if (exit && cancel->request().label == "Pair"
                && std::holds_alternative<no::Limit>(cancel->request().trigger)
                && cancel->handle() != exit->handle()
                && cancel->definition->birth.acceptance_ordinal < exit->ordinal
                && exit->ordinal < cancel->ordinal) ++limit_cancels;
        }
    }
    if (!entry || entries != 1 || !std::holds_alternative<no::Market>(entry->request().trigger)
        || entry->interval_index() != 1 || entry->cursor.point.path_phase != NativePathPhase::Open
        || entry->resolved_price != bars[1].open || entry->opened_units != entry_units
        || entry->closed_units != 0 || entry->handle().incarnation == 0) {
        std::cerr << arm << " NOT COMPARED: expected one two-unit Long market fill at bar 1 open\n";
        return Result::Invalid;
    }

    bool ok = true;
    const auto require = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << arm << " FAIL: " << message << '\n'; ok = false; }
    };
    const double stop = bars[0].close * 0.99;
    require(fills == 2 && exit && exit->ordinal > entry->ordinal && exit->interval_index() == 1
            && std::abs(exit->resolved_price - stop) < 1e-8 && exit->opened_units == 0
            && exit->closed_units == expected_closed,
            "expected exactly one Pair stop fill on the entry bar for the requested units");
    // Closed rows exclude the range-end mark of a still-open unit.
    const auto trades = probe.closed_trade_count();
    require(trades == 1, "expected exactly one closed trade");
    if (trades == 1) {
        const auto trade = probe.closed_trade(0);
        std::cout << arm << " trade " << trade.entry_id << '@' << trade.entry_bar_index << " -> "
                  << trade.exit_id << '@' << trade.exit_bar_index << " qty=" << trade.qty << '\n';
        require(trade.is_long && trade.entry_id == "Long" && trade.exit_id == "Pair"
                && trade.entry_incarnation == entry->handle().incarnation && trade.exit_from_bracket
                && trade.entry_bar_index == 1 && trade.exit_bar_index == 1
                && trade.entry_price == bars[1].open && std::abs(trade.exit_price - stop) < 1e-8
                && trade.qty == expected_closed, "wrong closed-trade binding, bar, price or size");
    }
    const double remaining = entry_units - expected_closed;
    require(closes[1].position == remaining && closes[2].position == remaining,
            "wrong remaining position on the stop bar or after the later limit crossing");
    require(limit_cancels == 1 && closes[1].working_pair_legs == 0 && closes[2].working_pair_legs == 0,
            "the filled bracket must cancel its live limit sibling on the entry bar");
    std::cout << arm << (ok ? " PASS" : " FAIL") << ": trades=" << trades
              << " position@1,2=" << closes[1].position << ',' << closes[2].position
              << " working_pair@1,2=" << closes[1].working_pair_legs << ',' << closes[2].working_pair_legs
              << " sibling_limit_cancels=" << limit_cancels << '\n';
    return ok ? Result::Pass : Result::Fail;
}
} // namespace

int main() {
    const auto omitted = check("omitted", Sizing::Omitted, 2);
    const auto percent = check("50 percent", Sizing::Percent, 1);
    const auto units = check("qty 1", Sizing::Units, 1);
    if (omitted != Result::Pass || percent != Result::Pass || units == Result::Invalid) return 2;
    return units == Result::Pass ? 0 : 1;
}
