// Differential controls for the direct hash writer and completed-boundary fold.
// The reference keeps the pre-PERF441 owning Field/variant hash protocol.
#include "../src/source/market_admission_fold.hpp"
#include <pineforge/compat/pine/market_admission.hpp>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr uint64_t kBasis = 1469598103934665603ULL;

uint64_t reference(uint64_t digest, const admission::Event& event,
                   const std::string& path = "event") {
    BrokerStateHashSink sink;
    sink.h = digest;
    admission::reflect(event, path, [&](const admission::Field& field) {
        sink.s(field.path);
        sink.u(field.value.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, uint64_t>) sink.u(value);
            else if constexpr (std::is_same_v<T, int64_t>) sink.i(value);
            else if constexpr (std::is_same_v<T, double>) sink.d(value);
            else sink.s(value);
        }, field.value);
    });
    return sink.h;
}

admission::CommandEvent command(admission::Journal& journal, int bar,
                                admission::Outcome outcome) {
    auto observation = std::make_shared<admission::CommandObservation>();
    observation->command = journal.next_sequence();
    observation->bar = bar;
    observation->id = std::string(128, 'i') + std::string("\0id", 3);
    observation->oca_name = std::string("oca\0name", 8);
    observation->kind = admission::CommandKind::Raw;
    observation->birth = OrderBirth::fill_evaluation(bar, -123456,
        BirthCursor::segment(BirthCursorDomain::HistoricalPath, 2, 4),
        17.5, 23, 29, 31);
    observation->buy = true;
    observation->configuration.process_on_close = true;
    observation->configuration.slippage = -7;
    observation->placement_cycle = std::numeric_limits<int64_t>::min();
    observation->requested_quantity = -0.0;
    observation->prices.limit = std::numeric_limits<double>::infinity();
    observation->prices.stop = -std::numeric_limits<double>::infinity();
    uint64_t nan_bits = 0x7ff8000000000042ULL;
    static_assert(sizeof(nan_bits) == sizeof(observation->signal_close));
    std::memcpy(&observation->signal_close, &nan_bits, sizeof(nan_bits));
    observation->original_sizing = admission::SizingObservation{1.5, 2.5, 3.5, -0.0, 5.5};
    admission::CommandEvent event;
    event.observation = std::move(observation);
    event.outcome = outcome;
    event.admitted_incarnation = std::numeric_limits<uint64_t>::max();
    event.removed = {0, 1, std::numeric_limits<uint64_t>::max()};
    return event;
}

std::vector<admission::Event> append_window(admission::Journal& journal, int bar) {
    std::vector<admission::Event> events;
    auto opening = command(journal, bar, admission::Outcome::RejectedAffordability);
    admission::BookObservation book;
    book.draft.bind(opening.observation);
    book.incarnation = 47;
    book.priority = -49;
    book.bar = bar;
    book.buy = true;
    book.id = opening.observation->id;
    book.oca_name = opening.observation->oca_name;
    book.birth = opening.observation->birth;
    opening.before.push_back(book);
    events.emplace_back(opening);

    admission::ReviewEvent review;
    review.receipt = {journal.next_sequence(), admission::Checkpoint::ExplicitPair, bar, 0};
    book.draft.reviewed({review.receipt.sequence, review.receipt.checkpoint, bar,
                         opening.observation->command});
    review.book.assign(13, book);
    review.reviewed.assign(13, book); // Multi-digit indices and repeated shared origins.
    review.resolutions.push_back({47, admission::ResolutionKind::PairedTransaction,
                                  53, -59, 61, 7.5});
    review.causes = {opening.observation->command};

    admission::SizingEvent sizing;
    sizing.receipt = {journal.next_sequence(), 67, bar, opening.observation->command};
    sizing.incarnation = 47;
    sizing.before = {11.5, 13.5, 17.5, -0.0, 19.5};
    sizing.after = {23.5, 29.5, 31.5, 37.5, 41.5};
    sizing.affordability_equity_before = -0.0;
    sizing.affordability_equity_after = 43.5;
    for (auto& row : review.reviewed) row.draft.sizing_revised(sizing.receipt);
    events.emplace_back(std::move(review));
    events.emplace_back(std::move(sizing));
    for (const auto& event : events) journal.append(event);
    return events;
}

void direct_bytes_match_diagnostic_fields() {
    admission::Journal journal;
    const auto events = append_window(journal, 3);
    for (const auto& path : {std::string("event"), std::string(1024, 'p')}) {
        for (const uint64_t seed : {kBasis, uint64_t(0), std::numeric_limits<uint64_t>::max()}) {
            auto expected = seed;
            BrokerStateHashSink direct;
            direct.h = seed;
            for (const auto& event : events) {
                expected = reference(expected, event, path);
                admission::reflect(event, std::string_view(path), direct);
                assert(direct.h == expected);
            }
        }
    }
}

void fused_retirement_matches_separate_protocol() {
    // Cover a close before any query, after a partial query, and after a
    // complete query, with both an empty and a nonzero retired prefix.
    for (std::size_t already_folded = 0; already_folded <= 3; ++already_folded) {
        admission::Journal fused;
        admission::Journal separate;
        std::size_t count = 0;
        uint64_t digest = kBasis, last = 0;
        for (int bar = 0; bar < 3; ++bar) {
            const auto events = append_window(fused, bar);
            for (const auto& event : events) {
                // Reproduce the allocations in the independent journal.
                const auto sequence = separate.next_sequence();
                assert(sequence == admission::sequence(event));
                separate.append(event);
            }
            auto expected = separate.retired_prefix().digest;
            auto expected_count = separate.retired_prefix().count;
            uint64_t expected_last = separate.retired_prefix().last_sequence;
            for (std::size_t i = 0; i < events.size(); ++i) {
                expected = reference(expected, events[i]);
                ++expected_count;
                expected_last = admission::sequence(events[i]);
                if (i < already_folded) {
                    digest = reference(digest, events[i]);
                    ++count;
                    last = expected_last;
                }
            }
            separate.retire_prefix(expected_count, expected, expected_last);
            fused.fold_and_retire(count, digest, last);
            const auto& prefix = fused.retired_prefix();
            assert(count == expected_count && prefix.count == expected_count);
            assert(digest == expected && prefix.digest == expected);
            assert(last == expected_last && prefix.last_sequence == expected_last);
            assert(prefix.last_rejected_command_bar == bar);
            assert(compat::pine::last_rejected_command_bar(fused)
                   == compat::pine::last_rejected_command_bar(separate));
            assert(fused.events().empty());
            // The empty boundary must repair a stale query cursor.
            count = 0; digest = 0; last = 0;
            fused.fold_and_retire(count, digest, last);
            assert(count == expected_count && digest == expected && last == expected_last);
        }
    }
}

template<class Fn> void refuses_unfinished(Fn&& call) {
    bool refused = false;
    try { call(); }
    catch (const std::logic_error&) { refused = true; }
    assert(refused);
}

void unfinished_and_reordered_windows() {
    admission::Journal journal;
    std::size_t count = 0;
    uint64_t digest = kBasis, last = 0;
    {
        auto allocation = journal.reserve();
        refuses_unfinished([&] { journal.fold_and_retire(count, digest, last); });
        assert(journal.retired_prefix().count == 0);
    }
    auto outer = command(journal, 0, admission::Outcome::RejectedPricedCap);
    auto inner = command(journal, 1, admission::Outcome::RejectedAffordability);
    journal.append(inner);
    digest = reference(kBasis, inner);
    count = 1; last = admission::sequence(inner);
    refuses_unfinished([&] { journal.fold_and_retire(count, digest, last); });
    assert(journal.events().size() == 1 && journal.retired_prefix().count == 0);
    journal.append(outer); // Earlier insertion invalidates the query's boundary.
    auto expected = reference(reference(kBasis, outer), inner);
    journal.fold_and_retire(count, digest, last);
    assert(count == 2 && digest == expected);
    assert(journal.retired_prefix().last_rejected_command_bar == 1);

    auto removed = command(journal, 2, admission::Outcome::RejectedAffordability);
    auto kept = command(journal, 3, admission::Outcome::Admitted);
    journal.append(removed);
    journal.append(kept);
    digest = reference(reference(digest, removed), kept);
    count += 2; last = admission::sequence(kept);
    journal.retain({admission::sequence(kept)});
    expected = reference(expected, kept);
    journal.fold_and_retire(count, digest, last);
    assert(count == 3 && digest == expected);
    assert(journal.retired_prefix().last_rejected_command_bar == 1);

    const auto saved = journal;
    journal.reset();
    journal.fold_and_retire(count, digest, last);
    assert(count == 0 && digest == kBasis && last == 0);
    journal = saved;
    journal.fold_and_retire(count, digest, last);
    assert(count == 3 && digest == expected && last == admission::sequence(kept));

    admission::Journal lifetime;
    auto event = command(lifetime, 4, admission::Outcome::Admitted);
    std::weak_ptr<const admission::CommandObservation> weak = event.observation;
    lifetime.append(std::move(event));
    count = 0; digest = kBasis; last = 0;
    lifetime.fold_and_retire(count, digest, last);
    assert(weak.expired() && lifetime.events().empty());
}

void full_history_keeps_the_same_fold() {
    admission::Journal journal;
    journal.retain_full_observation_history();
    std::size_t count = 0;
    uint64_t digest = kBasis, last = 0, expected = kBasis;
    for (int bar = 0; bar < 3; ++bar) {
        const auto events = append_window(journal, bar);
        for (const auto& event : events) expected = reference(expected, event);
        journal.fold_and_retire(count, digest, last);
        assert(count == static_cast<std::size_t>((bar + 1) * 3));
        assert(digest == expected && journal.events().size() == count);
        assert(journal.retired_prefix().count == 0);
        assert(journal.retired_prefix().last_rejected_command_bar == -1);
        journal.fold_and_retire(count, digest, last);
        assert(digest == expected && journal.events().size() == count);
    }
}
} // namespace

int main() {
    direct_bytes_match_diagnostic_fields();
    fused_retirement_matches_separate_protocol();
    unfinished_and_reordered_windows();
    full_history_keeps_the_same_fold();
    std::puts("PERF441 admission fold: byte protocol and retirement controls passed");
}
