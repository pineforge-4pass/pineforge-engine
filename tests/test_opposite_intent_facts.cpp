// Native journal-backed placement dependency. No Engine::run, feed, Pine,
// reference strategy, or grader is used here.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <cstdio>
#include <memory>

using namespace pineforge;
using pineforge::source::PendingOrder;
using pineforge::source::placement_has_opposite_market_predecessor;
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, const MarketAdmissionJournal*,
                               pf_pending_order_v1_t*);
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
}
namespace {
int checks = 0;
int failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #value); } } while (0)

std::shared_ptr<const admission::CommandObservation> observation(
        uint64_t command, admission::CommandKind kind, bool buy, int bar,
        bool priced = true) {
    auto result = std::make_shared<admission::CommandObservation>();
    result->command = command;
    result->kind = kind;
    result->buy = buy;
    result->bar = bar;
    result->placement_side = static_cast<int>(PositionSide::FLAT);
    result->prices.limit = priced ? 101.0 : admission::absent;
    result->prices.stop = admission::absent;
    return result;
}

admission::Journal journal_with(admission::BookObservation peer,
                                const std::shared_ptr<const admission::CommandObservation>& current,
                                uint64_t current_incarnation, bool removed) {
    admission::Journal journal;
    auto first = journal.reserve();
    auto second = journal.reserve();
    journal.abandon(first.sequence());
    admission::CommandEvent event;
    event.observation = current;
    event.admitted_incarnation = current_incarnation;
    event.before.push_back(std::move(peer));
    if (removed) event.removed.push_back(event.before.front().incarnation);
    journal.append(std::move(event));
    return journal;
}

PendingOrder current_order(const std::shared_ptr<const admission::CommandObservation>& current) {
    PendingOrder order{};
    order.type = OrderType::ENTRY;
    order.is_long = current->buy;
    order.created_position_side = PositionSide::FLAT;
    order.created_seq = 2;
    order.incarnation = 22;
    order.market_admission.bind(current);
    order.legs.set_stop_price(101.0);
    return order;
}

void accepted_peer_is_reconstructed() {
    auto current_origin = observation(2, admission::CommandKind::Entry, true, 0);
    auto peer_origin = observation(1, admission::CommandKind::Entry, false, 0, false);
    admission::BookObservation peer;
    peer.incarnation = 11;
    peer.priority = 1;
    peer.bar = 0;
    peer.type = static_cast<int>(OrderType::MARKET);
    peer.draft.bind(peer_origin);
    const auto order = current_order(current_origin);
    auto journal = journal_with(peer, current_origin, order.incarnation, false);
    CHECK(placement_has_opposite_market_predecessor(journal, order));
    pf_pending_order_v1_t mirror{};
    fill_pending_order_mirror(order, &journal, &mirror);
    CHECK(mirror.reverses_same_bar_market_from_flat == 1);
    bool refused_without_context = false;
    try { fill_pending_order_mirror(order, &mirror); }
    catch (const std::logic_error&) { refused_without_context = true; }
    CHECK(refused_without_context);
    refused_without_context = false;
    mirror.reverses_same_bar_market_from_flat = 37;
    try { fill_pending_order_mirror(order, nullptr, &mirror); }
    catch (const std::logic_error&) { refused_without_context = true; }
    CHECK(refused_without_context);
    CHECK(mirror.reverses_same_bar_market_from_flat == 37);

    // An ordinary MARKET cannot have this priced-entry predecessor fact, so
    // its complete mirror requires no historical context.
    auto market = order;
    market.type = OrderType::MARKET;
    fill_pending_order_mirror(market, &mirror);
    CHECK(mirror.reverses_same_bar_market_from_flat == 0);
}

void removed_peer_and_unknown_peer_fail_closed() {
    auto current_origin = observation(2, admission::CommandKind::Entry, true, 0);
    auto peer_origin = observation(1, admission::CommandKind::Entry, false, 0, false);
    admission::BookObservation removed_peer;
    removed_peer.incarnation = 11;
    removed_peer.priority = 1;
    removed_peer.bar = 0;
    removed_peer.type = static_cast<int>(OrderType::MARKET);
    removed_peer.draft.bind(peer_origin);
    const auto order = current_order(current_origin);
    auto removed_journal = journal_with(removed_peer, current_origin, order.incarnation, true);
    CHECK(!placement_has_opposite_market_predecessor(removed_journal, order));

    admission::BookObservation unknown_peer;
    unknown_peer.incarnation = 12;
    unknown_peer.priority = 1;
    unknown_peer.bar = 0;
    unknown_peer.type = static_cast<int>(OrderType::MARKET);
    unknown_peer.buy = false;
    auto unknown_journal = journal_with(unknown_peer, current_origin, order.incarnation, false);
    CHECK(placement_has_opposite_market_predecessor(unknown_journal, order));

    unknown_peer.buy = true;
    auto same_direction_journal = journal_with(unknown_peer, current_origin,
                                               order.incarnation, false);
    CHECK(!placement_has_opposite_market_predecessor(same_direction_journal, order));
}

void absent_current_draft_and_controls_fail_closed() {
    PendingOrder manual{};
    manual.type = OrderType::ENTRY;
    manual.created_position_side = PositionSide::FLAT;
    manual.created_seq = 2;
    manual.incarnation = 22;
    admission::Journal empty;
    CHECK(!placement_has_opposite_market_predecessor(empty, manual));

    auto unpriced_origin = observation(2, admission::CommandKind::Entry, true, 0, false);
    const auto unpriced = current_order(unpriced_origin);
    auto peer_origin = observation(1, admission::CommandKind::Entry, false, 0, false);
    admission::BookObservation peer;
    peer.incarnation = 11; peer.priority = 1; peer.bar = 0;
    peer.type = static_cast<int>(OrderType::MARKET); peer.draft.bind(peer_origin);
    auto journal = journal_with(peer, unpriced_origin, unpriced.incarnation, false);
    CHECK(!placement_has_opposite_market_predecessor(journal, unpriced));
}
}

int main() {
    accepted_peer_is_reconstructed();
    removed_peer_and_unknown_peer_fail_closed();
    absent_current_draft_and_controls_fail_closed();
    std::printf("opposite intent facts: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
