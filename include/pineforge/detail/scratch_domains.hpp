#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pineforge::detail::working_store {

// One entry per working-store domain, including reservations. Owners using
// these constants share the check below; existing literal sites are inventoried
// here but must adopt their constant to make future renumbering checked.
inline constexpr std::uint64_t domain_version_allocator = 1; // detail/working_store.hpp: version/owner allocator.
inline constexpr std::uint64_t domain_payload_chunks = 2; // detail/working_store.hpp: serialized payload versions.
inline constexpr std::uint64_t domain_native_requests = 10; // src/native_order.cpp: live requests by priority.
inline constexpr std::uint64_t domain_native_priorities = 11; // src/native_order.cpp: incarnation-to-priority links.
inline constexpr std::uint64_t domain_native_successor_roots = 12; // src/native_order.cpp: successor roots.
inline constexpr std::uint64_t domain_native_cohort_owners = 13; // src/native_order.cpp: cohort-to-sequence owners.
inline constexpr std::uint64_t domain_native_cohort_origins = 14; // src/native_order.cpp: cohort origin sequences.
inline constexpr std::uint64_t domain_native_book_metadata = 15; // src/native_order.cpp: working-book count.
inline constexpr std::uint64_t domain_native_cohort_receipts = 16; // src/native_order.cpp: cohort receipt sequence.
inline constexpr std::uint64_t domain_native_issued = 17; // src/native_order.cpp: issued incarnation ranges.
inline constexpr std::uint64_t domain_cohort_origins = 42; // src/source/pine_adapter.cpp: OriginRoster sequence.
inline constexpr std::uint64_t domain_cohort_metadata = 43; // source/pine_adapter.hpp, detail/working_id_table.hpp: roster/scalar facts.
inline constexpr std::uint64_t domain_cohort_opened = 44; // source/pine_adapter.hpp: opened cohort members.
inline constexpr std::uint64_t domain_cohort_live_units = 45; // source/pine_adapter.hpp: live units by opening origin.
inline constexpr std::uint64_t domain_live_by_source_key = 46; // src/source/pine_adapter.cpp: live source-key map.
inline constexpr std::uint64_t domain_cohort_id_directory = 47; // detail/working_id_table.hpp: full cohort source ids.
inline constexpr std::uint64_t domain_cohort_id_order = 48; // detail/working_id_table.hpp: cohort id iteration order.
inline constexpr std::uint64_t domain_cohort_origin_positions = 50; // detail/working_store.hpp: OriginRoster position keys.
inline constexpr std::uint64_t domain_pending_roster_legacy = 51; // source/pending_roster_impl.hpp: legacy roster and detached snapshots.
inline constexpr std::uint64_t domain_queued_requests = 52; // src/source/pine_adapter.cpp: stored queued requests.
inline constexpr std::uint64_t domain_placements = 60; // src/source/pine_adapter.cpp: placement row table.
inline constexpr std::uint64_t domain_placement_metadata = 61; // src/source/pine_adapter.cpp: placement counts/history metadata.
inline constexpr std::uint64_t domain_pending_queues = 62; // src/source/pine_adapter.cpp: pending owner sequences.
inline constexpr std::uint64_t domain_pyramid_entries = 63; // src/source/pine_adapter.cpp: host pyramid entry sequence.
inline constexpr std::uint64_t domain_source_pending_view = 64; // src/source/pine_strategy_host.cpp: pending view cache.
inline constexpr std::uint64_t domain_erased_placement_minima = 65; // detail/working_minima.hpp: erased-placement minima.
inline constexpr std::uint64_t domain_pending_projection_index = 66; // Lane B pending projection index (reserved, unused; no owner file yet).
inline constexpr std::uint64_t domain_pending_roster_directory = 67; // source/pending_roster_impl.hpp: lane C full-handle directory.
inline constexpr std::uint64_t domain_pending_roster_tree = 68; // source/pending_roster.hpp: lane C counted AVL/root and format marker.
inline constexpr std::uint64_t domain_cohort_exposure = 69; // Lane E cohort exposure (reserved, unused; no owner file yet).
inline constexpr std::uint64_t domain_cohort_exposure_aux = 70; // Lane E cohort exposure (reserved, unused; no owner file yet).
inline constexpr std::uint64_t domain_pending_projection_counts = 71; // source/pending_projection_counts.hpp: temporary lane A counts until B replaces them.

inline constexpr std::array<std::uint64_t, 32> scratch_domains{{
    domain_version_allocator,
    domain_payload_chunks,
    domain_native_requests,
    domain_native_priorities,
    domain_native_successor_roots,
    domain_native_cohort_owners,
    domain_native_cohort_origins,
    domain_native_book_metadata,
    domain_native_cohort_receipts,
    domain_native_issued,
    domain_cohort_origins,
    domain_cohort_metadata,
    domain_cohort_opened,
    domain_cohort_live_units,
    domain_live_by_source_key,
    domain_cohort_id_directory,
    domain_cohort_id_order,
    domain_cohort_origin_positions,
    domain_pending_roster_legacy,
    domain_queued_requests,
    domain_placements,
    domain_placement_metadata,
    domain_pending_queues,
    domain_pyramid_entries,
    domain_source_pending_view,
    domain_erased_placement_minima,
    domain_pending_projection_index,
    domain_pending_roster_directory,
    domain_pending_roster_tree,
    domain_cohort_exposure,
    domain_cohort_exposure_aux,
    domain_pending_projection_counts,
}};

constexpr bool scratch_domains_are_distinct() noexcept {
    for (std::size_t i = 0; i < scratch_domains.size(); ++i)
        for (std::size_t j = i + 1; j < scratch_domains.size(); ++j)
            if (scratch_domains[i] == scratch_domains[j]) return false;
    return true;
}

static_assert(scratch_domains_are_distinct(), "working-store domains must be distinct");

} // namespace pineforge::detail::working_store
