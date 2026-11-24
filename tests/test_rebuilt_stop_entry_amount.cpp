#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;
enum class Schedule { DeferredSell, ThrottledBuy };

class Probe final : public Host {
public:
    Probe(Amount a, Schedule schedule) : Host(a), schedule(schedule) { fixture_retain_all_events(); }
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0) return;
        if (schedule == Schedule::DeferredSell) {
            // Both stops are marketable at 80. The older sell yields to the buy.
            entry("stop", false, missing, 124);
            strategy_entry("seed", true, missing, 75, 1);
        } else {
            strategy_entry("seed", true, missing, missing, 1);
            strategy_exit("protect", "seed", missing, 96);
            entry("stop", true, missing, 120);
        }
        for (const auto& p : adapter_.fixture_pending_snapshots())
            if (!p.staged && p.snapshot.source_id == "stop") original = p.incarnation;
    }
    const Schedule schedule;
    std::uint64_t original = 0;
};

bool check(Amount a, Schedule s) {
    Probe p(a, s);
    const bool sell = s == Schedule::DeferredSell;
    const char* label = sell ? "deferred sell" : "throttled buy";
    const std::vector<Bar> bars = sell
        ? std::vector<Bar>{flat(0, 120), flat(1, 80), flat(2, 80)}
        : std::vector<Bar>{flat(0, 100), bar(1, 100, 126, 95, 110), flat(2, 140), flat(3, 140)};
    const bool done = p.completed(bars);
    bool retired = false;
    std::uint64_t successor = 0;
    for (const auto& event : p.native_events(0)) {
        if (!event.command) continue;
        if (const auto* c = std::get_if<no::CancelledEvent>(&*event.command))
            if (sell && c->handle().incarnation == p.original) retired = true;
        if (const auto* r = std::get_if<no::MatchRejectedEvent>(&*event.command))
            if (!sell && r->handle().incarnation == p.original
                && r->reason == no::MatchRejectReason::HostPrecommit) retired = true;
        if (const auto* n = std::get_if<no::AcceptedEvent>(&*event.command))
            if (retired && n->request().label == "stop" && n->handle().incarnation != p.original)
                successor = n->handle().incarnation;
    }
    const auto* seed = p.only_fill("seed");
    const auto* protect = p.only_fill("protect");
    if (!done || !p.original || !retired || !successor || !seed || seed->bar != 1
        || seed->opened != 1 || seed->price != (sell ? 80 : 100)
        || (!sell && (!protect || protect->bar != 1 || protect->price != 96 || protect->closed != 1)))
        return report("NOT COMPARED", label, a);
    // Sell: 825 or 850 at 80 (point value 2), less the one closed. The
    // marketable default froze at the signal, 120. Buy: the stopped seed
    // cost 8, so the money is 825 or 4.25% of 19,992 at the gapped 140.
    // The default froze its 850 at the stop, 120.
    const double contracts = a == Amount::DefaultEquity ? 3.5
        : sell ? (a == Amount::Cash ? 5 : 5.25) : (a == Amount::Cash ? 2.75 : 3);
    const double opened = sell ? -(contracts - 1) : contracts;
    const auto* fill = p.only_fill("stop");
    const bool ok = fill && fill->incarnation == successor && fill->ordinal > seed->ordinal
        && fill->bar == (sell ? 1 : 2) && fill->price == (sell ? 80 : 140)
        && fill->closed == (sell ? 1 : 0) && fill->opened == opened
        && p.fills.size() == (sell ? 2U : 3U) && p.live_position_size() == opened;
    return report(ok ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = true;
    for (auto s : {Schedule::DeferredSell, Schedule::ThrottledBuy})
        for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity}) ok &= check(a, s);
    return ok ? 0 : 1;
}
