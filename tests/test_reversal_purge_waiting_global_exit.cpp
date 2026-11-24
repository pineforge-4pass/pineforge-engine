#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace pineforge;
namespace no = native_order;
constexpr double no_price = std::numeric_limits<double>::quiet_NaN();
constexpr int bars_count = 5;

void need(bool condition, const char* observation) {
    if (!condition) throw std::runtime_error(observation);
}

class WaitingExitHost final : public source::PineStrategyHost {
public:
    explicit WaitingExitHost(bool hidden) : hidden(hidden) {
        source::PineStrategyConfig config;
        config.close_entries_rule_any = true;
        config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        switch (pine_bar_index()) {
        case 0: strategy_entry("Long", true, no_price, no_price, 2); break;
        case 1:
            need(live_position_size() == 2, "expected the original long");
            strategy_exit("Guard", "", 120, hidden ? no_price : 80);
            break;
        case 2:
            need(projected_legs() == 1, "expected one global exit on the long book");
            wait_for_parent();
            strategy_entry("Flip", false, no_price, no_price, 1);
            break;
        case 3:
            // This public command can see an unarmed leg even when the
            // working-order projection cannot. No matching label is a no-op.
            leftover = static_cast<int>(cancel_where("Guard", NativeRequestField::Label));
            need(native_working_request(park).has_value(), "the separate parent did not stay working");
            break;
        }
    }

    int compare() const {
        need(last_run_status() == 0 && callbacks == bars_count && reversals == 1,
             "expected a completed run with one reversal");
        need(leftover >= 0 && projected_after >= 0, "missing book observation");
        if (hidden) {
            const bool ok = limit_cancels == 1 && leftover == 0 && projected_after == 0;
            (ok ? std::cout : std::cerr) << (ok ? "PASS" : "FAIL")
                << " hidden global exit: expected user cancels=1 still live=0 projected=0,"
                << " got user cancels=" << limit_cancels << " still live=" << leftover
                << " projected=" << projected_after << '\n';
            return ok ? 0 : 1;
        }
        need(limit_cancels == 1 && dependent_ends == 1 && leftover == 0,
             "expected the limit cancel to end its waiting stop");
        const bool ok = redundant == 0 && projected_after == 0;
        (ok ? std::cout : std::cerr) << (ok ? "PASS" : "FAIL")
            << " global exit ended by its parent: expected NotWorking=0 projected=0, got NotWorking="
            << redundant << " projected=" << projected_after << '\n';
        return ok ? 0 : 1;
    }

private:
    int projected_legs() const {
        int found = 0;
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0, "could not copy the source book");
            if (std::string(row.id) != "Guard") continue;
            need(row.from_entry[0] == '\0' && row.limit_price == 120
                 && row.created_position_side == static_cast<int>(PositionSide::LONG),
                 "expected a global exit placed on the old side");
            ++found;
        }
        return found;
    }

    void wait_for_parent() {
        strategy_order("Park", true, 1, 50);  // every bar spans 99..101
        for (const auto& w : native_working_requests()) {
            if (w.definition->request.label == "Park"
                && std::holds_alternative<no::Limit>(w.definition->request.trigger))
                park = w.definition->handle;
            if (w.definition->request.label != "Guard") continue;
            if (std::holds_alternative<no::Limit>(w.definition->request.trigger))
                limit = w.definition->handle;
            if (std::holds_alternative<no::Stop>(w.definition->request.trigger))
                stop = w.definition->handle;
        }
        need(park.incarnation != 0 && limit.incarnation != 0
             && (hidden || (stop.incarnation != 0 && limit != stop)),
             "could not identify the separate parent and original global exit legs");
        const auto wait = [&](const no::RequestHandle& leg, const no::RequestHandle& parent,
                              no::NativeArmVisibility visibility) {
            const auto before = native_working_request(leg);
            need(before.has_value(), "leg was not working before replacement");
            auto request = before->definition->request;
            request.owner = no::WaitForApplied{parent, visibility, no::NativeArmFirstMatch::AtArmPrint,
                                               no::NativeArmScope::Book};
            no::ReplaceOptions options;
            options.keep_handle = true;
            const auto changed = replace(leg, request, options);
            need(changed.status == no::ReplaceStatus::Replaced && changed.successor == leg,
                 "replacement did not retain the exit handle");
        };
        wait(limit, park, hidden ? no::NativeArmVisibility::PendingUntilArmed
                                 : no::NativeArmVisibility::Working);
        if (!hidden) wait(stop, limit, no::NativeArmVisibility::Working);
        need(native_working_request(limit).has_value() == !hidden
             && (hidden || native_working_request(stop).has_value()),
             "unexpected visibility of the accepted waiting legs");
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext&) override {
        if (fill.request().label != "Flip") return;
        ++reversals;
        need(fill.interval_index() == 3 && fill.closed_units == 2 && fill.opened_units == -1
             && native_working_request(park).has_value(), "expected a reversal beside the resting parent");
        projected_after = projected_legs();
        std::uint64_t limit_end = 0;
        for (const auto& e : native_events(fill.ordinal)) {
            if (!e.command) continue;
            if (const auto* end = std::get_if<no::CancelledEvent>(&*e.command)) {
                const auto* wait = std::get_if<no::Wait>(&end->prior_authority);
                if (end->handle() == limit && end->reason == no::CancelReason::User
                    && wait && wait->parent == park) {
                    ++limit_cancels;
                    limit_end = end->ordinal;
                }
                if (!hidden && end->handle() == stop && end->reason == no::CancelReason::OwnerGone
                    && wait && wait->parent == limit && end->cause && limit_end != 0
                    && end->cause->ordinal == limit_end) ++dependent_ends;
            } else if (const auto* miss = std::get_if<no::NotWorkingEvent>(&*e.command)) {
                if (miss->target == stop) ++redundant;
            }
        }
    }

    const bool hidden;
    no::RequestHandle park{}, limit{}, stop{};
    int callbacks = 0, reversals = 0, limit_cancels = 0, dependent_ends = 0, redundant = 0;
    int leftover = -1, projected_after = -1;
};
}  // namespace

int main() {
    std::array<Bar, bars_count> bars{};
    for (int i = 0; i < bars_count; ++i)
        bars[i] = {100, 101, 99, 100, 1000, 1704067200000LL + i * 60000LL};
    int status = 0;
    for (bool hidden : {true, false}) {
        try {
            WaitingExitHost host(hidden);
            host.run(bars.data(), bars_count);
            if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
            if (host.compare() != 0 && status == 0) status = 1;
        } catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << (hidden ? "hidden global exit" : "global exit ended by its parent")
                      << ": " << e.what() << '\n';
            status = 2;
        }
    }
    return status;
}
