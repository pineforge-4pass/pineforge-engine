// 1.7.0 pending observer: the old raw walk is the equivalence oracle.
// Direct runs: pending_walk_oracle, pending_walk_cost, pending_walk_growth.
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/compat/pine/trail_ticks.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using PlacementSnapshot = source::PlacementSnapshot;
using PineOrderFamily = source::PineOrderFamily;
using PineCancellationCause = source::PineCancellationCause;
using Handle = native_order::RequestHandle;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) {                  \
    ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n",                      \
                             __FILE__, __LINE__, #expression);              \
} } while (false)

// Same C++17 explicit-instantiation seam as test_l8c_adapter_ordering.cpp.
// No altered class definition, friend added to production, or raw-lookup call.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct SameBar { friend auto access(SameBar); };
struct Entries { friend auto access(Entries); };
struct Brackets { friend auto access(Brackets); };
struct Coof { friend auto access(Coof); };
struct Shadows { friend auto access(Shadows); };
struct Cohorts { friend auto access(Cohorts); };
struct Staged { friend auto access(Staged); };
struct ShortSeed { friend auto access(ShortSeed); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<SameBar, &Adapter::pending_same_bar_commands_>;
template struct Access<Entries, &Adapter::pending_entries_>;
template struct Access<Brackets, &Adapter::pending_bracket_legs_>;
template struct Access<Coof, &Adapter::pending_coof_requests_>;
template struct Access<Shadows, &Adapter::source_shadow_pending_>;
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<Staged, &Adapter::staged_>;
template struct Access<ShortSeed, &Adapter::short_seed_>;

struct RawRow {
    std::optional<PlacementSnapshot> snapshot;
    Handle handle{};
    int owner = 0;
};

std::vector<RawRow> capture_raw(const Adapter& adapter) {
    std::vector<RawRow> rows;
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(Live{})) {
        const auto found = placements.find(handle.incarnation);
        if (found == placements.end()) rows.push_back({std::nullopt, handle, 0});
        else rows.push_back({found->second, handle, 0});
    }
    const auto append = [&](const auto& owner, int number) {
        for (const auto& row : owner) rows.push_back({row.snapshot, {}, number});
    };
    append(adapter.*access(SameBar{}), 1);
    append(adapter.*access(Entries{}), 2);
    append(adapter.*access(Brackets{}), 3);
    append(adapter.*access(Coof{}), 4);
    append(adapter.*access(Shadows{}), 5);
    // Preserve raw positions, including holes, before int-domain clipping.
    if (rows.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        rows.resize(static_cast<std::size_t>(std::numeric_limits<int>::max()));
    return rows;
}

bool is_exit(PineOrderFamily family) {
    switch (family) {
    case PineOrderFamily::ExitLimit:
    case PineOrderFamily::ExitStop:
    case PineOrderFamily::ExitTrail: return true;
    default: return false;
    }
}

bool same_exit(const PlacementSnapshot& a, const PlacementSnapshot& b) {
    // Deliberately independent of same_projected_order, including every
    // component of the origin, not just its incarnation or its run number.
    return is_exit(a.family) && is_exit(b.family)
        && a.source_id == b.source_id && a.from_entry == b.from_entry
        && a.bracket_origin.incarnation == b.bracket_origin.incarnation
        && a.bracket_origin.run.run_number == b.bracket_origin.run.run_number
        && a.bracket_origin.run.session_key == b.bracket_origin.run.session_key;
}

struct CohortSummary {
    bool opened = false;
    double exposure = 0.0;
};
struct OracleFacts {
    double held = 0.0;
    double entry = kNaN;
    double tick = kNaN;
    source::ShortSeedPlan short_seed{};
    std::map<std::string, CohortSummary> cohorts;

    int resolved(const PlacementSnapshot& row) const {
        if (row.from_entry.empty()) return 1;
        const auto found = cohorts.find(row.from_entry);
        return found != cohorts.end() && found->second.opened ? 1 : 0;
    }
    double exposure(const std::string& id) const {
        const auto found = cohorts.find(id);
        return found == cohorts.end() ? 0.0 : found->second.exposure;
    }
    int collision_role(const Handle& handle) const {
        if (!short_seed.active) return 0;
        if (handle == short_seed.long_entry) return 1;
        if (handle == short_seed.materialize_long) return 2;
        if (handle == short_seed.final_short) return 3;
        return 0;
    }
};

OracleFacts capture_facts(const Adapter& adapter,
                          const source::PineStrategyHost& host,
                          const std::vector<RawRow>& rows) {
    OracleFacts facts;
    facts.held = host.physical_position().signed_units;
    facts.entry = host.position_avg_price();
    facts.tick = (adapter.*access(Staged{})).syminfo.mintick;
    facts.short_seed = adapter.*access(ShortSeed{});
    const auto& cohorts = adapter.*access(Cohorts{});
    for (const auto& raw : rows) {
        if (!raw.snapshot || raw.snapshot->from_entry.empty()) continue;
        const auto& id = raw.snapshot->from_entry;
        if (facts.cohorts.count(id) != 0) continue;
        CohortSummary summary;
        const auto found = cohorts.find_resident(id);
        if (found != cohorts.end()) {
            const auto& cohort = found->second;
            summary.opened = !cohort.opened.empty();
            // Traverse retained origins in roster order, independently of
            // the production allocation-free positions-of-minimum walk.
            for (const auto& origin : cohort.origins) {
                const auto units = cohort.live_units_by_origin.find(origin.incarnation);
                if (units != cohort.live_units_by_origin.end()
                    && std::isfinite(units->second) && units->second > 0.0)
                    summary.exposure += units->second;
            }
            if (!std::isfinite(summary.exposure) || !(summary.exposure > 0.0))
                summary.exposure = 0.0;
        }
        facts.cohorts.emplace(id, summary);
    }
    return facts;
}

std::vector<std::size_t> old_walk(const std::vector<RawRow>& rows) {
    std::vector<std::size_t> selected;
    for (std::size_t raw = 0; raw < rows.size(); ++raw) {
        if (!rows[raw].snapshot) continue;
        bool duplicate = false;
        // OLD algorithm: scan earlier raw slots for EVERY valid candidate,
        // including non-exits, and do not compact holes before this walk.
        for (std::size_t prior = 0; prior < raw; ++prior) {
            if (rows[prior].snapshot
                && same_exit(*rows[raw].snapshot, *rows[prior].snapshot)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) selected.push_back(raw);
    }
    return selected;
}

// Mirror string helpers are local to this TU. Hash the entire string even
// when the printable prefix truncates; the final memcmp includes padding.
std::uint64_t append_hash(std::uint64_t hash, std::string_view text) {
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
void copy_pending_string(std::string_view value, char* out,
                         std::uint8_t* truncated, std::uint64_t* hash) {
    *hash = append_hash(1469598103934665603ULL, value);
    const auto length = std::min<std::size_t>(value.size(), 63);
    if (length != 0) std::memcpy(out, value.data(), length);
    out[length] = '\0';
    *truncated = value.size() > length ? 1U : 0U;
}
void copy_pending_prefixed_string(std::string_view prefix, std::string_view value,
                                  char* out, std::uint8_t* truncated,
                                  std::uint64_t* hash) {
    const std::string full = std::string(prefix) + std::string(value);
    copy_pending_string(full, out, truncated, hash);
}
int mirror_order_type(PineOrderFamily family) {
    switch (family) {
    case PineOrderFamily::Entry: return 1;
    case PineOrderFamily::Order: return 3;
    case PineOrderFamily::Close:
    case PineOrderFamily::CloseAll:
    case PineOrderFamily::ExitLimit:
    case PineOrderFamily::ExitStop:
    case PineOrderFamily::ExitTrail: return 2;
    case PineOrderFamily::Margin:
    case PineOrderFamily::Risk: return 0;
    }
    return 0;
}

double old_price_grid(double level, double tick) {
    if (!std::isfinite(level) || !std::isfinite(tick) || !(tick > 0.0))
        return level;
    double scaled = tick;
    double scale = 1.0;
    for (int digits = 0; digits <= 10; ++digits) {
        const double rounded = std::floor(scaled + 0.5);
        if (rounded >= 1.0 && std::abs(scaled - rounded) <= 1e-6 * rounded) {
            const double points = level / (1.0 / scale);
            const double grid = std::floor(points + 0.5);
            return std::abs(points - grid) <= 0.01 / scale ? grid / scale : level;
        }
        scaled *= 10.0;
        scale *= 10.0;
    }
    return level;
}

struct Levels { double stop = kNaN, limit = kNaN, trail = kNaN; };
Levels old_levels(const RawRow& selected, const std::vector<RawRow>& rows,
                  const OracleFacts& facts) {
    const auto& snapshot = *selected.snapshot;
    Levels out{snapshot.exit_levels.stop, snapshot.exit_levels.limit, kNaN};
    double points = snapshot.exit_levels.trail_points;
    double price = snapshot.exit_levels.trail_price;
    double profit = snapshot.exit_levels.profit_ticks;
    double loss = snapshot.exit_levels.loss_ticks;
    // OLD sibling walk also runs for a selected non-exit.
    for (const auto& raw : rows) {
        if (!raw.snapshot || !same_exit(snapshot, *raw.snapshot)) continue;
        const auto& levels = raw.snapshot->exit_levels;
        if (!std::isnan(levels.stop)) out.stop = levels.stop;
        if (!std::isnan(levels.limit)) out.limit = levels.limit;
        if (!std::isnan(levels.trail_points)) points = levels.trail_points;
        if (!std::isnan(levels.trail_price)) price = levels.trail_price;
        if (!std::isnan(levels.profit_ticks)) profit = levels.profit_ticks;
        if (!std::isnan(levels.loss_ticks)) loss = levels.loss_ticks;
    }
    const bool resolved = facts.held != 0.0 && std::isfinite(facts.entry)
        && facts.resolved(snapshot) == 1;
    const double direction = facts.held > 0.0 ? 1.0 : -1.0;
    if (is_exit(snapshot.family) && resolved) {
        if (std::isnan(out.limit) && !std::isnan(profit))
            out.limit = old_price_grid(facts.entry + direction * profit * facts.tick,
                                      facts.tick);
        if (std::isnan(out.stop) && !std::isnan(loss))
            out.stop = old_price_grid(facts.entry - direction * loss * facts.tick,
                                     facts.tick);
    }
    if (!std::isnan(points)) {
        if (resolved) {
            // The line starts with TrailPointsMintickTolerance enabled;
            // these cases do not toggle that policy. Recompute its arithmetic
            // from the snapshot and tick, including the invalid-tick fallback.
            const double tolerance = facts.tick > 0.0 && std::isfinite(facts.tick)
                ? compat::pine::kTrailPointsCeilMintickScale * facts.tick
                : compat::pine::kTrailPointsCeilEps;
            const double ticks = std::ceil(points - tolerance);
            out.trail = compat::pine::snap_trail_level_to_tick_grid(
                facts.entry + direction * ticks * facts.tick, facts.tick);
        }
    } else {
        out.trail = price;
    }
    return out;
}

int serialize_old(const RawRow&, const std::vector<RawRow>&,
                  const OracleFacts&, pf_pending_order_v1_t*);

struct ExpectedRow {
    pf_pending_order_v1_t pod{};
    Levels levels;
    int resolved = -1;
};
std::vector<ExpectedRow> expected_rows(const std::vector<RawRow>& rows,
                                      const OracleFacts& facts) {
    const auto selected = old_walk(rows);
    std::vector<ExpectedRow> expected(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
        CHECK(serialize_old(rows[selected[i]], rows, facts, &expected[i].pod) == 0);
        expected[i].levels = old_levels(rows[selected[i]], rows, facts);
        expected[i].resolved = facts.resolved(*rows[selected[i]].snapshot);
    }
    return expected;
}

bool same_double(const double& a, const double& b) {
    // Compare representations, including NaN payloads and signed zero.
    return std::memcmp(&a, &b, sizeof a) == 0;
}
void check_projection(const Adapter& adapter, const source::PineStrategyHost& host) {
    const auto raw = capture_raw(adapter);
    const auto facts = capture_facts(adapter, host, raw);
    const auto expected = expected_rows(raw, facts);
    const auto& view = adapter.pending_intent_view();
    CHECK(view.size() == static_cast<int>(expected.size()));
    // Reverse then forward exercises every index and repeated calls without
    // giving the implementation a monotone position to reuse.
    for (const bool reverse : {true, false}) {
        for (std::size_t n = 0; n < expected.size(); ++n) {
            const int i = static_cast<int>(reverse ? expected.size() - 1 - n : n);
            const auto& want = expected[static_cast<std::size_t>(i)];
            pf_pending_order_v1_t got{};
            CHECK(view.copy_v1(i, &got) == 0);
            CHECK(std::memcmp(&got, &want.pod, sizeof got) == 0);
            Levels levels;
            CHECK(view.effective_levels(i, &levels.stop, &levels.limit,
                                        &levels.trail) == 0);
            CHECK(same_double(levels.stop, want.levels.stop));
            CHECK(same_double(levels.limit, want.levels.limit));
            CHECK(same_double(levels.trail, want.levels.trail));
            CHECK(view.level_resolved(i) == want.resolved);
        }
    }
    const int count = static_cast<int>(expected.size());
    for (const int i : {-1, count, count + 1, std::numeric_limits<int>::max()}) {
        pf_pending_order_v1_t got{};
        Levels levels;
        CHECK(view.copy_v1(i, &got) == -1);
        CHECK(view.effective_levels(i, &levels.stop, &levels.limit, &levels.trail) == -1);
        CHECK(view.level_resolved(i) == -1);
    }
    CHECK(view.copy_v1(0, nullptr) == -1);
}

Handle handle(std::uint64_t incarnation, const char* session = "pending-walk",
              std::uint64_t run = 1) {
    Handle out;
    out.run.session_key = session;
    out.run.run_number = run;
    out.incarnation = incarnation;
    return out;
}

PlacementSnapshot make_row(PineOrderFamily family, std::size_t serial) {
    PlacementSnapshot row;
    row.family = family;
    row.source_id = "row-" + std::to_string(serial);
    row.opening = family == PineOrderFamily::Entry || family == PineOrderFamily::Order;
    row.is_long = serial % 2 == 0;
    row.requested_qty = 1.0 + static_cast<double>(serial % 7);
    row.projection_remaining_qty = serial % 2 == 0 ? row.requested_qty / 2.0 : kNaN;
    row.qty_type = static_cast<int>(QtyType::FIXED);
    row.qty_percent = row.opening ? kNaN : 25.0;
    row.comment = std::string(72, 'c') + std::to_string(serial);
    row.oca_name = "oca-" + std::to_string(serial % 3);
    row.oca_type = static_cast<int>(serial % 3);
    row.projection_created_bar = static_cast<int>(serial % 11);
    row.source_sequence = serial + 1;
    row.command_sequence = serial + 1;
    row.projection_position_side = static_cast<std::int32_t>(
        serial % 2 == 0 ? PositionSide::LONG : PositionSide::SHORT);
    row.projection_predecessor = serial % 4 == 0 ? serial + 3 : 0;
    row.projection_predecessor_market = serial % 3 == 0;
    row.projection_predecessor_exit = serial % 5 == 0;
    row.sizing.equity = 10'000.0 + static_cast<double>(serial);
    row.sizing.price = 100.0;
    row.sizing.fx = 1.0;
    row.sizing.mark = 100.0;
    row.sizing.frozen_units = row.requested_qty;
    if (is_exit(family)) {
        row.from_entry = "held";
        row.bracket_origin = handle(900, "origin", 7);
        row.deferred_cohort = true;
    }
    if (family == PineOrderFamily::ExitStop) row.exit_levels.stop = 50.0 + serial % 9;
    else if (family == PineOrderFamily::ExitTrail) {
        row.exit_levels.trail_points = 8.25 + static_cast<double>(serial % 4);
        row.exit_levels.trail_offset = 2.0 + static_cast<double>(serial % 3);
    } else row.exit_levels.limit = 150.0 + static_cast<double>(serial % 9);
    return row;
}
PlacementSnapshot exit_leg(PineOrderFamily family, const std::string& id,
                           std::size_t serial) {
    auto row = make_row(family, serial);
    row.source_id = id;
    return row;
}

// Test-owned adapter bound to a real, running source host. Its synthetic
// queues never reach matching or settlement and die before the callback ends.
// This supplements the public command population; it does not replace it.
class Fixture {
public:
    Fixture(source::PineStrategyHost& host, const Adapter& actual)
        : adapter(host) {
        adapter.fixture_use_memory_auxiliary_tables();
        adapter.set_staged_configuration(actual.*access(Staged{}));
        auto& cohorts = adapter.*access(Cohorts{});
        auto& held = cohorts.emplace(
            "held", decltype(cohorts.begin()->second){}).first->second;
        held.origins.push_back(handle(500));
        held.origins.push_back(handle(501));
        held.origins.push_back(handle(500));
        held.opened.push_back(handle(500));
        held.live_units_by_origin[500] = 1.25;
        held.live_units_by_origin[501] = 0.5;
    }
    Handle live(const PlacementSnapshot& snapshot, bool hole = false) {
        const auto h = handle(next_incarnation_++);
        auto& placements = adapter.*access(Placements{});
        placements.try_emplace(h.incarnation, snapshot);
        (adapter.*access(Live{})).push_back(h);
        if (hole) CHECK(placements.erase(h.incarnation));
        return h;
    }
    template<class Tag> void queued(const PlacementSnapshot& snapshot) {
        auto& rows = adapter.*access(Tag{});
        rows.emplace_back();
        rows.back().snapshot = snapshot;
    }
    void put(int owner, const PlacementSnapshot& snapshot, bool hole = false) {
        switch (owner) {
        case 0: live(snapshot, hole); break;
        case 1: queued<SameBar>(snapshot); break;
        case 2: queued<Entries>(snapshot); break;
        case 3: queued<Brackets>(snapshot); break;
        case 4: queued<Coof>(snapshot); break;
        case 5: queued<Shadows>(snapshot); break;
        }
    }
    void hostile(std::size_t q) {
        live(exit_leg(PineOrderFamily::ExitLimit, "hole-first", 0), true);
        const auto first_valid = live(exit_leg(PineOrderFamily::ExitStop, "hole-first", 1));
        const auto entry = make_row(PineOrderFamily::Entry, 2);
        const auto entry_handle = live(entry);
        (adapter.*access(Live{})).push_back(entry_handle); // identical non-exit alias
        const auto group = exit_leg(PineOrderFamily::ExitLimit, "cross-owner", 4);
        const auto group_handle = live(group);
        (adapter.*access(Live{})).push_back(group_handle); // exit alias DOES merge
        queued<SameBar>(exit_leg(PineOrderFamily::ExitTrail, "hole-first", 6));
        queued<SameBar>(entry); // identical snapshot in another owner must not merge
        queued<Entries>(exit_leg(PineOrderFamily::ExitStop, "cross-owner", 8));
        auto order = entry;
        order.family = PineOrderFamily::Order;
        queued<Entries>(order);
        queued<Brackets>(exit_leg(PineOrderFamily::ExitTrail, "cross-owner", 10));
        auto variant = group;
        variant.bracket_origin.run.session_key = "different-origin-session";
        queued<Brackets>(variant);
        variant = group;
        ++variant.bracket_origin.run.run_number;
        queued<Coof>(variant);
        variant = group;
        ++variant.bracket_origin.incarnation;
        queued<Coof>(variant);
        variant = group;
        variant.from_entry = "missing";
        queued<Shadows>(variant);
        variant = group;
        variant.source_id += "-different";
        queued<Shadows>(variant);
        queued<Shadows>(order);
        for (std::size_t n = 17; n < q; ++n) {
            const PineOrderFamily families[] = {
                PineOrderFamily::Entry, PineOrderFamily::Order,
                PineOrderFamily::ExitLimit, PineOrderFamily::ExitStop,
                PineOrderFamily::ExitTrail};
            auto row = make_row(families[n % 5], n);
            if (is_exit(row.family)) {
                row.source_id = std::string(70, 'x') + std::to_string(n / 12);
                if (n % 7 == 0) row.from_entry.clear();
                if (n % 11 == 0) row.from_entry = "missing";
                if (n % 13 == 0) {
                    row.exit_levels = {};
                    row.exit_levels.profit_ticks = 11.25;
                    row.exit_levels.loss_ticks = 7.25;
                    row.exit_levels.trail_price = 123.25;
                }
            }
            put(static_cast<int>(n % 6), row, n % 13 == 0);
        }
        auto& plan = adapter.*access(ShortSeed{});
        plan.active = true;
        plan.long_entry = entry_handle;
        plan.materialize_long = group_handle;
        plan.final_short = first_valid;
    }
    void cost(std::size_t q) {
        // Fixed exit prefix, then growing non-exits. This specifically
        // measures the removable non-exit prior scans without claiming a
        // bound for a growing population of distinct exit groups.
        const PineOrderFamily families[] = {PineOrderFamily::ExitLimit,
            PineOrderFamily::ExitStop, PineOrderFamily::ExitTrail};
        for (std::size_t group = 0; group < 4; ++group)
            for (std::size_t leg = 0; leg < 3; ++leg)
                live(exit_leg(families[leg], "cost-exit-" + std::to_string(group),
                              3 * group + leg));
        for (std::size_t n = 12; n < q; ++n) {
            const auto family = n % 2 == 0 ? PineOrderFamily::Entry : PineOrderFamily::Order;
            put(1 + static_cast<int>((n - 12) % 5), make_row(family, n));
        }
    }
    Adapter adapter;
private:
    std::uint64_t next_incarnation_ = 1;
};

void check_hostile_coverage(const std::vector<RawRow>& rows, std::size_t q) {
    CHECK(rows.size() == q);
    std::array<bool, 6> owners{};
    bool hole = false, entry = false, order = false;
    bool limit = false, stop = false, trail = false;
    bool nonexit_alias = false, exit_alias = false, cross_owner_group = false;
    const auto selected = old_walk(rows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& a = rows[i];
        owners[static_cast<std::size_t>(a.owner)] = true;
        if (!a.snapshot) { hole = true; continue; }
        const auto family = a.snapshot->family;
        entry |= family == PineOrderFamily::Entry;
        order |= family == PineOrderFamily::Order;
        limit |= family == PineOrderFamily::ExitLimit;
        stop |= family == PineOrderFamily::ExitStop;
        trail |= family == PineOrderFamily::ExitTrail;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& b = rows[j];
            if (!b.snapshot) continue;
            const bool same_handle = a.owner == 0 && b.owner == 0 && a.handle == b.handle;
            if (same_handle && !is_exit(family)) {
                nonexit_alias = true;
                CHECK(std::find(selected.begin(), selected.end(), i) != selected.end());
                CHECK(std::find(selected.begin(), selected.end(), j) != selected.end());
            }
            exit_alias |= same_handle && same_exit(*a.snapshot, *b.snapshot);
            cross_owner_group |= a.owner != b.owner && same_exit(*a.snapshot, *b.snapshot);
        }
    }
    CHECK(std::all_of(owners.begin(), owners.end(), [](bool used) { return used; }));
    CHECK(hole && entry && order && limit && stop && trail);
    CHECK(nonexit_alias && exit_alias && cross_owner_group);
}

struct Models {
    long double old = 0, port = 0, mutation = 0;
};
Models operation_models(const std::vector<RawRow>& rows) {
    long double old_size = 0, port_size = 0, old_lookups = 0, port_lookups = 0;
    std::size_t count = 0, exits = 0;
    for (std::size_t raw = 0; raw < rows.size(); ++raw) {
        ++old_size;
        ++port_size;
        if (!rows[raw].snapshot) continue;
        bool duplicate = false;
        std::size_t prior_reads = 0;
        for (std::size_t prior = 0; prior < raw; ++prior) {
            ++prior_reads;
            if (rows[prior].snapshot
                && same_exit(*rows[raw].snapshot, *rows[prior].snapshot)) {
                duplicate = true;
                break;
            }
        }
        old_size += prior_reads;
        if (is_exit(rows[raw].snapshot->family)) port_size += prior_reads;
        if (!duplicate) {
            ++count;
            exits += is_exit(rows[raw].snapshot->family) ? 1 : 0;
            old_lookups += old_size;
            port_lookups += port_size;
        }
    }
    // size() is evaluated N+1 times by the literal for condition.
    // copy_v1 looks up its representative twice: once directly, once in
    // short_seed_collision_role. Its old sibling pass visits Q raw slots.
    const long double old_walks = (count + 1) * old_size + 2 * old_lookups;
    const long double port_walks = (count + 1) * port_size + 2 * port_lookups;
    return {old_walks + count * rows.size(),
            port_walks + exits * rows.size(),
            old_walks + exits * rows.size()};
}

using Clock = std::chrono::steady_clock;
static_assert(Clock::is_steady, "pending walk measurement requires a monotonic clock");
constexpr double kMinimumSampleUs = 20'000.0;
struct Cost {
    double pass_us = 0.0, size_us = 0.0, copy_last_us = 0.0;
    double linear_per_row_us = 0.0;
    double minimum_sample_us = 0.0;
    Models model;
};
double elapsed_us(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
std::uint64_t linear_walk(const Adapter& adapter) {
    std::uint64_t sum = 0;
    const auto add = [&](const PlacementSnapshot& snapshot) {
        // Read the actual row on every walk, not an oracle copy or a value
        // the optimizer can hoist out of the repeated timing workload.
        const volatile PineOrderFamily& family = snapshot.family;
        sum += 1 + static_cast<std::uint64_t>(family);
    };
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(Live{})) {
        const auto found = placements.find(handle.incarnation);
        if (found != placements.end()) add(found->second);
    }
    const auto append = [&](const auto& owner) {
        for (const auto& row : owner) add(row.snapshot);
    };
    append(adapter.*access(SameBar{}));
    append(adapter.*access(Entries{}));
    append(adapter.*access(Brackets{}));
    append(adapter.*access(Coof{}));
    append(adapter.*access(Shadows{}));
    return sum;
}
double measure_linear_per_row(const Adapter& adapter, const std::vector<RawRow>& raw) {
    std::uint64_t expected = 0;
    for (const auto& row : raw)
        if (row.snapshot)
            expected += 1 + static_cast<std::uint64_t>(row.snapshot->family);
    double best = std::numeric_limits<double>::infinity();
    for (int trial = 0; trial < 3; ++trial) {
        const auto start = Clock::now();
        std::size_t walks = 0;
        double total = 0.0;
        do {
            CHECK(linear_walk(adapter) == expected);
            ++walks;
            total = elapsed_us(start);
        } while (total < kMinimumSampleUs);
        best = std::min(best, total / static_cast<double>(walks));
    }
    return best / static_cast<double>(raw.size());
}
bool full_pass(const source::PendingIntentView& view,
               const std::vector<ExpectedRow>& expected) {
    int copied = 0;
    for (int i = 0; i < view.size(); ++i) {
        pf_pending_order_v1_t got{};
        if (static_cast<std::size_t>(i) >= expected.size()
            || view.copy_v1(i, &got) != 0
            || std::memcmp(&got, &expected[static_cast<std::size_t>(i)].pod,
                           sizeof got) != 0) {
            CHECK(false);
            return false;
        }
        ++copied;
    }
    CHECK(copied == static_cast<int>(expected.size()));
    return copied == static_cast<int>(expected.size());
}
Cost measure(const Adapter& adapter, const source::PineStrategyHost& host, std::size_t q) {
    const auto raw = capture_raw(adapter);
    CHECK(raw.size() == q);
    const auto facts = capture_facts(adapter, host, raw);
    const auto expected = expected_rows(raw, facts);
    CHECK(!expected.empty());
    Cost result;
    result.model = operation_models(raw);
    if (expected.empty()) return result;
    const auto& view = adapter.pending_intent_view();
    double best = std::numeric_limits<double>::infinity();
    result.minimum_sample_us = std::numeric_limits<double>::infinity();
    // Best of three samples. Each sample is whole full passes repeated
    // until >=20 ms, then normalized per pass; even a sub-ms Q=257 pass has
    // a >=20 ms timing denominator. Setup/oracle/model work is outside.
    for (int trial = 0; trial < 3; ++trial) {
        const auto start = Clock::now();
        std::size_t passes = 0;
        double total = 0.0;
        do {
            if (!full_pass(view, expected)) return result;
            ++passes;
            total = elapsed_us(start);
        } while (total < kMinimumSampleUs);
        best = std::min(best, total / static_cast<double>(passes));
        result.minimum_sample_us = std::min(result.minimum_sample_us, total);
    }
    result.pass_us = best;
    auto start = Clock::now();
    const int size = view.size();
    result.size_us = elapsed_us(start);
    CHECK(size == static_cast<int>(expected.size()));
    pf_pending_order_v1_t last{};
    start = Clock::now();
    const int status = view.copy_v1(static_cast<int>(expected.size() - 1), &last);
    result.copy_last_us = elapsed_us(start);
    CHECK(status == 0);
    CHECK(std::memcmp(&last, &expected.back().pod, sizeof last) == 0);
    result.linear_per_row_us = measure_linear_per_row(adapter, raw);
    std::printf("pending-walk-cost q=%zu pass_us=%.3f size_us=%.3f copy_last_us=%.3f "
                "linear_per_row_us=%.6f\n",
                q, result.pass_us, result.size_us, result.copy_last_us,
                result.linear_per_row_us);
    std::fflush(stdout);
    return result;
}

class WalkHost final : public source::PineStrategyHost {
public:
    WalkHost(std::size_t q, bool timed, bool pooc = false, bool long_side = true,
             bool coof = false)
        : q_(q), timed_(timed), long_side_(long_side) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = static_cast<int>(q + 16);
        config.process_orders_on_close = pooc;
        config.calc_on_order_fills = coof;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(false);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            if (!seeded_) {
                seeded_ = true;
                strategy_entry("held", long_side_, kNaN, kNaN, 1.0);
            }
            return;
        }
        if (pine_bar_index() != 1 || captured) return;
        captured = true;
        CHECK(physical_position().signed_units != 0.0);
        const double limit = long_side_ ? 150.0 : 50.0;
        const double stop = long_side_ ? 50.0 : 150.0;
        strategy_exit("public-X", "held", limit, stop, 8.25, 2.0);
        const std::size_t commands = timed_ ? 2 : q_;
        for (std::size_t n = 0; n < commands; ++n) {
            const std::string id = "public-" + std::to_string(n);
            if (n % 2 == 0)
                strategy_entry(id, long_side_, long_side_ ? 25.0 : 250.0, kNaN, 1.0);
            else strategy_order(id, long_side_, 1.0, long_side_ ? 20.0 : 260.0);
        }
        strategy_exit("public-P", "public-0", limit, stop, 12.25, 3.0);
        strategy_entry("public-batch", long_side_, kNaN, kNaN, 1.0);
        if (!timed_) {
            const auto raw = capture_raw(adapter_);
            bool entry = false, order = false, exit = false;
            std::array<bool, 6> owners{};
            for (const auto& row : raw) {
                owners[static_cast<std::size_t>(row.owner)] = true;
                if (!row.snapshot) continue;
                entry |= row.snapshot->family == PineOrderFamily::Entry;
                order |= row.snapshot->family == PineOrderFamily::Order;
                exit |= is_exit(row.snapshot->family);
            }
            CHECK(entry && order && exit);
            check_projection(adapter_, *this);
            std::printf("pending-walk-public q=%zu pooc=%d coof=%d "
                        "owners=%d%d%d%d%d%d raw=%zu\n", q_,
                        fixture_configuration().process_orders_on_close,
                        fixture_configuration().calc_on_order_fills,
                        owners[0], owners[1], owners[2], owners[3], owners[4], owners[5],
                        raw.size());
        }
        Fixture fixture(*this, adapter_);
        if (timed_) {
            fixture.cost(q_);
            cost = measure(fixture.adapter, *this, q_);
        } else {
            fixture.hostile(q_);
            check_hostile_coverage(capture_raw(fixture.adapter), q_);
            check_projection(fixture.adapter, *this);
            // A new read must observe a changed representative/group key:
            // this catches any cross-call position or count cache.
            auto& live = fixture.adapter.*access(Live{});
            auto& placements = fixture.adapter.*access(Placements{});
            placements.at(live[1].incarnation).source_id += "-rekeyed";
            check_projection(fixture.adapter, *this);
        }
    }
    bool captured = false;
    Cost cost;
private:
    std::size_t q_;
    bool timed_, long_side_, seeded_ = false;
};

void run_host(WalkHost& host) {
    const Bar bars[] = {{100, 100, 100, 100, 1, 1'000},
                        {100, 100, 100, 100, 1, 2'000}};
    host.run(bars, 2, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.captured);
}
void oracle_case() {
    for (const std::size_t q : {17U, 65U, 257U}) {
        WalkHost ordinary(q, false);
        run_host(ordinary);
        WalkHost close_short(q, false, true, false);
        run_host(close_short);
        WalkHost fill_recalc(q, false, false, true, true);
        run_host(fill_recalc);
    }
    source::PendingIntentView empty;
    CHECK(empty.size() == 0);
    pf_pending_order_v1_t row{};
    Levels levels;
    CHECK(empty.copy_v1(0, &row) == -1);
    CHECK(empty.effective_levels(0, &levels.stop, &levels.limit, &levels.trail) == -1);
    CHECK(empty.level_resolved(0) == -1);
}
// Same PINEFORGE_*_SCALING_GATE=0 convention as the P7/V19-E ratio rows.
// CMake disables only the final ratio comparison for non-Release libraries.
bool gated() {
    const char* gate = std::getenv("PINEFORGE_PENDING_WALK_SCALING_GATE");
    return !(gate && std::strcmp(gate, "0") == 0);
}
void cost_case(bool growth) {
    WalkHost small(257, true);
    run_host(small);
    WalkHost large(4096, true);
    run_host(large);
    if (!growth) return;
    // This is a TIMING RATIO, not an observed deterministic fetch count.
    // For Q raw non-exits the prior scans make size/lookup Theta(Q^2);
    // the literal full pass is Theta(Q^3). Skipping those scans makes each
    // call Theta(Q) and the pass Theta(Q^2). With four fixed 3-leg exit
    // groups first, N=Q-8, and the exact raw-fetch models are:
    // port = 2Q^2+184Q-1902;
    // mutation = 5Q^3/6-2Q^2-65Q/6-428;
    // old = mutation + (N-4)Q (the old non-exit sibling scans).
    // operation_models derives these counts by walking the actual snapshots,
    // without fetching production rows. Use their finite-Q growth factors,
    // not rounded asymptotes: Q2/Q1=4096/257 is slightly below 16, so simply
    // sqrt((Q2/Q1)^2 * (Q2/Q1)^3) would give slightly LESS than 4x margin.
    // The geometric mean of port growth and the smaller old/mutation growth
    // gives >4x room on BOTH sides for this fixed-prefix population.
    // Lab raw port growth was 160.5-166.1 versus bound 886.74 and model
    // 193.3; raw old growth was 4,043 (round-0 test) and 4,218 (prior scan
    // restored), versus model 4,068. The raw margins are at least 5.3x on
    // the green side and 4.56x on the red side.
    // Raw fetches do not have equal cost at both sizes: a family-only read
    // strides over full PlacementSnapshot values in the queue elements.
    // Memory-hierarchy cost (cache/TLB/ASan shadow) grows with Q and affects
    // paths differently, explaining why lab raw port growth is below its
    // operation-count model. Linear-walk timings and the normalized ratio
    // are diagnostic only: dividing by that growth erodes the red margin.
    // A host whose memory hierarchy makes the 4096-row port pass >5x worse
    // per row than the 257-row pass, or whose old growth falls below 886.74,
    // can make the gate flap. Enforce it only in Release: Debug assertions
    // and sanitizer instrumentation can distort these timing margins.
    // No measured timing chooses the threshold. Every sample has >=20 ms of
    // complete passes, best of three at both sizes.
    const long double port_growth = large.cost.model.port / small.cost.model.port;
    const long double old_growth = std::min(
        large.cost.model.old / small.cost.model.old,
        large.cost.model.mutation / small.cost.model.mutation);
    const long double threshold = std::sqrt(port_growth * old_growth);
    // Design asserts, not evidence: fixed-population model margins and
    // sample lengths guaranteed by measure()'s do/while loop.
    CHECK(threshold >= 4 * port_growth);
    CHECK(old_growth >= 4 * threshold);
    CHECK(small.cost.minimum_sample_us >= kMinimumSampleUs);
    CHECK(large.cost.minimum_sample_us >= kMinimumSampleUs);
    CHECK(small.cost.pass_us > 0.0 && large.cost.pass_us > 0.0);
    const double raw_observed = large.cost.pass_us / small.cost.pass_us;
    const double linear_per_row_growth =
        large.cost.linear_per_row_us / small.cost.linear_per_row_us;
    const double normalized_observed = raw_observed / linear_per_row_growth;
    std::printf("pending-walk-growth ratio=%.3f bound=%.3Lf "
                "model_port=%.3Lf model_old_min=%.3Lf minimum_sample_us=%.0f "
                "raw_ratio=%.3f linear_per_row_growth=%.3f normalized_ratio=%.3f\n",
                raw_observed, threshold, port_growth, old_growth, kMinimumSampleUs,
                raw_observed, linear_per_row_growth, normalized_observed);
    if (!gated()) {
        std::printf("  (ratio not gated: non-Release library)\n");
        return;
    }
    CHECK(raw_observed <= threshold);
}

// Frozen 14da9f0d82 mirror field mapping. Selection and sibling inputs come
// exclusively from test-owned raw snapshots and old_walk, not the production
// lookup/size/serializer. Keep the whole POD mapping: checking a handful of
// fields would miss a changed representative, handle, lifecycle or padding.
int serialize_old(const RawRow& selected, const std::vector<RawRow>& rows,
                  const OracleFacts& facts, pf_pending_order_v1_t* out) {
    if (!out || !selected.snapshot) return -1;
    const PlacementSnapshot& snapshot = *selected.snapshot;
    const Handle& handle = selected.handle;

    std::memset(out, 0, sizeof(*out));
    out->struct_version = 1;
    out->size = static_cast<std::uint32_t>(sizeof(*out));
    if (snapshot.family == PineOrderFamily::Close) {
        copy_pending_prefixed_string("__close__", snapshot.source_id,
                                     out->id, &out->id_truncated,
                                     &out->id_hash64);
    } else {
        copy_pending_string(snapshot.source_id, out->id,
                            &out->id_truncated, &out->id_hash64);
    }
    copy_pending_string(snapshot.from_entry, out->from_entry, &out->from_entry_truncated,
                        &out->from_entry_hash64);
    copy_pending_string(snapshot.oca_name, out->oca_name, &out->oca_name_truncated,
                        &out->oca_name_hash64);
    copy_pending_string(snapshot.comment, out->comment, &out->comment_truncated,
                        &out->comment_hash64);
    out->type = mirror_order_type(snapshot.family);
    out->is_long = snapshot.is_long ? 1U : 0U;
    out->limit_price = snapshot.exit_levels.limit;
    out->stop_price = snapshot.exit_levels.stop;
    out->trail_points = snapshot.exit_levels.trail_points;
    out->trail_price = snapshot.exit_levels.trail_price;
    out->trail_offset = snapshot.exit_levels.trail_offset;
    out->profit_ticks = snapshot.exit_levels.profit_ticks;
    out->loss_ticks = snapshot.exit_levels.loss_ticks;
    for (const auto& raw : rows) {
        const PlacementSnapshot* sibling = raw.snapshot ? &*raw.snapshot : nullptr;
        if (!sibling || !same_exit(snapshot, *sibling)) continue;
        if (!std::isnan(sibling->exit_levels.limit))
            out->limit_price = sibling->exit_levels.limit;
        if (!std::isnan(sibling->exit_levels.stop))
            out->stop_price = sibling->exit_levels.stop;
        if (!std::isnan(sibling->exit_levels.trail_points))
            out->trail_points = sibling->exit_levels.trail_points;
        if (!std::isnan(sibling->exit_levels.trail_price))
            out->trail_price = sibling->exit_levels.trail_price;
        if (!std::isnan(sibling->exit_levels.trail_offset))
            out->trail_offset = sibling->exit_levels.trail_offset;
        if (!std::isnan(sibling->exit_levels.profit_ticks))
            out->profit_ticks = sibling->exit_levels.profit_ticks;
        if (!std::isnan(sibling->exit_levels.loss_ticks))
            out->loss_ticks = sibling->exit_levels.loss_ticks;
    }
    out->qty = snapshot.family == PineOrderFamily::Close
        ? snapshot.requested_qty
        : (std::isfinite(snapshot.projection_remaining_qty)
            ? snapshot.projection_remaining_qty : snapshot.requested_qty);
    out->qty_type = snapshot.qty_type;
    out->qty_percent = snapshot.qty_percent;
    out->oca_type = snapshot.oca_type;
    out->created_bar = snapshot.projection_created_bar;
    out->created_seq = static_cast<std::int64_t>(snapshot.source_sequence);
    out->incarnation = handle.incarnation;
    out->created_by_same_id_replacement = snapshot.projection_predecessor != 0
        && snapshot.family != PineOrderFamily::Order ? 1U : 0U;
    out->replaced_default_market_incarnation = snapshot.projection_predecessor_market
        ? snapshot.projection_predecessor : 0;
    out->declined_by_replaced_short_market = snapshot.cancellation.cause
        == PineCancellationCause::Replacement ? 1U : 0U;
    out->replaced_exit_order_incarnation = snapshot.projection_predecessor_exit
        ? snapshot.projection_predecessor : 0;
    out->recreated_after_named_cancelled_entry_incarnation =
        snapshot.recreated_after_named_cancelled_entry_incarnation;
    out->named_cancel_surviving_exit_incarnation = snapshot.named_cancel_surviving_exit_incarnation;
    out->stop_limit_activated = snapshot.stop_limit_activated ? 1U : 0U;
    out->coof_suppress_stop_on_entry_bar = snapshot.exit_activation.holds_stop() ? 1U : 0U;
    out->coof_suppress_limit_on_entry_bar = snapshot.exit_activation.holds_limit() ? 1U : 0U;
    out->created_during_coof_recalc = snapshot.birth.from_fill() ? 1U : 0U;
    out->coof_born_at_close_recalc = snapshot.birth.at_terminal_fill() ? 1U : 0U;
    out->coof_born_mid_bar = compat::pine::historical_cascade_reach(snapshot.birth_reach) ? 1U : 0U;
    out->coof_cascade_seg_i = snapshot.coof_cascade_seg_i;
    out->coof_cascade_inflight_fires = snapshot.coof_cascade_inflight_fires ? 1U : 0U;
    out->created_position_side = snapshot.projection_position_side;
    out->created_position_cycle_seq = snapshot.placement_cycle;
    out->created_after_position_close_in_bar = snapshot.projection_after_close ? 1U : 0U;
    out->over_pyramiding_cap_at_placement = snapshot.projection_over_pyramiding ? 1U : 0U;
    out->same_id_stop_deferred_close_all_bar = snapshot.cancellation.cause
        == PineCancellationCause::Dependency ? snapshot.projection_created_bar : -1;
    out->same_id_stop_deferred_close_all_incarnation = snapshot.cancellation.cause
        == PineCancellationCause::Dependency ? snapshot.cancellation.target_incarnation : 0;
    out->reverses_same_bar_market_from_flat =
        snapshot.projection_opposite_market_predecessor ? 1U : 0U;
    out->paired_flat_market_candidate = snapshot.paired_flat_market_candidate ? 1U : 0U;
    out->paired_flat_market_own_qty = snapshot.paired_flat_market_own_qty;
    out->paired_flat_market_signal_close = snapshot.paired_flat_market_signal_close;
    out->paired_flat_market_signal_equity = snapshot.paired_flat_market_signal_equity;
    out->paired_flat_market_signal_margin_pct = snapshot.paired_flat_market_signal_margin_pct;
    out->paired_flat_market_signal_pointvalue = snapshot.paired_flat_market_signal_pointvalue;
    out->paired_flat_market_signal_fx = snapshot.paired_flat_market_signal_fx;
    out->paired_flat_market_peer_seq = snapshot.paired_flat_market_peer_seq;
    out->paired_flat_market_transaction_qty = snapshot.paired_flat_market_transaction_qty;
    out->default_flat_market_gross_candidate = snapshot.paired_flat_market_candidate
        && !std::isfinite(snapshot.requested_qty) ? 1U : 0U;
    out->tv_carry_qty = snapshot.projection_tv_carry_qty;
    out->frozen_default_qty = snapshot.sizing.frozen_units;
    out->default_stop_placement_qty = snapshot.sizing.frozen_units;
    out->default_stop_placement_equity = snapshot.projection_default_stop_equity;
    out->default_stop_placement_signal_close = snapshot.projection_default_stop_signal_close;
    out->default_stop_sizing_price = snapshot.sizing.price;
    out->sizing_equity = snapshot.sizing.equity;
    out->sizing_price = snapshot.sizing.price;
    out->sizing_fx = snapshot.sizing.fx;
    out->sizing_mark = snapshot.sizing.mark;
    out->opening_affordability_exemption_candidate = snapshot.opening
        && !std::isfinite(snapshot.requested_qty)
        && snapshot.projection_position_side == static_cast<std::int32_t>(PositionSide::FLAT) ? 1U : 0U;
    out->explicit_flat_admission_candidate = snapshot.opening
        && std::isfinite(snapshot.requested_qty)
        && snapshot.projection_position_side == static_cast<std::int32_t>(PositionSide::FLAT) ? 1U : 0U;
    out->explicit_placement_equity = snapshot.projection_explicit_equity;
    out->explicit_slipped_signal_close = snapshot.projection_explicit_signal_close;
    out->affordability_placement_equity = snapshot.projection_affordability_equity;
    out->affordability_signal_price = snapshot.projection_affordability_signal_price;
    out->affordability_held_qty = snapshot.projection_affordability_held_qty;
    out->affordability_close_only = snapshot.affordability_close_only ? 1U : 0U;
    out->rounded_signal_cost_close_only = snapshot.rounded_signal_cost_close_only ? 1U : 0U;
    // The source command boundary itself is the truthful placement
    // observation after the legacy admission draft owner was retired.  Its
    // original sizing tuple is the immutable adapter snapshot captured by
    // entry()/order(); no allocation or reconstructed executable book is
    // involved in this projection.
    out->market_admission_observation_present = snapshot.opening ? 1U : 0U;
    out->market_admission_observation_original_sizing_present =
        snapshot.opening ? 1U : 0U;
    out->market_admission_observation_requested_quantity = snapshot.requested_qty;
    out->market_admission_observation_quantity_type = snapshot.qty_type;
    out->market_admission_observation_buy = snapshot.is_long ? 1U : 0U;
    out->market_admission_observation_prices_limit = snapshot.exit_levels.limit;
    out->market_admission_observation_prices_stop = snapshot.exit_levels.stop;
    copy_pending_string(snapshot.source_id,
                        out->market_admission_observation_id,
                        &out->market_admission_observation_id_truncated,
                        &out->market_admission_observation_id_hash64);
    copy_pending_string(snapshot.oca_name,
                        out->market_admission_observation_oca_name,
                        &out->market_admission_observation_oca_name_truncated,
                        &out->market_admission_observation_oca_name_hash64);
    out->market_admission_observation_placement_equity = snapshot.sizing.equity;
    out->market_admission_observation_signal_close = snapshot.sizing.price;
    out->market_admission_observation_original_sizing_quantity =
        snapshot.requested_qty;
    out->market_admission_observation_original_sizing_equity = snapshot.sizing.equity;
    out->market_admission_observation_original_sizing_price = snapshot.sizing.price;
    out->market_admission_observation_original_sizing_mark = snapshot.sizing.mark;
    out->market_admission_observation_original_sizing_fx = snapshot.sizing.fx;
    out->signal_close_mc_bar = snapshot.signal_close_mc_bar;
    out->signal_close_mc_entry_incarnation = snapshot.signal_close_mc_entry_incarnation;
    out->signal_close_mc_fill_seq = snapshot.signal_close_mc_fill_seq;
    out->signal_close_mc_remaining_qty = snapshot.signal_close_mc_remaining_qty;
    out->requested_partial = (!snapshot.opening && std::isfinite(snapshot.qty_percent)
        && snapshot.qty_percent < 100.0) || (!snapshot.opening
        && std::isfinite(snapshot.requested_qty)) ? 1U : 0U;
    out->full_percent_exit_request = !snapshot.opening && !std::isfinite(snapshot.requested_qty)
        && (!std::isfinite(snapshot.qty_percent) || snapshot.qty_percent == 100.0) ? 1U : 0U;
    out->pooc_global_full_exit_dynamic_qty = snapshot.pooc_global_full_exit_dynamic_qty ? 1U : 0U;
    out->pooc_global_full_exit_tracks_bound_adds = snapshot.pooc_global_full_exit_tracks_bound_adds ? 1U : 0U;
    out->pooc_global_full_exit_bound_add = snapshot.pooc_global_full_exit_bound_add ? 1U : 0U;
    out->created_while_in_position = !snapshot.opening
        && snapshot.projection_position_side != static_cast<std::int32_t>(PositionSide::FLAT)
        ? 1U : 0U;
    out->sbmt_member = snapshot.frozen_market_instruction ? 1U : 0U;
    out->sbmt_own_qty = snapshot.frozen_market_instruction
        ? snapshot.frozen_market_own_units : kNaN;
    out->sbmt_tx_qty = snapshot.frozen_market_instruction
        ? snapshot.frozen_market_transaction_units : kNaN;
    out->sbmt_kept_over_cap = snapshot.frozen_market_instruction
        && snapshot.projection_over_pyramiding ? 1U : 0U;
    out->sbmt_close_qty = snapshot.frozen_market_targeted_close
        ? snapshot.requested_qty : kNaN;
    out->sbmt_close_buy = snapshot.frozen_market_targeted_close
        && snapshot.projection_position_side == static_cast<std::int32_t>(PositionSide::SHORT)
        ? 1U : 0U;
    out->suppress_as_declined_reversal_close = snapshot.cancellation.cause
        == PineCancellationCause::Dependency ? 1U : 0U;
    out->dormant_bracket = snapshot.legs.dormant() ? 1U : 0U;
    out->dormant_reissue_pending = snapshot.legs.pending_replacement() ? 1U : 0U;
    out->dormant_original_stop_price = snapshot.legs.original_stop();
    out->dormant_hold_bar = snapshot.legs.hold_bar();
    out->dormant_reversal_kill_bar = snapshot.legs.excluded_bar();
    out->dormant_trail_best = snapshot.legs.trail_best();
    out->dormant_trail_best_start = snapshot.legs.trail_prefix();
    out->dormant_trail_leg_dead = snapshot.legs.retired(exit_legs::Leg::Trail) ? 1U : 0U;
    out->suppressed_close_consumed_ledger_qty = snapshot.cancellation.close_claim_consumed;
    out->suppressed_close_retired_ledger_qty = snapshot.cancellation.close_claim_retired;
    out->short_seed_collision_role = facts.collision_role(handle);
    out->replaced_order_incarnation = snapshot.projection_predecessor;
    out->birth_timestamp = snapshot.birth.timestamp();
    out->birth_cause = static_cast<std::int32_t>(snapshot.birth.cause());
    out->birth_bar = snapshot.birth.bar();
    out->birth_cursor_domain = static_cast<std::int32_t>(snapshot.birth.cursor().domain());
    out->birth_cursor_position = static_cast<std::int32_t>(snapshot.birth.cursor().position());
    out->birth_cursor_index = snapshot.birth.cursor().index();
    out->birth_cursor_count = snapshot.birth.cursor().count();
    out->birth_cursor_price = snapshot.birth.cursor_price();
    out->birth_first_fill = snapshot.birth.first_fill();
    out->birth_last_fill = snapshot.birth.last_fill();
    out->birth_evaluation_ordinal = snapshot.birth.evaluation_ordinal();
    out->pine_birth_reach = static_cast<std::int32_t>(snapshot.birth_reach);
    out->pine_frozen_market_instruction_kind = snapshot.frozen_market_instruction ? 1U : 0U;
    out->pine_frozen_market_instruction_own_units = snapshot.frozen_market_own_units;
    out->pine_frozen_market_instruction_transaction_units =
        snapshot.frozen_market_transaction_units;
    copy_pending_string(snapshot.frozen_market_targeted_close
                            ? std::string_view(snapshot.source_id) : std::string_view{},
                        out->pine_frozen_market_instruction_target_id,
                        &out->pine_frozen_market_instruction_target_id_truncated,
                        &out->pine_frozen_market_instruction_target_id_hash64);
    const bool explicit_units = std::isfinite(snapshot.requested_qty);
    const bool percentage = !explicit_units && std::isfinite(snapshot.qty_percent);
    out->quantity_intent_kind = explicit_units ? 2U : (percentage ? 3U : 1U);
    out->quantity_intent_units = explicit_units ? snapshot.requested_qty : 0.0;
    out->quantity_intent_numerator = percentage ? snapshot.qty_percent : 0.0;
    out->quantity_intent_denominator = percentage ? 100.0 : 0.0;
    const double exposure = snapshot.from_entry.empty() ? 0.0
        : facts.exposure(snapshot.from_entry);
    out->quantity_reservation_present = snapshot.deferred_cohort && exposure > 0.0 ? 1U : 0U;
    out->quantity_reservation_units = out->quantity_reservation_present ? exposure : 0.0;
    out->quantity_reservation_basis_units = out->quantity_reservation_present ? exposure : 0.0;
    out->leg_activation_present = snapshot.leg_activation.bounds() ? 1U : 0U;
    out->leg_activation_owner_cycle = snapshot.leg_activation.bounds()
        ? snapshot.leg_activation.bounds()->position_cycle : 0;
    out->leg_activation_stop_first_bar = snapshot.leg_activation.bounds()
        ? snapshot.leg_activation.bounds()->stop_first_bar : 0;
    out->leg_activation_limit_first_bar = snapshot.leg_activation.bounds()
        ? snapshot.leg_activation.bounds()->limit_first_bar : 0;
    const auto& activation = snapshot.exit_activation.evidence();
    out->pine_exit_activation_present = activation ? 1U : 0U;
    out->pine_exit_activation_owner_cycle_at_birth = activation ? activation->position_cycle : 0;
    out->pine_exit_activation_entry_bar_at_birth = activation ? activation->entry_bar : 0;
    out->pine_exit_activation_direction_at_birth = activation ? activation->direction : 0;
    out->pine_exit_activation_cursor_price_at_birth = activation ? activation->cursor_price : 0.0;
    out->pine_exit_activation_stop_level_at_birth = activation ? activation->stop_level : 0.0;
    out->pine_exit_activation_limit_level_at_birth = activation ? activation->limit_level : 0.0;
    out->pine_exit_activation_limit_continuation_present = activation
        && activation->limit_continuation ? 1U : 0U;
    out->pine_exit_activation_limit_continuation_cause = activation
        && activation->limit_continuation
        ? static_cast<std::int32_t>(activation->limit_continuation->cause) : 0;
    out->pine_exit_activation_limit_continuation_fill = activation
        && activation->limit_continuation
        ? activation->limit_continuation->observed_fill_sequence : 0;
    const auto& expansion = snapshot.reservation_expansion.capture();
    out->reservation_expansion_present = expansion ? 1U : 0U;
    out->reservation_expansion_position_cycle = expansion ? expansion->position_cycle : 0;
    out->reservation_expansion_side = expansion ? static_cast<std::int32_t>(expansion->side) : 0;
    out->reservation_expansion_first_later_admission_present = expansion
        && expansion->first_later_admission ? 1U : 0U;
    out->reservation_expansion_first_later_admission = expansion
        && expansion->first_later_admission ? *expansion->first_later_admission : 0;
    out->reservation_growth_source_present = snapshot.reservation_growth_source.reservation_owner()
        ? 1U : 0U;
    out->reservation_growth_source_reservation_owner = snapshot.reservation_growth_source.reservation_owner()
        ? *snapshot.reservation_growth_source.reservation_owner() : 0;

    const auto target = snapshot.legs.target();
    const auto& definition = snapshot.legs.current_definition();
    out->legs_target_incarnation = target.incarnation;
    out->legs_target_owner = target.owner;
    out->legs_revision = snapshot.legs.revision();
    out->legs_definition_incarnation = definition.incarnation();
    out->legs_definition_revision = definition.revision();
    out->legs_definition_value_present = definition.has_value() ? 1U : 0U;
    out->legs_definition_limit_price = definition.has_value() ? definition.prices().limit_price : kNaN;
    out->legs_definition_stop_price = definition.has_value() ? definition.prices().stop_price : kNaN;
    out->legs_definition_trail_points = definition.has_value() ? definition.prices().trail_points : kNaN;
    out->legs_definition_trail_price = definition.has_value() ? definition.prices().trail_price : kNaN;
    out->legs_definition_trail_offset = definition.has_value() ? definition.prices().trail_offset : kNaN;
    out->legs_definition_profit_ticks = definition.has_value() ? definition.prices().profit_ticks : kNaN;
    out->legs_definition_loss_ticks = definition.has_value() ? definition.prices().loss_ticks : kNaN;
    const auto& retirements = snapshot.legs.retirements();
    const auto copy_retirement = [&](std::size_t number, std::uint64_t& generation,
                                     std::uint8_t& present, std::uint64_t& receipt_generation,
                                     std::uint64_t& event, std::int64_t& bar,
                                     std::uint32_t& domain, std::uint32_t& phase) {
        const auto leg = static_cast<exit_legs::Leg>(number);
        generation = snapshot.legs.generation(leg);
        const auto& receipt = retirements[number];
        present = receipt ? 1U : 0U;
        receipt_generation = receipt ? receipt->generation : 0;
        event = receipt ? receipt->cause.event : 0;
        bar = receipt ? receipt->cause.bar : 0;
        domain = receipt ? static_cast<std::uint32_t>(receipt->cause.domain) : 0U;
        phase = receipt ? static_cast<std::uint32_t>(receipt->cause.phase) : 0U;
    };
    copy_retirement(0, out->legs_generation0, out->legs_retirement0_present,
                    out->legs_retirement0_generation, out->legs_retirement0_cause_event,
                    out->legs_retirement0_cause_bar, out->legs_retirement0_cause_domain,
                    out->legs_retirement0_cause_phase);
    copy_retirement(1, out->legs_generation1, out->legs_retirement1_present,
                    out->legs_retirement1_generation, out->legs_retirement1_cause_event,
                    out->legs_retirement1_cause_bar, out->legs_retirement1_cause_domain,
                    out->legs_retirement1_cause_phase);
    copy_retirement(2, out->legs_generation2, out->legs_retirement2_present,
                    out->legs_retirement2_generation, out->legs_retirement2_cause_event,
                    out->legs_retirement2_cause_bar, out->legs_retirement2_cause_domain,
                    out->legs_retirement2_cause_phase);
    const auto& suspension = snapshot.legs.suspension();
    out->legs_suspension_present = suspension ? 1U : 0U;
    out->legs_suspension_cause_event = suspension ? suspension->cause.event : 0;
    out->legs_suspension_cause_bar = suspension ? suspension->cause.bar : 0;
    out->legs_suspension_cause_domain = suspension
        ? static_cast<std::uint32_t>(suspension->cause.domain) : 0U;
    out->legs_suspension_cause_phase = suspension
        ? static_cast<std::uint32_t>(suspension->cause.phase) : 0U;
    out->legs_suspension_legs_count = suspension
        ? static_cast<std::uint32_t>(suspension->legs.size()) : 0U;
    const auto suspended_leg = [&](std::size_t number) -> std::uint32_t {
        return suspension && suspension->legs.size() > number
            ? static_cast<std::uint32_t>(suspension->legs[number]) : UINT32_MAX;
    };
    out->legs_suspension_legs_item0 = suspended_leg(0);
    out->legs_suspension_legs_item1 = suspended_leg(1);
    out->legs_suspension_legs_item2 = suspended_leg(2);
    out->legs_suspension_hold_present = suspension && suspension->hold ? 1U : 0U;
    if (suspension && suspension->hold) {
        const auto& hold = *suspension->hold;
        out->legs_suspension_hold_requested_event = hold.requested.event;
        out->legs_suspension_hold_requested_bar = hold.requested.bar;
        out->legs_suspension_hold_requested_domain = static_cast<std::uint32_t>(hold.requested.domain);
        out->legs_suspension_hold_requested_phase = static_cast<std::uint32_t>(hold.requested.phase);
        out->legs_suspension_hold_target_incarnation = hold.target.incarnation;
        out->legs_suspension_hold_target_owner = hold.target.owner;
        out->legs_suspension_hold_revision = hold.revision;
    }
    out->legs_suspension_revival_definition_limit_price = kNaN;
    out->legs_suspension_revival_definition_stop_price = kNaN;
    out->legs_suspension_revival_definition_trail_points = kNaN;
    out->legs_suspension_revival_definition_trail_price = kNaN;
    out->legs_suspension_revival_definition_trail_offset = kNaN;
    out->legs_suspension_revival_definition_profit_ticks = kNaN;
    out->legs_suspension_revival_definition_loss_ticks = kNaN;
    const auto* revival = suspension && suspension->revival_definition
        ? &*suspension->revival_definition : nullptr;
    out->legs_suspension_revival_definition_present = revival ? 1U : 0U;
    if (revival) {
        out->legs_suspension_revival_definition_incarnation = revival->incarnation();
        out->legs_suspension_revival_definition_revision = revival->revision();
        out->legs_suspension_revival_definition_value_present = revival->has_value() ? 1U : 0U;
        if (revival->has_value()) {
            const auto& prices = revival->prices();
            out->legs_suspension_revival_definition_limit_price = prices.limit_price;
            out->legs_suspension_revival_definition_stop_price = prices.stop_price;
            out->legs_suspension_revival_definition_trail_points = prices.trail_points;
            out->legs_suspension_revival_definition_trail_price = prices.trail_price;
            out->legs_suspension_revival_definition_trail_offset = prices.trail_offset;
            out->legs_suspension_revival_definition_profit_ticks = prices.profit_ticks;
            out->legs_suspension_revival_definition_loss_ticks = prices.loss_ticks;
        }
    }
    out->legs_suspension_replacement_revival_definition_limit_price = kNaN;
    out->legs_suspension_replacement_revival_definition_stop_price = kNaN;
    out->legs_suspension_replacement_revival_definition_trail_points = kNaN;
    out->legs_suspension_replacement_revival_definition_trail_price = kNaN;
    out->legs_suspension_replacement_revival_definition_trail_offset = kNaN;
    out->legs_suspension_replacement_revival_definition_profit_ticks = kNaN;
    out->legs_suspension_replacement_revival_definition_loss_ticks = kNaN;
    const auto* replacement = suspension && suspension->replacement
        ? &*suspension->replacement : nullptr;
    out->legs_suspension_replacement_present = replacement ? 1U : 0U;
    if (replacement) {
        out->legs_suspension_replacement_queue_predecessor = replacement->queue_predecessor;
        const auto& definition = replacement->revival_definition;
        out->legs_suspension_replacement_revival_definition_incarnation = definition.incarnation();
        out->legs_suspension_replacement_revival_definition_revision = definition.revision();
        out->legs_suspension_replacement_revival_definition_value_present =
            definition.has_value() ? 1U : 0U;
        if (definition.has_value()) {
            const auto& prices = definition.prices();
            out->legs_suspension_replacement_revival_definition_limit_price = prices.limit_price;
            out->legs_suspension_replacement_revival_definition_stop_price = prices.stop_price;
            out->legs_suspension_replacement_revival_definition_trail_points = prices.trail_points;
            out->legs_suspension_replacement_revival_definition_trail_price = prices.trail_price;
            out->legs_suspension_replacement_revival_definition_trail_offset = prices.trail_offset;
            out->legs_suspension_replacement_revival_definition_profit_ticks = prices.profit_ticks;
            out->legs_suspension_replacement_revival_definition_loss_ticks = prices.loss_ticks;
        }
        const auto& release = replacement->release;
        out->legs_suspension_replacement_release_requested_event = release.requested.event;
        out->legs_suspension_replacement_release_requested_bar = release.requested.bar;
        out->legs_suspension_replacement_release_requested_domain =
            static_cast<std::uint32_t>(release.requested.domain);
        out->legs_suspension_replacement_release_requested_phase =
            static_cast<std::uint32_t>(release.requested.phase);
        out->legs_suspension_replacement_release_target_incarnation = release.target.incarnation;
        out->legs_suspension_replacement_release_target_owner = release.target.owner;
        out->legs_suspension_replacement_release_revision = release.revision;
    }
    out->legs_suspension_window_present = suspension && suspension->window ? 1U : 0U;
    out->legs_suspension_window_best = kNaN;
    out->legs_suspension_window_prefix = kNaN;
    if (suspension && suspension->window) {
        const auto& window = *suspension->window;
        out->legs_suspension_window_excluded_event = window.excluded.event;
        out->legs_suspension_window_excluded_bar = window.excluded.bar;
        out->legs_suspension_window_excluded_domain = static_cast<std::uint32_t>(window.excluded.domain);
        out->legs_suspension_window_excluded_phase = static_cast<std::uint32_t>(window.excluded.phase);
        out->legs_suspension_window_best = window.best;
        out->legs_suspension_window_prefix = window.prefix;
    }
    const auto& last = snapshot.legs.last_action();
    out->legs_last_present = last ? 1U : 0U;
    out->legs_last_suspend_legs_item0 = UINT32_MAX;
    out->legs_last_suspend_legs_item1 = UINT32_MAX;
    out->legs_last_suspend_legs_item2 = UINT32_MAX;
    out->legs_last_suspend_window_best = kNaN;
    out->legs_last_suspend_window_prefix = kNaN;
    out->legs_last_suspend_retire_item0 = UINT32_MAX;
    out->legs_last_suspend_retire_item1 = UINT32_MAX;
    out->legs_last_suspend_retire_item2 = UINT32_MAX;
    out->legs_last_stage_revival_definition_limit_price = kNaN;
    out->legs_last_stage_revival_definition_stop_price = kNaN;
    out->legs_last_stage_revival_definition_trail_points = kNaN;
    out->legs_last_stage_revival_definition_trail_price = kNaN;
    out->legs_last_stage_revival_definition_trail_offset = kNaN;
    out->legs_last_stage_revival_definition_profit_ticks = kNaN;
    out->legs_last_stage_revival_definition_loss_ticks = kNaN;
    out->legs_last_restore_legs_item0 = UINT32_MAX;
    out->legs_last_restore_legs_item1 = UINT32_MAX;
    out->legs_last_restore_legs_item2 = UINT32_MAX;
    out->legs_last_observe_high = kNaN;
    out->legs_last_observe_low = kNaN;
    out->legs_last_cancel_legs_item0 = UINT32_MAX;
    out->legs_last_cancel_legs_item1 = UINT32_MAX;
    out->legs_last_cancel_legs_item2 = UINT32_MAX;
    if (last) {
        out->legs_last_target_incarnation = last->target.incarnation;
        out->legs_last_target_owner = last->target.owner;
        out->legs_last_expected_revision = last->expected_revision;
        out->legs_last_cause_event = last->cause.event;
        out->legs_last_cause_bar = last->cause.bar;
        out->legs_last_cause_domain = static_cast<std::uint32_t>(last->cause.domain);
        out->legs_last_cause_phase = static_cast<std::uint32_t>(last->cause.phase);
        out->legs_last_operation = static_cast<std::uint32_t>(last->operation.index());
        if (const auto* bind = std::get_if<exit_legs::BindOwner>(&last->operation)) {
            out->legs_last_bind_owner = bind->owner;
        } else if (const auto* suspended = std::get_if<exit_legs::Suspend>(&last->operation)) {
            const auto copy_leg = [](const std::vector<exit_legs::Leg>& legs,
                                     std::size_t index) -> std::uint32_t {
                return index < legs.size() ? static_cast<std::uint32_t>(legs[index])
                                           : UINT32_MAX;
            };
            out->legs_last_suspend_legs_count =
                static_cast<std::uint32_t>(suspended->legs.size());
            out->legs_last_suspend_legs_item0 = copy_leg(suspended->legs, 0);
            out->legs_last_suspend_legs_item1 = copy_leg(suspended->legs, 1);
            out->legs_last_suspend_legs_item2 = copy_leg(suspended->legs, 2);
            out->legs_last_suspend_hold_present = suspended->hold ? 1U : 0U;
            if (suspended->hold) {
                const auto& hold = *suspended->hold;
                out->legs_last_suspend_hold_requested_event = hold.requested.event;
                out->legs_last_suspend_hold_requested_bar = hold.requested.bar;
                out->legs_last_suspend_hold_requested_domain =
                    static_cast<std::uint32_t>(hold.requested.domain);
                out->legs_last_suspend_hold_requested_phase =
                    static_cast<std::uint32_t>(hold.requested.phase);
                out->legs_last_suspend_hold_target_incarnation = hold.target.incarnation;
                out->legs_last_suspend_hold_target_owner = hold.target.owner;
                out->legs_last_suspend_hold_revision = hold.revision;
            }
            out->legs_last_suspend_window_present = suspended->window ? 1U : 0U;
            if (suspended->window) {
                const auto& window = *suspended->window;
                out->legs_last_suspend_window_excluded_event = window.excluded.event;
                out->legs_last_suspend_window_excluded_bar = window.excluded.bar;
                out->legs_last_suspend_window_excluded_domain =
                    static_cast<std::uint32_t>(window.excluded.domain);
                out->legs_last_suspend_window_excluded_phase =
                    static_cast<std::uint32_t>(window.excluded.phase);
                out->legs_last_suspend_window_best = window.best;
                out->legs_last_suspend_window_prefix = window.prefix;
            }
            out->legs_last_suspend_retire_count =
                static_cast<std::uint32_t>(suspended->retire.size());
            out->legs_last_suspend_retire_item0 = copy_leg(suspended->retire, 0);
            out->legs_last_suspend_retire_item1 = copy_leg(suspended->retire, 1);
            out->legs_last_suspend_retire_item2 = copy_leg(suspended->retire, 2);
        } else if (const auto* stage =
                       std::get_if<exit_legs::StageReplacement>(&last->operation)) {
            const auto& relation = stage->relation;
            out->legs_last_stage_queue_predecessor = relation.queue_predecessor;
            const auto& definition = relation.revival_definition;
            out->legs_last_stage_revival_definition_incarnation = definition.incarnation();
            out->legs_last_stage_revival_definition_revision = definition.revision();
            out->legs_last_stage_revival_definition_value_present =
                definition.has_value() ? 1U : 0U;
            if (definition.has_value()) {
                const auto& prices = definition.prices();
                out->legs_last_stage_revival_definition_limit_price = prices.limit_price;
                out->legs_last_stage_revival_definition_stop_price = prices.stop_price;
                out->legs_last_stage_revival_definition_trail_points = prices.trail_points;
                out->legs_last_stage_revival_definition_trail_price = prices.trail_price;
                out->legs_last_stage_revival_definition_trail_offset = prices.trail_offset;
                out->legs_last_stage_revival_definition_profit_ticks = prices.profit_ticks;
                out->legs_last_stage_revival_definition_loss_ticks = prices.loss_ticks;
            }
            const auto& release = relation.release;
            out->legs_last_stage_release_requested_event = release.requested.event;
            out->legs_last_stage_release_requested_bar = release.requested.bar;
            out->legs_last_stage_release_requested_domain =
                static_cast<std::uint32_t>(release.requested.domain);
            out->legs_last_stage_release_requested_phase =
                static_cast<std::uint32_t>(release.requested.phase);
            out->legs_last_stage_release_target_incarnation = release.target.incarnation;
            out->legs_last_stage_release_target_owner = release.target.owner;
            out->legs_last_stage_release_revision = release.revision;
        } else if (const auto* restore = std::get_if<exit_legs::Restore>(&last->operation)) {
            out->legs_last_restore_legs_count =
                static_cast<std::uint32_t>(restore->legs.size());
            const auto copy_leg = [&](std::size_t index) -> std::uint32_t {
                return index < restore->legs.size()
                    ? static_cast<std::uint32_t>(restore->legs[index]) : UINT32_MAX;
            };
            out->legs_last_restore_legs_item0 = copy_leg(0);
            out->legs_last_restore_legs_item1 = copy_leg(1);
            out->legs_last_restore_legs_item2 = copy_leg(2);
        } else if (const auto* complete =
                       std::get_if<exit_legs::CompleteBarrier>(&last->operation)) {
            out->legs_last_complete_completed_event = complete->completed.event;
            out->legs_last_complete_completed_bar = complete->completed.bar;
            out->legs_last_complete_completed_domain =
                static_cast<std::uint32_t>(complete->completed.domain);
            out->legs_last_complete_completed_phase =
                static_cast<std::uint32_t>(complete->completed.phase);
            out->legs_last_complete_requested_present = complete->requested ? 1U : 0U;
            if (complete->requested) {
                const auto& requested = *complete->requested;
                out->legs_last_complete_requested_requested_event = requested.requested.event;
                out->legs_last_complete_requested_requested_bar = requested.requested.bar;
                out->legs_last_complete_requested_requested_domain =
                    static_cast<std::uint32_t>(requested.requested.domain);
                out->legs_last_complete_requested_requested_phase =
                    static_cast<std::uint32_t>(requested.requested.phase);
                out->legs_last_complete_requested_target_incarnation =
                    requested.target.incarnation;
                out->legs_last_complete_requested_target_owner = requested.target.owner;
                out->legs_last_complete_requested_revision = requested.revision;
            }
        } else if (const auto* observe = std::get_if<exit_legs::Observe>(&last->operation)) {
            out->legs_last_observe_high = observe->high;
            out->legs_last_observe_low = observe->low;
            out->legs_last_observe_direction = observe->direction;
            out->legs_last_observe_fold = static_cast<std::uint32_t>(observe->fold);
        } else if (const auto* cancel = std::get_if<exit_legs::Cancel>(&last->operation)) {
            out->legs_last_cancel_legs_count =
                static_cast<std::uint32_t>(cancel->legs.size());
            const auto copy_leg = [&](std::size_t index) -> std::uint32_t {
                return index < cancel->legs.size()
                    ? static_cast<std::uint32_t>(cancel->legs[index]) : UINT32_MAX;
            };
            out->legs_last_cancel_legs_item0 = copy_leg(0);
            out->legs_last_cancel_legs_item1 = copy_leg(1);
            out->legs_last_cancel_legs_item2 = copy_leg(2);
        }
    }
    const auto& admission = snapshot.market_admission;
    const auto& observation = admission.observation();
    if (observation) out->market_admission_observation_present = 1U;
    if (observation) {
        out->market_admission_observation_command = observation->command;
        out->market_admission_observation_kind = static_cast<std::int64_t>(observation->kind);
        out->market_admission_observation_birth_cause =
            static_cast<std::int64_t>(observation->birth.cause());
        out->market_admission_observation_birth_bar = observation->birth.bar();
        out->market_admission_observation_birth_timestamp = observation->birth.timestamp();
        out->market_admission_observation_birth_cursor_domain =
            static_cast<std::int64_t>(observation->birth.cursor().domain());
        out->market_admission_observation_birth_cursor_position =
            static_cast<std::int64_t>(observation->birth.cursor().position());
        out->market_admission_observation_birth_cursor_index = observation->birth.cursor().index();
        out->market_admission_observation_birth_cursor_count = observation->birth.cursor().count();
        out->market_admission_observation_birth_cursor_price = observation->birth.cursor_price();
        out->market_admission_observation_birth_first_fill = observation->birth.first_fill();
        out->market_admission_observation_birth_last_fill = observation->birth.last_fill();
        out->market_admission_observation_birth_evaluation_ordinal =
            observation->birth.evaluation_ordinal();
        copy_pending_string(observation->id, out->market_admission_observation_id,
            &out->market_admission_observation_id_truncated,
            &out->market_admission_observation_id_hash64);
        out->market_admission_observation_requested_quantity = observation->requested_quantity;
        out->market_admission_observation_quantity_type = observation->quantity_type;
        out->market_admission_observation_buy = observation->buy ? 1U : 0U;
        out->market_admission_observation_prices_limit = observation->prices.limit;
        out->market_admission_observation_prices_stop = observation->prices.stop;
        copy_pending_string(observation->oca_name, out->market_admission_observation_oca_name,
            &out->market_admission_observation_oca_name_truncated,
            &out->market_admission_observation_oca_name_hash64);
        out->market_admission_observation_oca_type = observation->oca_type;
        const auto& configuration = observation->configuration;
        out->market_admission_observation_configuration_process_on_close =
            configuration.process_on_close ? 1U : 0U;
        out->market_admission_observation_configuration_calc_on_fills =
            configuration.calc_on_fills ? 1U : 0U;
        out->market_admission_observation_configuration_magnifier =
            configuration.magnifier ? 1U : 0U;
        out->market_admission_observation_configuration_fill_recalculation =
            configuration.fill_recalculation ? 1U : 0U;
        out->market_admission_observation_configuration_scheduler =
            configuration.scheduler ? 1U : 0U;
        out->market_admission_observation_configuration_slippage = configuration.slippage;
        out->market_admission_observation_configuration_pyramiding = configuration.pyramiding;
        out->market_admission_observation_configuration_default_quantity_type =
            configuration.default_quantity_type;
        out->market_admission_observation_configuration_default_quantity_value =
            configuration.default_quantity_value;
        out->market_admission_observation_configuration_long_margin = configuration.long_margin;
        out->market_admission_observation_configuration_short_margin = configuration.short_margin;
        out->market_admission_observation_configuration_commission_value =
            configuration.commission_value;
        out->market_admission_observation_configuration_commission_type =
            configuration.commission_type;
        out->market_admission_observation_configuration_pointvalue = configuration.pointvalue;
        out->market_admission_observation_configuration_fx = configuration.fx;
        out->market_admission_observation_configuration_quantity_step =
            configuration.quantity_step;
        out->market_admission_observation_configuration_mintick = configuration.mintick;
        out->market_admission_observation_configuration_risk_direction =
            configuration.risk_direction;
        out->market_admission_observation_configuration_loss_days_limit =
            configuration.loss_days_limit;
        out->market_admission_observation_configuration_drawdown_limit =
            configuration.drawdown_limit;
        out->market_admission_observation_configuration_intraday_loss_limit =
            configuration.intraday_loss_limit;
        out->market_admission_observation_configuration_position_limit =
            configuration.position_limit;
        out->market_admission_observation_configuration_fill_cap_active =
            configuration.fill_cap_active ? 1U : 0U;
        out->market_admission_observation_configuration_risk_halted =
            configuration.risk_halted ? 1U : 0U;
        out->market_admission_observation_bar = observation->bar;
        out->market_admission_observation_placement_side = observation->placement_side;
        out->market_admission_observation_placement_cycle = observation->placement_cycle;
        out->market_admission_observation_prior_close_quantity =
            observation->prior_close_quantity;
        out->market_admission_observation_held_quantity = observation->held_quantity;
        out->market_admission_observation_held_entries = observation->held_entries;
        out->market_admission_observation_realized_equity = observation->realized_equity;
        out->market_admission_observation_placement_equity = observation->placement_equity;
        out->market_admission_observation_signal_close = observation->signal_close;
        out->market_admission_observation_quantized_fixed_quantity =
            observation->quantized_fixed_quantity;
        out->market_admission_observation_original_sizing_present =
            observation->original_sizing ? 1U : 0U;
        if (observation->original_sizing) {
            out->market_admission_observation_original_sizing_quantity =
                observation->original_sizing->quantity;
            out->market_admission_observation_original_sizing_equity =
                observation->original_sizing->equity;
            out->market_admission_observation_original_sizing_price =
                observation->original_sizing->price;
            out->market_admission_observation_original_sizing_mark =
                observation->original_sizing->mark;
            out->market_admission_observation_original_sizing_fx =
                observation->original_sizing->fx;
        }
        out->market_admission_observation_explicit_equity = observation->explicit_equity;
        out->market_admission_observation_explicit_price = observation->explicit_price;
    }
    out->market_admission_review_present = admission.review() ? 1U : 0U;
    if (admission.review()) {
        out->market_admission_review_sequence = admission.review()->sequence;
        out->market_admission_review_checkpoint =
            static_cast<std::int64_t>(admission.review()->checkpoint);
        out->market_admission_review_bar = admission.review()->bar;
        out->market_admission_review_target_command = admission.review()->target_command;
    }
    out->market_admission_sizing_revision_present = admission.sizing_revision() ? 1U : 0U;
    if (admission.sizing_revision()) {
        out->market_admission_sizing_revision_sequence = admission.sizing_revision()->sequence;
        out->market_admission_sizing_revision_cause_fill =
            admission.sizing_revision()->cause_fill;
        out->market_admission_sizing_revision_bar = admission.sizing_revision()->bar;
        out->market_admission_sizing_revision_target_command =
            admission.sizing_revision()->target_command;
    }
    out->cancellation_cause = static_cast<std::int32_t>(snapshot.cancellation.cause);
    out->cancellation_state = snapshot.cancellation.state;
    out->cancellation_close_claim_release = snapshot.cancellation.close_claim_release;
    out->cancellation_source_incarnation = snapshot.cancellation.source_incarnation;
    out->cancellation_source_sequence = snapshot.cancellation.source_sequence;
    out->cancellation_target_incarnation = snapshot.cancellation.target_incarnation;
    out->cancellation_target_owner = snapshot.cancellation.target_owner;
    out->cancellation_target_revision = snapshot.cancellation.target_revision;
    out->cancellation_close_claim_consumed = snapshot.cancellation.close_claim_consumed;
    out->cancellation_close_claim_retired = snapshot.cancellation.close_claim_retired;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s pending_walk_oracle|pending_walk_cost|"
                             "pending_walk_growth\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "pending_walk_oracle") == 0) oracle_case();
    else if (std::strcmp(argv[1], "pending_walk_cost") == 0) cost_case(false);
    else if (std::strcmp(argv[1], "pending_walk_growth") == 0) cost_case(true);
    else {
        std::fprintf(stderr, "unknown pending walk case: %s\n", argv[1]);
        return 2;
    }
    std::printf("%s: %d checks, %d failures\n", argv[1], checks, failures);
    return failures == 0 ? 0 : 1;
}
