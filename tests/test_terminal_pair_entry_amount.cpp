#include "entry_amount_fixture.hpp"
#include <pineforge/compat/pine/market_admission.hpp>
#include <variant>

namespace {
using namespace entry_amount;
enum class Book { BudgetFirst, BudgetSecond, ThreeEntries };

class Probe final : public Host {
public:
    Probe(Amount a, bool budget_buy, Book book)
        : Host(a, true, 0, 100), budget_buy(budget_buy), book(book) {
        if (book == Book::ThreeEntries) fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0) return;
        if (book != Book::BudgetSecond) entry("budget", budget_buy);
        if (book == Book::ThreeEntries)
            strategy_entry("five", budget_buy, missing, missing, 5);
        strategy_entry("unit", !budget_buy, missing, missing, 1);
        if (book == Book::BudgetSecond) entry("budget", budget_buy);
        for (const auto& p : adapter_.fixture_pending_snapshots()) {
            if (p.staged || !p.snapshot.market_admission.observation()) continue;
            if (p.snapshot.source_id == "five") five_id = p.incarnation;
            if (p.snapshot.source_id == "unit") fixed_id = p.incarnation;
            if (p.snapshot.source_id != "budget") continue;
            original = p.incarnation;
            qualified = pineforge::compat::pine::explicit_qualification(p.snapshot.market_admission);
        }
        before = pending_order_count();
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext& context) override {
        Host::on_native_applied_event(e, context);
        ordinary_close &= e.cursor.point.path_phase == pineforge::NativePathPhase::Close
            && e.cursor.point.provenance == pineforge::NativePriceProvenance::AfterCalculationClose;
    }
    const bool budget_buy;
    const Book book;
    int before = 0;
    bool qualified = false, ordinary_close = true;
    std::uint64_t fixed_id = 0, original = 0, five_id = 0;
};

bool check(Amount a, bool sell_first) {
    Probe p(a, !sell_first, Book::BudgetFirst);
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

bool check_mixed(Amount a, bool buy, Book book) {
    const bool three = book == Book::ThreeEntries;
    const char* label = three
        ? (buy ? "budget buy, fixed buy, fixed sell" : "budget sell, fixed sell, fixed buy")
        : (buy ? "fixed sell then budget buy" : "fixed buy then budget sell");
    Probe p(a, buy, book);
    if (!p.completed({flat(0, 80), flat(1, 80)}) || !p.original || !p.fixed_id
        || p.before != (three ? 3 : 2) || (three && !p.five_id))
        return report("NOT COMPARED", label, a);

    // Each contract costs 80 * 2. The quarter-lot floor buys 5 from 825
    // or 5.25 from 4.25% of 20,000. A budget placed second reverses the
    // first contract. Placed first, it uses the only same-side opening slot.
    const double signed_qty = (buy ? 1 : -1) * (a == Amount::Cash ? 5 : 5.25);
    const auto* budget = p.only_fill("budget");
    const auto* unit = p.only_fill("unit");
    bool ok = budget && unit && p.fills.size() == 2 && p.ordinary_close
        && budget->incarnation == p.original && unit->incarnation == p.fixed_id
        && budget->price == 80 && unit->price == 80 && budget->bar == 0 && unit->bar == 0
        && budget->opened == signed_qty;
    if (ok && three) {
        std::uint64_t rejected = 0;
        for (const auto& event : p.native_events(budget->ordinal)) {
            if (!event.command) continue;
            const auto* r = std::get_if<no::MatchRejectedEvent>(&*event.command);
            if (r && r->handle().incarnation == p.five_id
                && r->reason == no::MatchRejectReason::HostPrecommit) rejected = r->ordinal;
        }
        ok = budget->ordinal < rejected && rejected < unit->ordinal
            && budget->closed == 0 && unit->closed == 1 && unit->opened == 0
            && p.live_position_size() == signed_qty + (buy ? -1 : 1);
    } else if (ok) {
        ok = unit->ordinal < budget->ordinal && unit->opened == (buy ? -1 : 1)
            && unit->closed == 0 && budget->closed == 1 && p.live_position_size() == signed_qty;
    }
    return report(ok ? "PASS" : "FAIL", label, a);
}
} // namespace

int main() {
    bool ok = true;
    for (bool sell_first : {false, true})
        for (auto a : {Amount::Cash, Amount::Equity, Amount::DefaultEquity, Amount::Fixed})
            ok &= check(a, sell_first);
    for (auto book : {Book::BudgetSecond, Book::ThreeEntries})
        for (bool buy : {false, true})
            for (auto a : {Amount::Cash, Amount::Equity}) ok &= check_mixed(a, buy, book);
    return ok ? 0 : 1;
}
