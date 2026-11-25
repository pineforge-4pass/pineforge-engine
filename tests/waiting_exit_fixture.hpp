#pragma once

#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace waiting_exit {
using namespace pineforge;
namespace no = native_order;
using Handle = no::RequestHandle;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();

inline void need(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<std::size_t N>
std::array<Bar, N> bars() {
    std::array<Bar, N> tape{};
    for (std::size_t i = 0; i < N; ++i)
        tape[i] = {100, 101, 99, 100, 1000, 1704067200000LL + static_cast<std::int64_t>(i) * 60000};
    return tape;
}

class Host : public source::PineStrategyHost {
public:
    Host(bool any, int pyramiding = 1, bool on_close = false) {
        source::PineStrategyConfig config;
        config.close_entries_rule_any = any;
        config.pyramiding = pyramiding;
        config.process_orders_on_close = on_close;
        config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1);
        fixture_retain_all_events();
    }

    const no::WorkingRequestCore& book() const {
        return NativeExecutionConsumer::bound(*this).request_core();
    }

    template<class Trigger = no::Limit>
    Handle working(const char* label) const {
        Handle result{};
        for (const auto& w : native_working_requests()) {
            if (w.definition->request.label != label
                || !std::holds_alternative<Trigger>(w.definition->request.trigger)) continue;
            need(result.incarnation == 0, "ambiguous working order");
            result = w.definition->handle;
        }
        need(result.incarnation != 0, "missing original working order");
        return result;
    }

    // The projection can combine two legs in one row. Count by source id
    // as well as capturing its first row; the kernel counts individual legs.
    int projected(const char* label, pf_pending_order_v1_t* first = nullptr) const {
        int count = 0;
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0, "unreadable pending projection");
            if (std::string(row.id) != label) continue;
            if (++count == 1 && first) *first = row;
        }
        return count;
    }

    int live_count(const char* label) const {
        int count = 0;
        for (const auto& row : book().live())
            if (row.request().label == label) ++count;
        return count;
    }

    void wait_for(Handle leg, Handle parent, bool visible = false,
                  std::optional<no::OrderIntent> intent = std::nullopt) {
        const auto before = native_working_request(leg);
        need(before && leg != parent && book().find_live(parent), "missing leg or separate parent");
        auto request = before->definition->request;
        if (intent) request.intent = *intent;
        const auto scope = std::holds_alternative<no::Transact>(request.intent)
            ? no::NativeArmScope::OwnerLot : no::NativeArmScope::Book;
        request.owner = no::WaitForApplied{parent, visible ? no::NativeArmVisibility::Working
                                                          : no::NativeArmVisibility::PendingUntilArmed,
                                           no::NativeArmFirstMatch::AtArmPrint, scope};
        no::ReplaceOptions options;
        options.keep_handle = true;
        const auto changed = replace(leg, request, options);
        need(changed.status == no::ReplaceStatus::Replaced && changed.successor == leg,
             "waiting replacement did not retain the original handle");
        const auto* live = book().find_live(leg);
        const auto* wait = live ? std::get_if<no::Wait>(&live->authority) : nullptr;
        need(wait && wait->parent == parent, "expected a live unarmed leg");
        const auto* reduce = std::get_if<no::Reduce>(&request.intent);
        if (const auto* size = reduce ? std::get_if<no::ExplicitUnits>(&reduce->size) : nullptr) {
            const auto* units = std::get_if<no::RemainingUnits>(&live->remaining);
            need(units && units->q == size->units, "missing fixed sibling reservation");
        }
        need(native_working_request(leg).has_value() == visible, "wrong reader visibility");
    }
};

struct Checks {
    const char* name;
    bool ok = true;

    template<class A, class B>
    void equal(const char* what, A actual, B expected) {
        if (actual == expected) return;
        std::cerr << "FAIL " << name << ": " << what << " expected " << expected
                  << ", got " << actual << '\n';
        ok = false;
    }
};

template<class Run>
int compare(const char* name, Run run) {
    try {
        Checks checks{name};
        run(checks);
        if (checks.ok) std::cout << "PASS " << name << '\n';
        return checks.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED " << name << ": " << e.what() << '\n';
        return 2;
    }
}
} // namespace waiting_exit
