#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;
enum class Route { EmptyClose, CloseAll, CountedClose };

class Probe final : public Host {
public:
    explicit Probe(Route r) : Host(Amount::Cash, r == Route::CountedClose), route(r) {
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (route == Route::CountedClose) set_pine_risk_max_intraday_filled_orders(20);
        if (last_bar == 0) strategy_entry("seed", true, missing, missing, 1);
        if (last_bar == 1) {
            held = physical_position().signed_units;
            strategy_entry("market", false, missing, missing, 825, "", "alternatives", 1,
                           type(Amount::Cash));
            no::Member market_group;
            for (const auto& w : native_working_requests()) {
                const auto& req = w.definition->request;
                if (req.label != "market" || !std::holds_alternative<no::Market>(req.trigger))
                    continue;
                if (const auto* group = std::get_if<no::Member>(&req.group)) {
                    original = w.definition->handle;
                    market_group = *group;
                }
            }
            if (route == Route::CountedClose) {
                // The counted close rebuilds a lone source candidate. A
                // native limit shares its group without adding a second
                // source candidate and sending the original to the matcher.
                if (original.incarnation) {
                    no::Request rest{no::Transact{1}, "sibling", ""};
                    rest.trigger = no::Limit{60};
                    rest.group = no::Member{market_group.group, 0, no::GroupEffect::Cancel};
                    (void)submit(rest);
                }
            } else {
                strategy_entry("sibling", true, 60, missing, 1, "", "alternatives", 1);
            }
            for (const auto& w : native_working_requests()) {
                const auto& req = w.definition->request;
                const auto* group = std::get_if<no::Member>(&req.group);
                if (req.label == "sibling" && std::holds_alternative<no::Limit>(req.trigger)
                    && group && market_group.group != 0 && group->group == market_group.group
                    && group->cohort != market_group.cohort
                    && group->effect == no::GroupEffect::Cancel
                    && market_group.effect == no::GroupEffect::Cancel)
                    sibling = w.definition->handle;
            }
            if (route == Route::CloseAll) strategy_close_all();
            else strategy_close("");
        }
        if (last_bar == 3 && sibling.incarnation)
            sibling_working = native_working_request(sibling).has_value();
    }

    const Route route;
    double held = missing;
    no::RequestHandle original{}, sibling{};
    bool sibling_working = true;
};

bool check(Route r, const char* label) {
    Probe p(r);
    const bool counted = r == Route::CountedClose;
    const bool done = p.completed({flat(0, 80), flat(1, 80), flat(2, 80), flat(3, 80)});
    const auto* seed = p.only_fill("seed");
    const auto* close = p.only_fill(r == Route::CloseAll ? "__close__" : "__pine_close_all");
    const auto* market = p.only_fill("market");
    if (!done || p.held != 1 || !p.original.incarnation || !p.sibling.incarnation
        || !seed || seed->bar != (counted ? 0 : 1) || seed->opened != 1
        || !close || close->bar != (counted ? 1 : 2) || close->closed != 1 || close->opened != 0
        || close->ordinal <= seed->ordinal || !market || market->incarnation == p.original.incarnation
        || market->bar != close->bar || market->ordinal <= close->ordinal || market->price != 80
        || market->opened != -5 || market->closed != 0)
        return report("NOT COMPARED", label, p.amount);

    bool retired_original = false;
    int sibling_cancels = 0;
    for (const auto& event : p.native_events(0)) {
        if (!event.command) continue;
        const auto* cancel = std::get_if<no::CancelledEvent>(&*event.command);
        if (!cancel) continue;
        if (cancel->handle() == p.original && cancel->ordinal < market->ordinal)
            retired_original = true;
        if (cancel->handle() == p.sibling && cancel->reason == no::CancelReason::Group
            && cancel->cause && cancel->cause->run == p.original.run
            && cancel->cause->ordinal == market->ordinal && cancel->ordinal > market->ordinal)
            ++sibling_cancels;
    }
    if (!retired_original) return report("NOT COMPARED", label, p.amount);
    const bool ok = sibling_cancels == 1 && !p.sibling_working && p.fills.size() == 3
        && p.live_position_size() == -5;
    return report(ok ? "PASS" : "FAIL", label, p.amount);
}
} // namespace

int main() {
    bool ok = check(Route::EmptyClose, "empty close cancels the resting group member");
    ok &= check(Route::CloseAll, "close_all cancels the resting group member");
    ok &= check(Route::CountedClose, "counted close cancels the resting group member");
    return ok ? 0 : 1;
}
