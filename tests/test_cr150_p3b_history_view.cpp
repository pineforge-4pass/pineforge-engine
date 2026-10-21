// P37: compare the bounded per-bar query with the unchanged independent
// map-based fold in src/compat/pine/market_admission.cpp after every event,
// then prove the L19e exact prefix retirement and the Full override still
// govern which events the window readback sees.
// RED against 6cfcff0696: HistoryView and Journal::visit_events are absent.
#include <pineforge/compat/pine/market_admission.hpp>
#include "cr150_p3b_allocations.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace pineforge;
using namespace pineforge::admission;
int checks = 0;
int failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (false)

Configuration configuration(bool pair) {
    Configuration c;
    c.long_margin = c.short_margin = 100.0;
    c.pyramiding = pair ? 2 : 1;
    c.default_quantity_type = 1;
    c.default_quantity_value = 100.0;
    return c;
}
BookObservation old_order(int bar, uint64_t incarnation, bool pair, int type) {
    BookObservation row;
    row.incarnation = incarnation;
    row.bar = bar;
    row.type = type;
    row.placement_side = static_cast<int>(incarnation % 2);
    row.id = "old-" + std::to_string(incarnation);
    auto observation = std::make_shared<CommandObservation>();
    observation->command = incarnation;
    observation->bar = bar;
    observation->configuration = configuration(pair);
    observation->requested_quantity = pair ? 2.0 : absent;
    observation->quantized_fixed_quantity = 2.0;
    observation->original_sizing = SizingObservation{2, 1000, 20, 20, 1};
    row.draft.bind(std::move(observation));
    return row;
}

void verify(const Journal& journal) {
    const auto oracle = compat::pine::admission_history(journal);
    const auto view = compat::pine::admission_history_view(journal);
    // The oracle's resident maps are deliberately outside the sample. The
    // production query retains one scalar even as the journal window widens.
    cr150_p3b_allocations::begin();
    for (int bar = -9; bar <= 30; ++bar) {
        const auto p = oracle.pair_causes.find(bar);
        const auto d = oracle.default_causes.find(bar);
        CHECK(view.pair_cause(bar) == (p == oracle.pair_causes.end() ? 0 : p->second));
        CHECK(view.default_cause(bar) == (d == oracle.default_causes.end() ? 0 : d->second));
    }
    cr150_p3b_allocations::end();
    CHECK(cr150_p3b_allocations::live == 0);
    CHECK(cr150_p3b_allocations::peak <= 1024);
}

void window(int width) {
    Journal journal;
    for (int i = 0; i < width; ++i) {
        const int bar = i % 23 - 6;
        const bool pair = (i % 2) != 0;
        auto observation = std::make_shared<CommandObservation>();
        observation->command = journal.next_sequence();
        observation->bar = bar;
        observation->id = "command-" + std::to_string(i);
        observation->kind = static_cast<CommandKind>(i % 4);
        observation->configuration = configuration(pair);
        observation->requested_quantity = pair ? 2.0 : absent;
        observation->quantized_fixed_quantity = 2.0;
        // Exercise both ordinary-terminal and scope-qualified commands.
        if (i % 7 == 0) {
            observation->configuration.process_on_close = true;
            observation->configuration.calc_on_fills = true;
        }
        CommandEvent event;
        event.observation = std::move(observation);
        event.outcome = i % 11 == 0 ? Outcome::IgnoredTradingWindow
            : (i % 13 == 0 ? Outcome::RejectedIntradayCap
               : (i % 3 == 0 ? Outcome::OpeningRejectedReductionAdmitted : Outcome::Admitted));
        event.before.push_back(old_order(bar, 2 * i + 100, pair, i % 4));
        event.before.push_back(old_order(bar - 1, 2 * i + 101, false, (i + 1) % 4));
        event.removed.push_back(2 * i + 100);
        if (i % 3) event.removed.push_back(2 * i + 101);
        journal.append(std::move(event));
        verify(journal);
        if (i % 5 == 4) {
            ReviewEvent review;
            review.receipt = {journal.next_sequence(), static_cast<Checkpoint>((i / 5) % 3), bar, 0};
            if (i % 2) review.reviewed.push_back(old_order(bar - 1, 1000 + i, pair, 0));
            journal.append(std::move(review));
            verify(journal);
        }
    }
    CHECK(journal.events().size() >= static_cast<std::size_t>(width));
    std::size_t visited = 0;
    uint64_t prior = 0;
    CHECK(journal.visit_events([&](const Event& event) {
        CHECK(sequence(event) > prior);
        prior = sequence(event);
        // Nested const iteration must not invalidate the outer event.
        std::size_t inner = 0;
        CHECK(!journal.visit_events([&](const Event&) { ++inner; return false; }));
        CHECK(inner == 1 && sequence(event) == prior);
        ++visited;
        return true;
    }));
    CHECK(visited == journal.events().size());
    visited = 0;
    CHECK(!journal.visit_events([&](const Event&) { return ++visited < 3; }));
    CHECK(visited == 3);
}

// L19e: a completed bar's window folds into the retired prefix and is
// destroyed. The per-bar view reads the live window only, so a retired bar
// answers zero exactly as the window fold does; the prefix bookkeeping is
// unchanged. Both halves are asserted against the journal's own contract.
void prefix_retirement() {
    Journal journal;
    constexpr int abandoned_bar = 7;
    for (int i = 0; i < 6; ++i) {
        auto observation = std::make_shared<CommandObservation>();
        observation->command = journal.next_sequence();
        observation->bar = i == 4 ? abandoned_bar : i;
        observation->id = "retire-" + std::to_string(i);
        observation->kind = CommandKind::Entry;
        observation->configuration = configuration(i % 2 != 0);
        observation->requested_quantity = absent;
        CommandEvent event;
        event.observation = std::move(observation);
        event.outcome = i == 4 ? Outcome::RejectedAffordability : Outcome::Admitted;
        journal.append(std::move(event));
    }
    const auto oracle = compat::pine::admission_history(journal);
    const auto view = compat::pine::admission_history_view(journal);
    for (int bar = -1; bar <= abandoned_bar + 1; ++bar) {
        const auto p = oracle.pair_causes.find(bar);
        const auto d = oracle.default_causes.find(bar);
        CHECK(view.pair_cause(bar) == (p == oracle.pair_causes.end() ? 0 : p->second));
        CHECK(view.default_cause(bar) == (d == oracle.default_causes.end() ? 0 : d->second));
    }
    const std::size_t count = journal.events().size();
    const uint64_t last = sequence(journal.events().back());
    CHECK(count == 6);
    journal.retire_prefix(count, 0x5a5a5a5a5a5a5a5aULL, last);
    CHECK(journal.events().empty());
    CHECK(journal.retired_prefix().count == count);
    CHECK(journal.retired_prefix().digest == 0x5a5a5a5a5a5a5a5aULL);
    CHECK(journal.retired_prefix().last_sequence == last);
    CHECK(journal.retired_prefix().last_rejected_command_bar == abandoned_bar);
    CHECK(journal.event_count() == count);
    // The retired window is gone; the live view replays nothing behind it.
    CHECK(compat::pine::admission_history_view(journal).pair_cause(abandoned_bar) == 0);
    CHECK(compat::pine::admission_history_view(journal).pair_cause(1) == 0);
    CHECK(compat::pine::admission_history_view(journal).default_cause(2) == 0);
    CHECK(journal.visit_events([&](const Event&) { return false; }));
    // Events appended after the boundary are the next window's alone.
    auto observation = std::make_shared<CommandObservation>();
    observation->command = journal.next_sequence();
    observation->bar = 11;
    observation->id = "after-retirement";
    observation->kind = CommandKind::Entry;
    observation->configuration = configuration(false);
    observation->requested_quantity = absent;
    CommandEvent event;
    event.observation = std::move(observation);
    journal.append(std::move(event));
    CHECK(journal.events().size() == 1);
    CHECK(compat::pine::admission_history_view(journal).default_cause(11) != 0);
    CHECK(compat::pine::admission_history_view(journal).default_cause(2) == 0);
    CHECK(journal.retired_prefix().count == count);
}

// Full override: the diagnostic observer retains every payload, so retirement
// validates the exact window but neither folds nor clears it.
void full_observation_override() {
    Journal journal;
    journal.retain_full_observation_history();
    for (int i = 0; i < 3; ++i) {
        auto observation = std::make_shared<CommandObservation>();
        observation->command = journal.next_sequence();
        observation->bar = i;
        observation->id = "full-" + std::to_string(i);
        observation->kind = CommandKind::Entry;
        observation->configuration = configuration(false);
        observation->requested_quantity = absent;
        CommandEvent event;
        event.observation = std::move(observation);
        journal.append(std::move(event));
    }
    const std::size_t count = journal.events().size();
    const uint64_t last = sequence(journal.events().back());
    journal.retire_prefix(count, 0xdeadbeefULL, last);
    CHECK(journal.events().size() == count);
    CHECK(journal.retired_prefix().count == 0);
    CHECK(journal.retired_prefix().digest == 1469598103934665603ULL);
    CHECK(journal.retired_prefix().last_sequence == 0);
    CHECK(journal.event_count() == count);
    CHECK(compat::pine::admission_history_view(journal).default_cause(2) != 0);
}
}
int main() {
    window(19);
    window(83);
    prefix_retirement();
    full_observation_override();
    std::printf("P37 admission history view: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
