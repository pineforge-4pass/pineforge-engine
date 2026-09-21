// L7 witnesses for the header-only native toolkit: the bracket shapes it
// emits and the key -> handle book. The toolkit header is included first and
// alone, so this translation unit also proves it carries its own includes;
// scripts/check_native_include_independence.py proves the installed copy
// reaches no source/Pine header.
#include <pineforge/native_toolkit.hpp>

#include "native_current_fixture.hpp"

#include <optional>
#include <string>

using namespace r4_test;
namespace tk = pineforge::native_toolkit;

namespace {

no::Request resting(const char* label) {
    no::Request request{no::Transact{1.0}, label, ""};
    request.trigger = no::Limit{50.0};
    return request;
}

// The builder writes exactly one owner and one group per leg and leaves the
// caller's own intent and trigger untouched.
void bracket_emits_owner_and_group_shapes() {
    Host host;
    tk::BracketReceipt receipt;
    no::RequestHandle parent;
    host.beginning = [&](Host& base) {
        parent = put(base, tx(1.0, "entry"));
        tk::BracketSpec spec_rows;
        spec_rows.parent = parent;
        no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "tp", "bracket"};
        take_profit.trigger = no::Limit{120.0};
        no::Request stop_loss{no::Reduce{no::OwnerOpenedUnits{}}, "sl", "bracket"};
        stop_loss.trigger = no::Stop{80.0};
        spec_rows.take_profit = take_profit;
        spec_rows.stop_loss = stop_loss;
        receipt = tk::submit_bracket(base, spec_rows);
    };

    run(host, spec("l7-toolkit-bracket"), {100.0});

    CHECK(receipt.parent == parent);
    REQUIRE(receipt.take_profit && receipt.stop_loss);
    CHECK(!receipt.trail.has_value());

    const auto rows = host.native_working_requests();
    REQUIRE(rows.size() == 2);
    const std::int64_t cohorts[] = {static_cast<std::int64_t>(tk::BracketLeg::TakeProfit),
                                    static_cast<std::int64_t>(tk::BracketLeg::StopLoss)};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& request = rows[i].definition->request;
        const auto* owner = std::get_if<no::WaitForApplied>(&request.owner);
        REQUIRE(owner != nullptr);
        CHECK(owner->parent == parent);
        const auto* group = std::get_if<no::Member>(&request.group);
        REQUIRE(group != nullptr);
        CHECK(group->group == parent.incarnation);
        CHECK(group->cohort == cohorts[i]);
        CHECK(group->effect == no::GroupEffect::Cancel);
        CHECK(std::holds_alternative<no::OwnerOpenedUnits>(
                std::get<no::Reduce>(request.intent).size));
        CHECK(request.comment == "bracket");
    }
    CHECK(std::holds_alternative<no::Limit>(rows[0].definition->request.trigger));
    CHECK(std::holds_alternative<no::Stop>(rows[1].definition->request.trigger));

    // A bracket with no allocated parent is not a bracket.
    Host orphan;
    tk::BracketReceipt empty;
    orphan.beginning = [&](Host& base) {
        tk::BracketSpec spec_rows;
        spec_rows.take_profit = resting("tp");
        empty = tk::submit_bracket(base, spec_rows);
    };
    run(orphan, spec("l7-toolkit-orphan"), {100.0});
    CHECK(!empty.take_profit.has_value());
    CHECK(orphan.native_working_requests().empty());
    completed(host);
    completed(orphan);
}

// submit_or_replace re-prices the key's own request, falls back to a fresh
// submit when it is gone, and never commands the host for an unknown key.
void order_book_submit_or_replace_and_cancel() {
    Host host;
    std::optional<no::RequestHandle> first, second, revived, rejected;
    no::CancelStatus cancel_known = no::CancelStatus::InvalidHandle;
    no::CancelStatus cancel_unknown = no::CancelStatus::Cancelled;
    std::size_t events_before_unknown = 0, events_after_unknown = 0;
    std::size_t live_after_replace = 0, live_after_cancel = 0;
    int bars = 0;

    std::optional<tk::OrderBook<std::string>> book;
    host.calculation = [&](Host& base) {
        if (!book) book.emplace(base);
        switch (++bars) {
        case 1: {
            first = book->submit_or_replace("edge", resting("edge-1"));
            CHECK(book->contains("edge"));
            CHECK(book->size() == 1);
            CHECK(book->handle("edge") == first);
            // Re-pricing the same key replaces in place.
            auto amended = resting("edge-2");
            amended.trigger = no::Limit{40.0};
            second = book->submit_or_replace("edge", amended);
            live_after_replace = base.native_working_requests().size();
            // A rejected request leaves the key unbound.
            rejected = book->submit_or_replace("bad", tx(0.0, "bad"));
            break;
        }
        case 2: {
            cancel_known = book->cancel("edge");
            live_after_cancel = base.native_working_requests().size();
            CHECK(!book->contains("edge"));
            events_before_unknown = base.native_events(0).size();
            cancel_unknown = book->cancel("never");
            events_after_unknown = base.native_events(0).size();
            // The key is free again, so the next call submits.
            revived = book->submit_or_replace("edge", resting("edge-3"));
            break;
        }
        default:
            break;
        }
    };

    run(host, spec("l7-order-book"), {100.0, 100.0, 100.0});

    REQUIRE(first && second && revived);
    CHECK(*first != *second);
    CHECK(*revived != *second);
    CHECK(live_after_replace == 1);
    CHECK(!rejected.has_value());
    CHECK(cancel_known == no::CancelStatus::Cancelled);
    CHECK(live_after_cancel == 0);
    CHECK(cancel_unknown == no::CancelStatus::NotWorking);
    CHECK(events_before_unknown == events_after_unknown);

    const auto replaced = events<no::ReplacedEvent>(host);
    REQUIRE(replaced.size() == 1);
    CHECK(replaced[0].predecessor() == *first);
    CHECK(replaced[0].successor() == *second);
    // The revival is a submit, not a command against a dead handle.
    CHECK(events<no::NotWorkingEvent>(host).empty());
    CHECK(events<no::InvalidHandleEvent>(host).empty());
    const auto rejected_events = events<no::RejectedEvent>(host);
    REQUIRE(rejected_events.size() == 1);
    CHECK(rejected_events[0].reason == no::RequestRejectReason::InvalidQuantity);
    completed(host);
}

// L7b: the builder's two anchored-leg knobs, and an OrderBook key bound to a
// PendingUntilArmed child, which is out of the working enumeration before its
// arm and must still be re-priced in place rather than duplicated.
void bracket_knobs_and_pending_order_book() {
    Host host;
    tk::BracketReceipt receipt;
    no::RequestHandle parent;
    std::optional<no::RequestHandle> keyed_first, keyed_second;
    std::size_t listed_at_submit = 0;
    no::CancelStatus keyed_cancel = no::CancelStatus::InvalidHandle;
    std::optional<tk::OrderBook<std::string>> book;
    host.beginning = [&](Host& base) {
        parent = put(base, tx(1.0, "entry"));
        tk::BracketSpec spec_rows;
        spec_rows.parent = parent;
        no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "tp", "bracket"};
        take_profit.trigger = no::Limit{0.0};
        take_profit.anchor = no::FromOwnerFill{10.0, true};
        no::Request stop_loss{no::Reduce{no::OwnerOpenedUnits{}}, "sl", "bracket"};
        stop_loss.trigger = no::Stop{0.0};
        stop_loss.anchor = no::FromOwnerFill{-10.0, true, no::NativeAnchorRounding::HalfUp};
        spec_rows.take_profit = take_profit;
        spec_rows.stop_loss = stop_loss;
        spec_rows.anchor_rounding = no::NativeAnchorRounding::Directional;
        spec_rows.visibility = no::NativeArmVisibility::PendingUntilArmed;
        receipt = tk::submit_bracket(base, spec_rows);

        // An OrderBook key bound to a pending child: re-pricing replaces in
        // place (no second live leg), cancelling addresses it by handle.
        book.emplace(base);
        no::Request keyed{no::Reduce{no::OwnerOpenedUnits{}}, "keyed", "bracket"};
        keyed.trigger = no::Stop{0.0};
        keyed.anchor = no::FromOwnerFill{-20.0, true};
        keyed.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed};
        keyed_first = book->submit_or_replace("keyed", keyed);
        keyed.anchor = no::FromOwnerFill{-30.0, true};
        keyed_second = book->submit_or_replace("keyed", keyed);
        listed_at_submit = base.native_working_requests().size();
        keyed_cancel = book->cancel("keyed");
    };

    run(host, spec("l7b-toolkit-knobs"), {100.0});
    REQUIRE(receipt.take_profit && receipt.stop_loss);
    // Only the parent is enumerated before the fill: every child is pending.
    CHECK(listed_at_submit == 1);
    REQUIRE(keyed_first && keyed_second);
    CHECK(*keyed_first != *keyed_second);
    CHECK(keyed_cancel == no::CancelStatus::Cancelled);
    const auto replaced = events<no::ReplacedEvent>(host);
    REQUIRE(replaced.size() == 1);
    CHECK(replaced[0].predecessor() == *keyed_first);
    CHECK(replaced[0].successor() == *keyed_second);
    CHECK(events<no::NotWorkingEvent>(host).empty());
    // The builder wrote the knobs: both legs pending, the rounding on both
    // anchors (the stop's own HalfUp overwritten by the spec's Directional).
    for (const auto& row : events<no::AcceptedEvent>(host)) {
        const auto& request = row.request();
        const auto* owner = std::get_if<no::WaitForApplied>(&request.owner);
        if (!owner || row.handle() == *keyed_first || row.handle() == *keyed_second) continue;
        CHECK(owner->visibility == no::NativeArmVisibility::PendingUntilArmed);
        const auto* anchor = std::get_if<no::FromOwnerFill>(&request.anchor);
        REQUIRE(anchor != nullptr);
        CHECK(anchor->rounding == no::NativeAnchorRounding::Directional);
    }
    // Both legs armed at the fill and are working from then on.
    CHECK(events<no::ArmedEvent>(host).size() == 2);
    CHECK(host.native_working_requests().size() == 2);
    completed(host);
}

// E8: the receipt tells apart the three outcomes of one leg -- a leg the spec
// never asked for, a leg the kernel accepted, and a leg the kernel refused --
// and carries the kernel's own reason for the refusal. The legacy optional
// handles cannot: they are empty for all but the accepted one, which is how
// lane L14-B's leg vanished while the strategy kept running.
void bracket_receipt_separates_absent_from_refused() {
    Host host;
    tk::BracketReceipt receipt;
    no::RequestHandle parent;
    host.beginning = [&](Host& base) {
        parent = put(base, tx(1.0, "entry"));
        tk::BracketSpec spec_rows;
        spec_rows.parent = parent;
        no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "tp", "bracket"};
        take_profit.trigger = no::Limit{120.0};
        spec_rows.take_profit = take_profit;
        // A leg the kernel must refuse: a reduce of zero units is
        // RequestRejectReason::InvalidQuantity at validation, so this leg is
        // requested, reaches the kernel and never rests.
        no::Request stop_loss{no::Reduce{no::ExplicitUnits{0.0}}, "sl", "bracket"};
        stop_loss.trigger = no::Stop{80.0};
        spec_rows.stop_loss = stop_loss;
        // The trail leg is never asked for.
        receipt = tk::submit_bracket(base, spec_rows);
    };

    run(host, spec("e8-toolkit-leg-outcomes"), {100.0});

    // The ambiguity the outcomes remove: the refused leg and the leg that was
    // never asked for are the same empty optional.
    CHECK(!receipt.stop_loss.has_value());
    CHECK(!receipt.trail.has_value());

    // Requested and accepted: the handle, and the kernel's own accepted result.
    const auto& profit = receipt.outcome(tk::BracketLeg::TakeProfit);
    CHECK(profit.state == tk::BracketLegState::Accepted);
    CHECK(profit.requested());
    CHECK(profit.accepted());
    CHECK(!profit.rejected());
    CHECK(profit.handle() == receipt.take_profit);
    CHECK(!profit.reason().has_value());
    REQUIRE(profit.result.has_value());
    CHECK(profit.result->status == no::SubmitStatus::Accepted);
    CHECK(profit.result->event_ordinal != 0);

    // Requested and refused: no handle, and the reason the kernel gave.
    const auto& loss = receipt.outcome(tk::BracketLeg::StopLoss);
    CHECK(loss.state == tk::BracketLegState::Rejected);
    CHECK(loss.requested());
    CHECK(!loss.accepted());
    CHECK(loss.rejected());
    CHECK(!loss.handle().has_value());
    REQUIRE(loss.reason().has_value());
    CHECK(*loss.reason() == no::RequestRejectReason::InvalidQuantity);
    REQUIRE(loss.result.has_value());
    CHECK(loss.result->status == no::SubmitStatus::Rejected);
    CHECK(loss.result->event_ordinal == profit.result->event_ordinal + 1);

    // Not requested: no kernel verdict at all, because nothing was submitted.
    const auto& trail = receipt.outcome(tk::BracketLeg::Trail);
    CHECK(trail.state == tk::BracketLegState::NotRequested);
    CHECK(!trail.requested());
    CHECK(!trail.result.has_value());
    CHECK(!trail.reason().has_value());
    CHECK(!trail.handle().has_value());

    CHECK(!receipt.every_requested_leg_accepted());
    // One refusal reached the kernel and exactly one leg rests.
    const auto rejected_events = events<no::RejectedEvent>(host);
    REQUIRE(rejected_events.size() == 1);
    CHECK(rejected_events[0].reason == no::RequestRejectReason::InvalidQuantity);
    CHECK(host.native_working_requests().size() == 1);
    completed(host);

    // The fourth state: the legs were asked for, but the builder had no
    // allocated parent to wait on, so nothing reached the kernel and there is
    // no verdict to report.
    Host orphan;
    tk::BracketReceipt unparented;
    orphan.beginning = [&](Host& base) {
        tk::BracketSpec spec_rows;
        spec_rows.take_profit = resting("tp");
        unparented = tk::submit_bracket(base, spec_rows);
    };
    run(orphan, spec("e8-toolkit-unparented"), {100.0});
    const auto& absent_parent = unparented.outcome(tk::BracketLeg::TakeProfit);
    CHECK(absent_parent.state == tk::BracketLegState::NotSubmitted);
    CHECK(absent_parent.requested());
    CHECK(!absent_parent.accepted());
    CHECK(!absent_parent.rejected());
    CHECK(!absent_parent.result.has_value());
    CHECK(unparented.outcome(tk::BracketLeg::StopLoss).state
          == tk::BracketLegState::NotRequested);
    CHECK(!unparented.every_requested_leg_accepted());
    CHECK(events<no::RejectedEvent>(orphan).empty());
    CHECK(orphan.native_working_requests().empty());
    completed(orphan);
}

}  // namespace

int main() {
    test("bracket emits owner and group shapes", bracket_emits_owner_and_group_shapes);
    test("order book submit_or_replace and cancel", order_book_submit_or_replace_and_cancel);
    test("bracket knobs and pending order book", bracket_knobs_and_pending_order_book);
    test("bracket receipt separates absent from refused",
         bracket_receipt_separates_absent_from_refused);
    std::printf("L7 native toolkit: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
