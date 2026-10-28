#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;
using Handle = pineforge::native_order::RequestHandle;
using PlacementSnapshot = pineforge::source::PlacementSnapshot;
using PineOrderFamily = pineforge::source::PineOrderFamily;

template<class Tag, auto Member>
struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct SameBar { friend auto access(SameBar); };
struct Entries { friend auto access(Entries); };
struct Brackets { friend auto access(Brackets); };
struct ProjectionCacheInitialized { friend auto access(ProjectionCacheInitialized); };

template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<SameBar, &Adapter::pending_same_bar_commands_>;
template struct Access<Entries, &Adapter::pending_entries_>;
template struct Access<Brackets, &Adapter::pending_bracket_legs_>;
template struct Access<ProjectionCacheInitialized,
                       &Adapter::pending_projection_cache_initialized_>;

struct RawRow {
    PlacementSnapshot snapshot{};
    Handle handle{};
    bool present = false;
};

int failures = 0;
int checks = 0;

#define CHECK(expression) do {                                                   \
    ++checks;                                                                    \
    if (!(expression)) {                                                         \
        ++failures;                                                              \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    }                                                                             \
} while (false)

bool is_exit(PineOrderFamily family) {
    return family == PineOrderFamily::ExitLimit
        || family == PineOrderFamily::ExitStop
        || family == PineOrderFamily::ExitTrail;
}

bool same_projected_key(const PlacementSnapshot& left,
                        const PlacementSnapshot& right) {
    return is_exit(left.family) && is_exit(right.family)
        && left.source_id == right.source_id
        && left.from_entry == right.from_entry
        && left.bracket_origin == right.bracket_origin;
}

std::vector<RawRow> raw_rows(const Adapter& adapter) {
    std::vector<RawRow> rows;
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(Live{})) {
        const auto found = placements.find(handle.incarnation);
        if (found != placements.end())
            rows.push_back({found->second, handle, true});
        else
            rows.push_back({{}, handle, false});
    }
    const auto append = [&](const auto& owner) {
        for (const auto& row : owner)
            rows.push_back({row.snapshot, {}, true});
    };
    append(adapter.*access(SameBar{}));
    append(adapter.*access(Entries{}));
    append(adapter.*access(Brackets{}));
    return rows;
}

std::vector<RawRow> independent_projection(const Adapter& adapter) {
    const auto raw = raw_rows(adapter);
    std::vector<RawRow> projected;
    projected.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        if (!raw[index].present) continue;
        bool duplicate = false;
        if (is_exit(raw[index].snapshot.family)) {
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (raw[prior].present
                    && same_projected_key(raw[index].snapshot, raw[prior].snapshot)) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (!duplicate) projected.push_back(raw[index]);
    }
    return projected;
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

Handle handle_for(std::size_t serial) {
    Handle handle;
    handle.run.session_key = "pending-book";
    handle.run.run_number = 1;
    handle.incarnation = static_cast<std::uint64_t>(serial + 1);
    return handle;
}

PlacementSnapshot snapshot_for(std::size_t serial, PineOrderFamily family) {
    PlacementSnapshot snapshot;
    snapshot.family = family;
    snapshot.source_id = "pending-" + std::to_string(serial);
    snapshot.from_entry = "entry-" + std::to_string(serial);
    snapshot.comment = "comment-" + std::to_string(serial);
    snapshot.oca_name = "oca-" + std::to_string(serial % 3);
    snapshot.oca_type = static_cast<int>(serial % 3);
    snapshot.qty_type = static_cast<int>(pineforge::QtyType::FIXED);
    snapshot.requested_qty = 10.0 + static_cast<double>(serial);
    snapshot.projection_remaining_qty = 1.0 + static_cast<double>(serial);
    snapshot.qty_percent = 25.0 + static_cast<double>(serial % 5);
    snapshot.is_long = (serial % 2) == 0;
    snapshot.source_sequence = static_cast<std::uint64_t>(serial + 100);
    snapshot.command_sequence = static_cast<std::uint64_t>(serial + 200);
    snapshot.projection_created_bar = static_cast<int>(serial % 17);
    snapshot.bracket_origin = handle_for(serial + 10000);
    snapshot.exit_levels.limit = 200.0 + static_cast<double>(serial);
    snapshot.exit_levels.stop = 100.0 + static_cast<double>(serial);
    snapshot.exit_levels.trail_points = 3.0 + static_cast<double>(serial);
    snapshot.exit_levels.trail_offset = 1.5 + static_cast<double>(serial);
    snapshot.exit_levels.trail_price = 150.0 + static_cast<double>(serial);
    snapshot.exit_levels.profit_ticks = 4.0 + static_cast<double>(serial);
    snapshot.exit_levels.loss_ticks = 2.0 + static_cast<double>(serial);
    return snapshot;
}

class Fixture {
public:
    explicit Fixture(std::size_t population) {
        adapter.fixture_use_memory_auxiliary_tables();
        auto& placements = adapter.*access(Placements{});
        auto& live = adapter.*access(Live{});
        auto& same_bar = adapter.*access(SameBar{});
        auto& entries = adapter.*access(Entries{});
        auto& brackets = adapter.*access(Brackets{});
        for (std::size_t serial = 0; serial < population; ++serial) {
            PineOrderFamily family = PineOrderFamily::ExitLimit;
            switch (serial % 4) {
            case 1: family = PineOrderFamily::ExitStop; break;
            case 2: family = PineOrderFamily::ExitTrail; break;
            default: break;
            }
            const auto snapshot = snapshot_for(serial, family);
            switch (serial % 4) {
            case 0: {
                const auto handle = handle_for(serial);
                placements.try_emplace(handle.incarnation, snapshot);
                live.push_back(handle);
                break;
            }
            case 1:
                same_bar.emplace_back();
                same_bar.back().snapshot = snapshot;
                same_bar.back().replacement_key = snapshot.source_id;
                break;
            case 2:
                entries.emplace_back();
                entries.back().snapshot = snapshot;
                entries.back().replacement_key = snapshot.source_id;
                break;
            default:
                brackets.emplace_back();
                brackets.back().snapshot = snapshot;
                brackets.back().replacement_key = snapshot.source_id;
                break;
            }
        }
    }

    Adapter adapter;
};

void check_returned_values(const Fixture& fixture,
                           const std::vector<RawRow>& expected) {
    const auto& view = fixture.adapter.pending_intent_view();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        pf_pending_order_v1_t row{};
        CHECK(view.copy_v1(static_cast<int>(index), &row) == 0);
        const auto& want = expected[index];
        CHECK(std::string(row.id) == want.snapshot.source_id);
        CHECK(std::string(row.from_entry) == want.snapshot.from_entry);
        CHECK(std::string(row.oca_name) == want.snapshot.oca_name);
        CHECK(std::string(row.comment) == want.snapshot.comment);
        CHECK(row.id_hash64 == fnv1a(want.snapshot.source_id));
        CHECK(row.from_entry_hash64 == fnv1a(want.snapshot.from_entry));
        CHECK(row.oca_name_hash64 == fnv1a(want.snapshot.oca_name));
        CHECK(row.comment_hash64 == fnv1a(want.snapshot.comment));
        const int expected_type = want.snapshot.family == PineOrderFamily::Entry
            ? 1 : (want.snapshot.family == PineOrderFamily::Order ? 3 : 2);
        CHECK(row.type == expected_type);
        CHECK(row.is_long == (want.snapshot.is_long ? 1U : 0U));
        CHECK(row.limit_price == want.snapshot.exit_levels.limit);
        CHECK(row.stop_price == want.snapshot.exit_levels.stop);
        CHECK(row.trail_points == want.snapshot.exit_levels.trail_points);
        CHECK(row.trail_price == want.snapshot.exit_levels.trail_price);
        CHECK(row.trail_offset == want.snapshot.exit_levels.trail_offset);
        CHECK(row.profit_ticks == want.snapshot.exit_levels.profit_ticks);
        CHECK(row.loss_ticks == want.snapshot.exit_levels.loss_ticks);
        CHECK(row.qty == want.snapshot.projection_remaining_qty);
        CHECK(row.qty_type == want.snapshot.qty_type);
        CHECK(row.qty_percent == want.snapshot.qty_percent);
        CHECK(row.created_bar == want.snapshot.projection_created_bar);
        CHECK(row.created_seq
              == static_cast<std::int64_t>(want.snapshot.source_sequence));
        CHECK(row.incarnation == want.handle.incarnation);
    }
}

void check_projection(const Fixture& fixture, bool force_fallback = false) {
    const auto expected = independent_projection(fixture.adapter);
    CHECK(fixture.adapter.pending_intent_view().size()
          == static_cast<int>(expected.size()));
    if (force_fallback)
        CHECK(!(fixture.adapter.*access(ProjectionCacheInitialized{})));
    check_returned_values(fixture, expected);
    pf_pending_order_v1_t extra{};
    CHECK(fixture.adapter.pending_intent_view().copy_v1(
              static_cast<int>(expected.size()), &extra) == -1);
    if (force_fallback)
        CHECK(!(fixture.adapter.*access(ProjectionCacheInitialized{})));
}

void check_cross_family_projection(bool force_fallback = false) {
    Fixture fixture(4);
    const auto first = raw_rows(fixture.adapter).front().snapshot;
    auto& same_bar = fixture.adapter.*access(SameBar{});
    auto& entries = fixture.adapter.*access(Entries{});
    auto& brackets = fixture.adapter.*access(Brackets{});
    same_bar.front().snapshot = first;
    same_bar.front().snapshot.family = PineOrderFamily::ExitStop;
    entries.front().snapshot = first;
    entries.front().snapshot.family = PineOrderFamily::ExitTrail;
    brackets.front().snapshot = first;

    // Equal sibling levels isolate identity and representative selection.
    // Each change follows a read and keeps the raw population unchanged.
    if (force_fallback) check_projection(fixture);
    fixture.adapter.fixture_force_pending_projection_fallback(force_fallback);
    check_projection(fixture, force_fallback);
    auto& changed = same_bar.front().snapshot;
    changed.source_id += "-rekeyed";
    check_projection(fixture, force_fallback);
    changed.source_id = first.source_id;
    check_projection(fixture, force_fallback);
    changed.from_entry += "-rekeyed";
    check_projection(fixture, force_fallback);
    changed.from_entry = first.from_entry;
    check_projection(fixture, force_fallback);
    changed.bracket_origin.run.session_key = "pending-book-other-session";
    check_projection(fixture, force_fallback);
    changed.bracket_origin = first.bracket_origin;
    check_projection(fixture, force_fallback);
    ++changed.bracket_origin.run.run_number;
    check_projection(fixture, force_fallback);
    changed.bracket_origin = first.bracket_origin;
    check_projection(fixture, force_fallback);
    ++changed.bracket_origin.incarnation;
    check_projection(fixture, force_fallback);
    changed.bracket_origin = first.bracket_origin;
    check_projection(fixture, force_fallback);
    changed.family = PineOrderFamily::Entry;
    check_projection(fixture, force_fallback);
    changed.family = PineOrderFamily::ExitStop;
    check_projection(fixture, force_fallback);

    // Erase only the placement: the live roster retains a raw hole. The
    // next representative is staged, and therefore has no live incarnation.
    auto& placements = fixture.adapter.*access(Placements{});
    const auto live_handle = (fixture.adapter.*access(Live{})).front();
    CHECK(placements.erase(live_handle.incarnation));
    check_projection(fixture, force_fallback);
    CHECK(placements.try_emplace(live_handle.incarnation, first).second);
    check_projection(fixture, force_fallback);
}

void check_roster_reorder(bool force_fallback = false) {
    Fixture fixture(12);
    auto& placements = fixture.adapter.*access(Placements{});
    auto& live = fixture.adapter.*access(Live{});
    placements.at(live[1].incarnation) = placements.at(live[0].incarnation);
    if (force_fallback) check_projection(fixture);
    fixture.adapter.fixture_force_pending_projection_fallback(force_fallback);
    check_projection(fixture, force_fallback);
    // A,A,B becomes B,A,A: count is unchanged, but the second selected raw
    // ordinal moves from 2 to 1 and must carry that live handle's incarnation.
    std::swap(live.front(), live.back());
    check_projection(fixture, force_fallback);
}

std::uint64_t measure_size(std::size_t population) {
    Fixture fixture(population);
    const auto expected = independent_projection(fixture.adapter);
    const auto before = fixture.adapter.fixture_pending_projection_operations();
    const int observed = fixture.adapter.pending_intent_view().size();
    const auto operations =
        fixture.adapter.fixture_pending_projection_operations() - before;
    CHECK(observed == static_cast<int>(expected.size()));
    CHECK(operations > 0);

    // One warm indexed read includes both representative lookups (copy_v1
    // and short_seed_collision_role), plus the existing raw sibling walk:
    // 2 * (R validation probes + 1 selected row) + R = 3R + 2 <= 4R.
    const auto before_indexed =
        fixture.adapter.fixture_pending_projection_operations();
    pf_pending_order_v1_t last{};
    const int copied = fixture.adapter.pending_intent_view().copy_v1(
        static_cast<int>(population - 1), &last);
    const auto indexed_operations =
        fixture.adapter.fixture_pending_projection_operations() - before_indexed;
    CHECK(indexed_operations > 0);
    CHECK(indexed_operations <= 4U * population);
    CHECK(copied == 0);
    CHECK(std::string(last.id) == expected.back().snapshot.source_id);
    CHECK(last.incarnation == expected.back().handle.incarnation);
    std::printf("pending-book indexed R=%zu probes=%llu\n", population,
                static_cast<unsigned long long>(indexed_operations));
    check_returned_values(fixture, expected);
    // Exercise fallback and retry after sampling both measured intervals.
    if (population == 64) {
        fixture.adapter.fixture_force_pending_projection_fallback(true);
        check_projection(fixture, true);
        fixture.adapter.fixture_force_pending_projection_fallback(false);
        check_projection(fixture);
    }
    return operations;
}
}

int main() {
    const std::uint64_t operations_16 = measure_size(16);
    const std::uint64_t operations_64 = measure_size(64);
    const std::uint64_t operations_256 = measure_size(256);

    CHECK(operations_64 <= 6U * operations_16);
    CHECK(operations_256 <= 20U * operations_16);
    check_cross_family_projection();
    check_roster_reorder();
    check_cross_family_projection(true);
    check_roster_reorder(true);
    std::printf("s4e-px2-pendingbook reads: ops16=%llu ops64=%llu ops256=%llu "
                "checks=%d failures=%d\n",
                static_cast<unsigned long long>(operations_16),
                static_cast<unsigned long long>(operations_64),
                static_cast<unsigned long long>(operations_256),
                checks, failures);
    return failures == 0 ? 0 : 1;
}
