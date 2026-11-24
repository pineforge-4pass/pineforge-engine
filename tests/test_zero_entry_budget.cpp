#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

enum class Route { EmptyClose, CloseAll, CountedClose, DeferredSell, RearmedBuy };

class Probe final : public Host {
public:
    Probe(Amount a, Route r) : Host(a, r == Route::CountedClose), route(r) {
        fixture_retain_all_events();
    }

    void budget(bool buy, double stop = missing) {
        ++calls;
        strategy_entry("budget", buy, missing, stop, 0.0, "", "", 0, type(amount));
    }

    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (route == Route::CountedClose) set_pine_risk_max_intraday_filled_orders(20);
        if (last_bar == 0) {
            if (route == Route::DeferredSell) {
                budget(false, 124);
                strategy_entry("seed", true, missing, 75, 1);
            } else {
                strategy_entry("seed", true, missing, missing, 1);
                if (route == Route::RearmedBuy) {
                    strategy_exit("protect", "seed", missing, 96);
                    budget(true, 120);
                }
            }
        }
        if (last_bar == 1 && route != Route::DeferredSell && route != Route::RearmedBuy) {
            held = physical_position().signed_units;
            budget(false);
            if (route == Route::CloseAll) strategy_close_all();
            else strategy_close("");
        }
    }

    const Route route;
    int calls = 0;
    double held = missing;
};

bool check(Amount a, Route r, const char* label) {
    Probe p(a, r);
    const bool sell = r == Route::DeferredSell, buy = r == Route::RearmedBuy;
    const bool counted = r == Route::CountedClose;
    const bool done = p.completed(buy
        ? std::vector<Bar>{flat(0, 100), bar(1, 100, 126, 95, 110), flat(2, 140), flat(3, 140)}
        : sell ? std::vector<Bar>{flat(0, 120), flat(1, 80), flat(2, 80)}
        : std::vector<Bar>{flat(0, 80), flat(1, 80), flat(2, 80), flat(3, 80)});
    const auto* seed = p.only_fill("seed");
    const auto* close = p.only_fill(buy ? "protect" : r == Route::CloseAll
                                    ? "__close__" : "__pine_close_all");
    if (!done || p.calls != 1 || !seed || seed->bar != (counted ? 0 : 1)
        || seed->price != (buy ? 100 : 80) || seed->opened != 1 || seed->closed != 0
        || (!sell && (!close || close->bar != (counted || buy ? 1 : 2)
            || close->price != (buy ? 96 : 80) || close->closed != 1
            || close->opened != 0 || close->ordinal <= seed->ordinal))
        || (!sell && !buy && p.held != 1))
        return report("NOT COMPARED", label, a);

    int seed_orders = 0, budget_orders = 0;
    for (const auto& event : p.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*event.command)) {
            if (accepted->handle().incarnation == seed->incarnation) ++seed_orders;
            if (accepted->request().label == "budget") ++budget_orders;
        }
    }
    if (seed_orders != 1) return report("NOT COMPARED", label, a);
    // Zero cash and zero percent both buy zero contracts, even though the
    // declaration would buy one. A zero-contract entry places no order.
    const bool no_trade = p.fills.size() == (sell ? 1U : 2U)
        && p.live_position_size() == (sell ? 1 : 0);
    if (budget_orders || !no_trade)
        std::printf("budget orders=%d, fills=%zu, final contracts=%g\n",
                    budget_orders, p.fills.size(), p.live_position_size());
    return report(!budget_orders && no_trade ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = true;
    for (auto a : {Amount::Cash, Amount::Equity}) {
        ok &= check(a, Route::CountedClose, "zero budget after counted close");
        ok &= check(a, Route::EmptyClose, "zero budget after empty close");
        ok &= check(a, Route::CloseAll, "zero budget after close_all");
        ok &= check(a, Route::DeferredSell, "zero budget beside buy stop");
        ok &= check(a, Route::RearmedBuy, "zero budget after protected seed");
    }
    return ok ? 0 : 1;
}
