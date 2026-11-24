#include "entry_amount_fixture.hpp"
#include <pineforge/compat/pine/market_admission.hpp>

namespace {
using namespace entry_amount;

class Probe final : public Host {
public:
    Probe(Amount a, bool sell_first) : Host(a, true, 0, 100), sell_first(sell_first) {}
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0) return;
        entry("budget", !sell_first);
        strategy_entry("unit", sell_first, missing, missing, 1);
        for (const auto& p : adapter_.fixture_pending_snapshots()) {
            if (p.staged || !p.snapshot.market_admission.observation()) continue;
            if (p.snapshot.source_id == "unit") fixed_id = p.incarnation;
            if (p.snapshot.source_id != "budget") continue;
            original = p.incarnation;
            qualified = pineforge::compat::pine::explicit_qualification(p.snapshot.market_admission);
        }
        before = pending_order_count();
    }
    const bool sell_first;
    int before = 0;
    bool qualified = false;
    std::uint64_t fixed_id = 0, original = 0;
};

bool check(Amount a, bool sell_first) {
    Probe p(a, sell_first);
    const bool done = p.completed({flat(0, 80), flat(1, 80)});
    const bool measured = done && p.before == 2 && p.original && p.fixed_id;
    const char* label = sell_first ? "budget sell then fixed buy" : "budget buy then fixed sell";
    const bool units = a == Amount::Fixed || a == Amount::UntypedUnits;
    bool ok = report(!measured ? "NOT COMPARED" : (p.qualified == units ? "PASS" : "FAIL"),
                     sell_first ? "sell qualification" : "buy qualification", a);
    // In a mixed pair the original budget order opens and the other order
    // transacts one unit. In a fixed sell-then-buy pair the buy fills first
    // with its own unit plus the pending sell (six), then sells five.
    const double contracts = a == Amount::Cash || units ? 5 : 5.25;
    const auto* budget = p.only_fill("budget");
    const auto* unit = p.only_fill("unit");
    const bool fills = budget && unit && p.fills.size() == 2
        && budget->price == 80 && budget->bar == 0 && unit->price == 80 && unit->bar == 0
        && (units ? budget->incarnation != p.original : budget->incarnation == p.original)
        && (units ? unit->incarnation != p.fixed_id : unit->incarnation == p.fixed_id)
        && (units && sell_first
            ? unit->ordinal < budget->ordinal && unit->opened == 6 && unit->closed == 0
                && budget->closed == 5 && budget->opened == 0
            : budget->ordinal < unit->ordinal && budget->opened == (sell_first ? -contracts : contracts)
                && budget->closed == 0 && unit->closed == (units ? contracts : 1)
                && unit->opened == (units ? -1 : 0))
        && p.live_position_size() == (units ? (sell_first ? 1 : -1)
                                            : (sell_first ? 1 - contracts : contracts - 1));
    ok &= report(!measured ? "NOT COMPARED" : (fills ? "PASS" : "FAIL"), label, a);
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (bool sell_first : {false, true})
        for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity, Amount::Fixed})
            ok &= check(a, sell_first);
    return ok ? 0 : 1;
}
