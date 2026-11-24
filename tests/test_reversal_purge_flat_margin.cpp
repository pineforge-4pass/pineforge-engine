#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace pineforge;
namespace no = native_order;
constexpr double no_price = std::numeric_limits<double>::quiet_NaN();
constexpr int bars_count = 6;

void need(bool condition, const char* observation) {
    if (!condition) throw std::runtime_error(observation);
}

class MarginHost final : public source::PineStrategyHost {
public:
    MarginHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 10000;
        config.slippage = 1;
        config.margin_long = config.margin_short = 100;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1.0);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        switch (pine_bar_index()) {
        case 0: strategy_entry("Seed", false, no_price, no_price, 1); break;
        case 1:
            need(live_position_size() == -1, "expected the seed short before its close");
            strategy_close("Seed");
            strategy_entry("Flip", true, no_price, 103, 2);
            break;
        case 2: {
            // The named close releases the stop on a flat book. A queued
            // projection also says FLAT, so require a live native handle.
            need(live_position_size() == 0, "expected flat after the seed close");
            const auto& view = pending_intent_view();
            for (int i = 0; i < view.size(); ++i) {
                pf_pending_order_v1_t row{};
                need(view.copy_v1(i, &row) == 0, "could not copy the pending stop");
                if (std::string(row.id) != "Flip") continue;
                need(row.created_position_side == static_cast<int>(PositionSide::FLAT)
                     && row.is_long && row.stop_price == 103 && row.incarnation != 0,
                     "expected the accepted buy stop at 103, placed flat");
                for (const auto& w : native_working_requests())
                    if (w.definition->handle.incarnation == row.incarnation
                        && w.definition->request.label == "Flip"
                        && std::holds_alternative<no::Stop>(w.definition->request.trigger))
                        flip = w.definition->handle;
            }
            need(flip.incarnation != 0, "buy stop was not working while flat");
            strategy_entry("Short", false, no_price, no_price, 80);
            break;
        }
        case 3:
            need(live_position_size() == -80 && native_working_request(flip).has_value(),
                 "expected the short and the original buy stop before the gap");
            break;
        }
    }

    int compare() const {
        need(last_error().empty() && last_run_status() == 0 && callbacks == bars_count,
             "run did not finish all six bars");
        need(reversals == 1, "expected one fill of the stop accepted flat");
        const bool ok = working == 0 && projected == 0 && cancelled == 1;
        (ok ? std::cout : std::cerr) << (ok ? "PASS" : "FAIL")
            << " old short margin order after reversal: expected working=0 projected=0 cancels=1,"
            << " got working=" << working << " projected=" << projected << " cancels=" << cancelled << '\n';
        return ok ? 0 : 1;
    }

private:
    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext&) override {
        if (fill.request().label == "Short") {
            need(fill.interval_index() == 3 && fill.opened_units == -80
                 && fill.resolved_price == 99.75, "expected 80 short at the slipped open");
            short_fill = fill.ordinal;
        }
        if (fill.handle() != flip) return;
        ++reversals;
        need(short_fill != 0 && fill.interval_index() == 4
             && fill.cursor.point.path_phase == NativePathPhase::Open
             && fill.closed_units == 80 && fill.opened_units == 2
             && physical_position().signed_units == 2, "stop did not reverse the short at the gap");

        // The seed lost 0.50. At the high the short requires 80*125;
        // equity is 9999.50 + 80*(99.75-125). The call buys 4*floor(2020.50/125).
        no::RequestHandle margin{};
        for (const auto& e : native_events(short_fill)) {
            if (!e.command || e.ordinal >= fill.ordinal) continue;
            const auto* placed = std::get_if<no::AcceptedEvent>(&*e.command);
            if (!placed || placed->request().label != source::kMarginCallLabel) continue;
            const auto* stop = std::get_if<no::Stop>(&placed->request().trigger);
            const auto* reduce = std::get_if<no::Reduce>(&placed->request().intent);
            const auto* size = reduce ? std::get_if<no::ExplicitUnits>(&reduce->size) : nullptr;
            need(margin.incarnation == 0 && placed->definition->origin == no::RequestOrigin::Host
                 && stop && stop->price == 125 && size && size->units == 64,
                 "expected one source margin buy of 64 at the high");
            margin = placed->handle();
        }
        need(margin.incarnation != 0, "no old-side margin order was accepted before the reversal");
        working = native_working_request(margin).has_value();
        for (const auto& e : native_events(fill.ordinal)) {
            if (!e.command) continue;
            const auto* end = std::get_if<no::CancelledEvent>(&*e.command);
            if (end && end->handle() == margin && end->reason == no::CancelReason::User) ++cancelled;
        }
        need(working || cancelled == 1, "margin order did not reach the reversal purge live");
        projected = 0;
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0, "could not copy the book after the reversal");
            if (row.incarnation == margin.incarnation) ++projected;
        }
    }

    no::RequestHandle flip{};
    std::uint64_t short_fill = 0;
    int callbacks = 0, reversals = 0, working = -1, projected = -1, cancelled = 0;
};
}  // namespace

int main() {
    try {
        std::array<Bar, bars_count> bars{};
        for (int i = 0; i < bars_count; ++i)
            bars[i] = {100, 100, 100, 100, 1000, 1704067200000LL + i * 60000LL};
        bars[4] = {104, 125, 102, 104, 1000, bars[4].timestamp};
        MarginHost host;
        host.run(bars.data(), bars_count);
        if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
        return host.compare();
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED old short margin order after reversal: " << e.what() << '\n';
        return 2;
    }
}
