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
        const int index = pine_bar_index();
        saw_last = index == bar_count - 1;
        switch (index) {
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
            saw_before = true;
            strategy_entry("X", false, na_price, na_price, 1.0);
            break;
        }
    }

    static int compare(const ReusedParentHost* host) {
        bool setup_ok = true;
        const auto observed = [&](const char* missing, const char* label,
                                  const auto& expected, auto read) {
            if (missing) {
                std::cerr << "NOT COMPARED " << label << ": " << missing << '\n';
            } else {
                try {
                    const auto actual = read();
                    const bool match = actual == expected;
                    (match ? std::cout : std::cerr) << (match ? "PASS " : "NOT COMPARED ")
                        << label << ": expected " << expected << ", got " << actual << '\n';
                    if (match) return true;
                } catch (const std::exception& error) {
                    std::cerr << "NOT COMPARED " << label << ": " << error.what() << '\n';
                }
            }
            setup_ok = false;
            return false;
        };

        const char* missing_run = host ? nullptr : "run did not return";
        const bool clean = observed(missing_run, "run error", "\"\"",
                                    [&] { return '"' + host->last_error() + '"'; });
        const bool success = observed(missing_run, "run status", 0,
                                      [&] { return host->last_run_status(); });
        const char* run_error = clean && success ? nullptr : "run did not complete successfully";
        observed(run_error, "bar callbacks", bar_count, [&] { return host->callbacks; });
        const auto lots = run_error ? 0u : host->closed_trade_count();
        observed(run_error, "closed lots", 2u, [&] { return lots; });

        Trade old{}, reversed{};
        const bool first_p = observed(lots > 0 ? nullptr : "first closed lot unavailable",
                                      "first closed entry", "P", [&] {
            old = host->get_trade(0);
            return old.entry_id;
        });
        const char* p = first_p ? nullptr : "first closed entry P unavailable";
        observed(p, "historical P was long", true, [&] { return old.is_long; });
        observed(p, "historical P units", 2.0, [&] { return old.qty; });
        observed(p, "historical P entry bar", 0, [&] { return old.entry_bar_index; });
        observed(p, "historical P exit bar", 1, [&] { return old.exit_bar_index; });
        const bool old_handle = observed(p, "historical P has an incarnation", true,
                                         [&] { return old.entry_incarnation != 0; });

        const char* before = !run_error && host->saw_before ? nullptr : "pre-reversal snapshot unavailable";
        const char* copies = before ? before : (host->copies_attempted ? nullptr : "empty pending projection");
        const bool copied = observed(copies, "pending projection copy errors", 0,
                                     [&] { return host->copy_errors; });
        const char* projection = copied ? nullptr : "pending projection unavailable";
        const bool parent_seen = observed(projection, "working short P limits at 110", 1,
                                          [&] { return host->short_parents; });
        observed(parent_seen && old_handle ? nullptr : "both P handles required",
                 "working P reuses the historical handle", false,
                 [&] { return host->parent.incarnation == old.entry_incarnation; });
        const bool limit_seen = observed(before, "working PB limit legs before X", 1,
                                         [&] { return host->limits; });
        const bool stop_seen = observed(before, "working PB stop legs before X", 1,
                                        [&] { return host->stops; });
        observed(limit_seen && stop_seen ? nullptr : "both PB legs required",
                 "PB has distinct nonzero handles", true, [&] {
            return host->limit.incarnation != 0 && host->stop.incarnation != 0 && host->limit != host->stop;
        });
        observed(projection, "projected PB brackets from P at 90/120", 1,
                 [&] { return host->brackets_from_p; });

        const bool second_a = observed(lots > 1 ? nullptr : "second closed lot unavailable",
                                       "lot closed by the reversal", "A", [&] {
            reversed = host->get_trade(1);
            return reversed.entry_id;
        });
        const char* a = second_a ? nullptr : "second closed entry A unavailable";
        observed(a, "side of A was long", true, [&] { return reversed.is_long; });
        observed(a, "A entry bar", 2, [&] { return reversed.entry_bar_index; });
        observed(a, "A exit bar", 4, [&] { return reversed.exit_bar_index; });
        observed(a, "reversing entry", "X", [&] { return reversed.exit_id; });
        const bool filled = observed(run_error, "X fills", 1, [&] { return host->x_fills; });
        const char* x = filled ? nullptr : "one X fill required";
        observed(x, "units X closed", 1.0, [&] { return host->x_closed; });
        observed(x, "signed units X opened", -1.0, [&] { return host->x_opened; });
        observed(!run_error && host->saw_last ? nullptr : "final bar callback unavailable",
                 "last position", -1.0, [&] { return host->last_position; });
        observed(filled && parent_seen ? nullptr : "working P and one X fill required",
                 "same P parent working after X", true, [&] { return host->parent_survived; });

        if (!setup_ok) {
            std::cerr << "NOT COMPARED original PB legs working after X: fixture prerequisites failed\n";
            return 2;
        }
        const bool survived = host->surviving_legs == 2;
        (survived ? std::cout : std::cerr) << (survived ? "PASS" : "FAIL")
            << " original PB legs working after X: expected 2, got " << host->surviving_legs << '\n';
        return survived ? 0 : 1;
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
            ++copies_attempted;
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
    int callbacks = 0, short_parents = 0, limits = 0, stops = 0, brackets_from_p = 0;
    int copies_attempted = 0, copy_errors = 0;
    int x_fills = 0, surviving_legs = -1;
    double x_closed = na_price, x_opened = na_price, last_position = na_price;
    bool saw_before = false, saw_last = false, parent_survived = false;
};
} // namespace

int main() {
    try {
        std::array<Bar, bar_count> bars{};
        for (int i = 0; i < bar_count; ++i)
            bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0, 1704067200000LL + i * 60000LL};
        ReusedParentHost host;
        host.run(bars.data(), bar_count);
        return ReusedParentHost::compare(&host);
    } catch (const std::exception& error) {
        std::cerr << "NOT COMPARED run: expected completed run, got exception: " << error.what() << '\n';
        return ReusedParentHost::compare(nullptr);
    }
}
