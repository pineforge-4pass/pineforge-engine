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
    const auto* next = p.only_fill("opposite");
    const bool measured = done && p.held == 1 && p.original && seed && seed->bar == 0
        && seed->opened == 1 && close && close->bar == 1
        && close->closed == 1 && close->opened == 0;
    // At 80, with point value 2, 825 cash buys 5 contracts and 4.25% of
    // 20,000 buys 5.25. The close has already consumed the one held unit.
    const double contracts = a == Amount::Cash ? 5 : 5.25;
    const std::array<const Fill*, 3> observed{seed, close, next};
    const auto matches = [&](const std::array<const Fill*, 3>& rows) {
        const auto& successor = *rows[2];
        // The seed spends one slot. The close spends the second; the
        // successor inherits that slot even though it has its own fill.
        return rows[0]->at_close(ClosePoint::AfterCalculation) && rows[0]->charged_slots == 1
            && rows[1]->at_close(ClosePoint::Calculation) && rows[1]->charged_slots == 2
            && successor.at_close(ClosePoint::Calculation) && successor.charged_slots == 2
            && rows[0]->ordinal < rows[1]->ordinal && rows[1]->price == 80
            && p.fills.size() == 3 && successor.incarnation != p.original
            && successor.ordinal > rows[1]->ordinal && successor.bar == rows[1]->bar
            && successor.price == 80 && successor.opened == -contracts && successor.closed == 0
            && p.live_position_size() == -contracts;
    };
    const bool matched = measured && next && matches(observed);
    bool ok = report(!measured ? "NOT COMPARED" : matched ? "PASS" : "FAIL", label, a);
    ok &= report(!matched ? "NOT COMPARED" : (rejects_wrong_close(observed, matches) ? "PASS" : "FAIL"),
                 "counted close rejects changed fill coordinates", a);
    const bool rejects_charges = matched
        && rejects_each_change(observed, matches, [](Fill& f) { --f.charged_slots; })
        && rejects_each_change(observed, matches, [](Fill& f) { ++f.charged_slots; });
    ok &= report(!matched ? "NOT COMPARED" : (rejects_charges ? "PASS" : "FAIL"),
                 "counted close rejects a missing or extra charge", a);
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity}) ok &= check(a);
    return ok ? 0 : 1;
}
