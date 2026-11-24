#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;
using pineforge::NativePathPhase;
using pineforge::NativePriceProvenance;

class Probe final : public Host {
public:
    Probe(Amount a, bool buy) : Host(a, false, 0, 100), buy(buy) {
        auto config = fixture_configuration();
        config.initial_capital = 1000;
        config.commission_type = static_cast<int>(pineforge::CommissionType::CASH_PER_CONTRACT);
        config.commission_value = 2.5;
        configure_pine_strategy(config);
        set_syminfo_pointvalue(1);
        set_margin_call_enabled(true);
    }
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0) return;
        if (amount == Amount::Fixed) {
            // The fixed buy of nine transacts ten ahead of the pending sell.
            strategy_entry("unit", false, missing, missing, 1);
            strategy_entry("opening", true, missing, missing, 9);
        } else {
            strategy_entry("opening", buy, missing, missing, amount == Amount::Cash ? 1000 : 100,
                           "", "", 0, type(amount));
            strategy_entry("unit", !buy, missing, missing, 1);
        }
        for (const auto& row : native_working_requests()) {
            const auto& d = *row.definition;
            if (d.request.label == "unit") peer = d.handle.incarnation;
            if (d.request.label == "opening") opening = d.handle.incarnation;
        }
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext& context) override {
        Host::on_native_applied_event(e, context);
        const bool call = e.request().label == source::kMarginCallLabel;
        at_open &= e.cursor.point.path_phase == NativePathPhase::Open
            && e.cursor.point.provenance == (call ? NativePriceProvenance::CurrentExecution
                                                  : NativePriceProvenance::ModeledOHLCOpen);
    }
    const bool buy;
    bool at_open = true;
    std::uint64_t peer = 0, opening = 0;
};

bool check(Amount a, bool buy) {
    const bool fixed = a == Amount::Fixed;
    const char* label = fixed ? "fixed pair fills without a margin call"
        : buy ? "buy checks margin before fixed sell" : "sell checks margin before fixed buy";
    Probe p(a, buy);
    const bool done = p.completed({flat(0, 100), flat(1, 100), flat(2, 100)});
    const auto* first = p.only_fill("opening");
    const auto* peer = p.only_fill("unit");
    const int sign = buy ? 1 : -1;
    if (!done || !p.opening || !p.peer || !first || !peer
        || first->incarnation != p.opening || peer->incarnation != p.peer
        || first->ordinal >= peer->ordinal || first->opened != sign * 10 || first->closed != 0
        || first->bar != 1 || first->price != 100 || peer->bar != 1 || peer->price != 100
        || peer->closed != 1 || peer->opened != 0)
        return report("NOT COMPARED", label, a);

    // Ten contracts require 1000 but their fee leaves 975. Restore four
    // times the 0.25-contract shortfall. Only a fixed pair may wait for
    // its peer to reduce the requirement to 900 instead.
    const auto* call = p.only_fill(source::kMarginCallLabel);
    const bool ok = p.at_open && (fixed ? p.fills.size() == 2 && !call
        : p.fills.size() == 3 && call && first->ordinal < call->ordinal
            && call->ordinal < peer->ordinal && call->bar == 1 && call->price == 100
            && call->closed == 1 && call->opened == 0)
        && p.live_position_size() == sign * (fixed ? 9 : 8);
    return report(ok ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = check(Amount::Fixed, true);
    for (bool buy : {false, true})
        for (auto a : {Amount::Cash, Amount::Equity}) ok &= check(a, buy);
    return ok ? 0 : 1;
}
