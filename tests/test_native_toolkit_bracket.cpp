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

}  // namespace

int main() {
    test("bracket emits owner and group shapes", bracket_emits_owner_and_group_shapes);
    test("order book submit_or_replace and cancel", order_book_submit_or_replace_and_cancel);
    std::printf("L7 native toolkit: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
