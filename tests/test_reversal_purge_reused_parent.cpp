#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <variant>

namespace {
using namespace pineforge;
constexpr double na_price = std::numeric_limits<double>::quiet_NaN();
constexpr int bar_count = 6;

// A missing observation is a setup failure, never evidence about the purge.
template<class Actual, class Expected>
bool observed(const char* label, const Actual& actual, const Expected& expected) {
    if (actual == expected) return true;
    std::cerr << "NOT COMPARED " << label << ": expected " << expected
              << ", got " << actual << '\n';
    return false;
}

class ReusedParentHost final : public source::PineStrategyHost {
public:
    ReusedParentHost() {
        source::PineStrategyConfig config;
        config.process_orders_on_close = true;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        last_position = live_position_size();
        switch (pine_bar_index()) {
        case 0: strategy_entry("P", true, na_price, na_price, 2.0); break;
        case 1: strategy_close_all(); break;
        case 2: strategy_entry("A", true, na_price, na_price, 1.0); break;
        case 3:
            // All bars span 99..101: neither the short limit nor its exits can fill.
            strategy_entry("P", false, 110.0, na_price, 1.0);
            strategy_exit("PB", "P", 90.0, 120.0);
            break;
        case 4:
            observe_before_reversal();
            strategy_entry("X", false, na_price, na_price, 1.0);
            break;
        }
    }

    int compare() const {
        if (!observed("run error", last_error(), std::string{}) ||
            !observed("run status", last_run_status(), 0) ||
            !observed("bar callbacks", callbacks, bar_count) ||
            !observed("closed lots", closed_trade_count(), 2u)) return 2;
        const auto old = get_trade(0);
        const auto reversed = get_trade(1);
        if (!observed("first closed entry", old.entry_id, "P") ||
            !observed("historical P was long", old.is_long, true) ||
            !observed("historical P units", old.qty, 2.0) ||
            !observed("historical P entry bar", old.entry_bar_index, 0) ||
            !observed("historical P exit bar", old.exit_bar_index, 1) ||
            !observed("historical P has an incarnation", old.entry_incarnation != 0, true) ||
            !observed("pending projection copy errors", copy_errors, 0) ||
            !observed("working short P limits at 110", short_parents, 1) ||
            !observed("working P reuses the historical handle",
                      parent.incarnation == old.entry_incarnation, false) ||
            !observed("working PB limit legs before X", limits, 1) ||
            !observed("working PB stop legs before X", stops, 1) ||
            !observed("PB has distinct nonzero handles", limit.incarnation != 0 &&
                      stop.incarnation != 0 && limit != stop, true) ||
            !observed("projected PB brackets from P at 90/120", brackets_from_p, 1) ||
            !observed("lot closed by the reversal", reversed.entry_id, "A") ||
            !observed("side of A was long", reversed.is_long, true) ||
            !observed("A entry bar", reversed.entry_bar_index, 2) ||
            !observed("A exit bar", reversed.exit_bar_index, 4) ||
            !observed("reversing entry", reversed.exit_id, "X") ||
            !observed("X fills", x_fills, 1) ||
            !observed("units X closed", x_closed, 1.0) ||
            !observed("signed units X opened", x_opened, -1.0) ||
            !observed("last position", last_position, -1.0) ||
            !observed("same P parent working after X", parent_survived, true)) return 2;

        if (surviving_legs != 2) {
            std::cerr << "FAIL original PB legs working after X: expected 2, got "
                      << surviving_legs << '\n';
            return 1;
        }
        std::cout << "PASS original PB legs working after X: expected 2, got 2\n";
        return 0;
    }

private:
    void observe_before_reversal() {
        for (const auto& row : native_working_requests()) {
            const auto& request = row.definition->request;
            if (request.label == "P" && std::holds_alternative<native_order::Limit>(request.trigger))
                parent = row.definition->handle;
            if (request.label != "PB") continue;
            if (std::holds_alternative<native_order::Limit>(request.trigger)) {
                limit = row.definition->handle;
                ++limits;
            } else if (std::holds_alternative<native_order::Stop>(request.trigger)) {
                stop = row.definition->handle;
                ++stops;
            }
        }
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(i, &row) != 0) {
                ++copy_errors;
                continue;
            }
            if (std::string(row.id) == "P" && parent.incarnation != 0 &&
                row.incarnation == parent.incarnation && !row.is_long && row.limit_price == 110.0)
                ++short_parents;
            if (std::string(row.id) == "PB" && std::string(row.from_entry) == "P" &&
                row.limit_price == 90.0 && row.stop_price == 120.0)
                ++brackets_from_p;
        }
    }

    // The host delivers this callback after the adapter processes X's fill.
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.request().label != "X") return;
        ++x_fills;
        x_closed = event.closed_units;
        x_opened = event.opened_units;
        parent_survived = parent.incarnation != 0 && native_working_request(parent).has_value();
        surviving_legs = (limit.incarnation != 0 && native_working_request(limit).has_value())
                       + (stop.incarnation != 0 && native_working_request(stop).has_value());
    }

    native_order::RequestHandle parent{}, limit{}, stop{};
    int callbacks = 0, short_parents = 0, limits = 0, stops = 0, brackets_from_p = 0, copy_errors = 0;
    int x_fills = 0, surviving_legs = -1;
    double x_closed = na_price, x_opened = na_price, last_position = na_price;
    bool parent_survived = false;
};
} // namespace

int main() {
    try {
        std::array<Bar, bar_count> bars{};
        for (int i = 0; i < bar_count; ++i)
            bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0, 1704067200000LL + i * 60000LL};
        ReusedParentHost host;
        host.run(bars.data(), bar_count);
        return host.compare();
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED run: expected completed run, got exception: " << error.what() << '\n';
        return 2;
    }
}
