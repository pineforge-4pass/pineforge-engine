#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

class Probe final : public Host {
public:
    using Host::Host;
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0) return;
        entry("limit", true, 96);
        for (const auto& p : adapter_.fixture_pending_snapshots()) {
            if (p.staged || p.snapshot.source_id != "limit") continue;
            ++placements;
            remaining = p.snapshot.projection_remaining_qty;
        }
        if (pending_order_count() == 1 && pending_intent_view().copy_v1(0, &row) == 0
            && std::string(row.id) == "limit") ++mirrors;
    }
    int placements = 0, mirrors = 0;
    double remaining = missing;
    pf_pending_order_v1_t row{};
};

bool check(Amount a) {
    Probe p(a);
    // At the gapped fill, 825 / (80 * 2 * 1) floors to 5 on the quarter
    // grid; 4.25% of 20,000 = 850, which floors to 5.25 at that fill.
    const bool complete = p.completed({flat(0, 120), flat(1, 80), flat(2, 80)});
    const auto* fill = p.only_fill("limit");
    const bool units = a == Amount::Fixed || a == Amount::UntypedUnits;
    const double contracts = a == Amount::Equity || a == Amount::DefaultEquity ? 5.25 : 5;
    const auto input_matches = [a](double q) {
        return a == Amount::DefaultEquity ? std::isnan(q) : q == operand(a);
    };
    const bool measured = complete && p.placements == 1 && p.mirrors == 1
        && p.row.market_admission_observation_command != 0
        && input_matches(p.row.qty) && p.row.qty_type == type(a)
        && input_matches(p.row.market_admission_observation_requested_quantity)
        && p.row.market_admission_observation_quantity_type == type(a)
        && fill && p.fills.size() == 1 && fill->incarnation == p.row.incarnation
        && fill->bar == 1 && fill->price == 80 && fill->opened == contracts && fill->closed == 0;
    const auto row = [&](const char* label, bool ok) {
        return report(!measured ? "NOT COMPARED" : (ok ? "PASS" : "FAIL"), label, a);
    };
    const double fixed = p.row.market_admission_observation_quantized_fixed_quantity;
    bool ok = row("admission fixed quantity", units ? fixed == 5 : std::isnan(fixed));
    ok &= row("remaining contracts before fill", units ? p.remaining == 5 : std::isnan(p.remaining));
    ok &= row("unit intent before fill", p.row.quantity_intent_kind == (units ? 2U : 1U)
              && p.row.quantity_intent_units == (units ? 5 : 0)
              && p.row.quantity_intent_numerator == 0 && p.row.quantity_intent_denominator == 0);
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity,
                   Amount::Fixed, Amount::UntypedUnits}) ok &= check(a);
    return ok ? 0 : 1;
}
