#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

class Probe final : public Host {
public:
    explicit Probe(Amount a) : Host(a, true) {}
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        set_pine_risk_max_intraday_filled_orders(20);
        if (last_bar == 0) strategy_entry("seed", true, missing, missing, 1);
        if (last_bar != 1) return;
        held = physical_position().signed_units;
        entry("opposite", false);
        const auto rows = adapter_.fixture_pending_snapshots();
        if (rows.size() == 1 && !rows[0].staged && rows[0].snapshot.source_id == "opposite")
            original = rows[0].incarnation;
        strategy_close("");
    }
    double held = missing;
    std::uint64_t original = 0;
};

bool check(Amount a) {
    Probe p(a);
    const char* label = "opposite entry after counted close";
    const bool done = p.completed({flat(0, 80), flat(1, 80), flat(2, 80)});
    const auto* seed = p.only_fill("seed");
    const auto* close = p.only_fill("__pine_close_all");
    if (!done || p.held != 1 || !p.original || !seed || seed->bar != 0 || seed->opened != 1
        || !close || close->bar != 1 || close->closed != 1 || close->opened != 0)
        return report("NOT COMPARED", label, a);
    const auto* next = p.only_fill("opposite");
    // At 80, with point value 2, 825 cash buys 5 contracts and 4.25% of
    // 20,000 buys 5.25. The close has already consumed the one held unit.
    const double contracts = a == Amount::Cash ? 5 : 5.25;
    const bool ok = next && p.fills.size() == 3 && next->incarnation != p.original
        && next->ordinal > close->ordinal && next->bar == close->bar && next->price == 80
        && next->opened == -contracts && next->closed == 0 && p.live_position_size() == -contracts;
    return report(ok ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = true;
    for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity}) ok &= check(a);
    return ok ? 0 : 1;
}
