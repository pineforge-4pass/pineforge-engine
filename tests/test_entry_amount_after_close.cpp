#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

class Probe final : public Host {
public:
    Probe(Amount a, bool all) : Host(a), all(all) {}
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar == 0) strategy_entry("seed", true, missing, missing, 1);
        if (last_bar != 1) return;
        entry("next", false);
        for (const auto& p : adapter_.fixture_pending_snapshots())
            if (!p.staged && p.snapshot.source_id == "next") original = p.incarnation;
        if (all) strategy_close_all();
        else strategy_close("");
        for (const auto& p : adapter_.fixture_pending_snapshots()) {
            if (p.staged && p.snapshot.source_id == "next")
                parent = p.snapshot.paired_reversal_parent.incarnation;
        }
    }
    const bool all;
    std::uint64_t original = 0, parent = 0;
};

bool check(Amount a, bool all) {
    Probe p(a, all);
    const bool done = p.completed({flat(0, 80), bar(1, 80, 120, 80, 120),
                                   flat(2, 80), flat(3, 80)});
    const char* label = all ? "close_all" : "empty close";
    const auto* seed = p.only_fill("seed");
    const auto* close = p.only_fill(all ? "__close__" : "__pine_close_all");
    if (!done || !p.original || !p.parent || !seed || !close || seed->bar != 1
        || seed->opened != 1 || seed->price != 80 || close->incarnation != p.parent
        || close->bar != 2 || close->price != 80 || close->closed != 1 || close->opened != 0)
        return report("NOT COMPARED", label, a);
    // The close realizes no gain. Typed: 825 or 850 / (80 * 2), floored
    // to quarters. The default froze at 120, with 80 of open profit:
    // (20,080 * .0425) / (120 * 2) floors to 3.5.
    const double contracts = a == Amount::DefaultEquity ? 3.5 : a == Amount::Equity ? 5.25 : 5;
    const auto* next = p.only_fill("next");
    const bool ok = p.fills.size() == 3 && next && next->incarnation != p.original
        && next->ordinal > close->ordinal && next->bar == 2 && next->price == 80
        && next->closed == 0 && next->opened == -contracts
        && p.live_position_size() == -contracts;
    return report(ok ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = true;
    for (bool all : {false, true})
        for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity})
            ok &= check(a, all);
    return ok ? 0 : 1;
}
