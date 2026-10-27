#include <pineforge/source/detail/live_row_index.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::source;
using namespace pineforge::source::detail;
using H = native_order::RequestHandle;
using C = LiveClass;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Independent spelling of the design predicates. Do not call the product's
// signature/table here: changing it must not change this oracle.
bool model_class(C c, const PlacementSnapshot& r) {
    const auto p = [](double x) { return std::isfinite(x) && x > 0; };
    const bool entry = r.family == PineOrderFamily::Entry;
    const bool order = r.family == PineOrderFamily::Order;
    const bool close = r.family == PineOrderFamily::Close;
    const bool all = r.family == PineOrderFamily::CloseAll;
    const bool exit = r.family == PineOrderFamily::ExitLimit
        || r.family == PineOrderFamily::ExitStop || r.family == PineOrderFamily::ExitTrail;
    const bool u2 = !p(r.exit_levels.limit) && !p(r.exit_levels.stop);
    const bool trails = !p(r.exit_levels.trail_points) && !p(r.exit_levels.trail_price)
        && !p(r.exit_levels.trail_offset);
    const bool birth = !r.birth.from_fill() && !r.birth.at_terminal_fill()
        && !compat::pine::historical_cascade_reach(r.birth_reach);
    switch (c) {
    case C::Placed: return true;
    case C::Opening: return r.opening;
    case C::EntryLikeOpening: return r.opening && (entry || order);
    case C::EntryOpening: return r.opening && entry;
    case C::OrderOpening: return r.opening && order;
    case C::CloseOpening: return r.opening && close;
    case C::CloseAllOpening: return r.opening && all;
    case C::ExitLimitOpening: return r.opening && r.family == PineOrderFamily::ExitLimit;
    case C::ExitStopOpening: return r.opening && r.family == PineOrderFamily::ExitStop;
    case C::ExitTrailOpening: return r.opening && r.family == PineOrderFamily::ExitTrail;
    case C::MarginOpening: return r.opening && r.family == PineOrderFamily::Margin;
    case C::RiskOpening: return r.opening && r.family == PineOrderFamily::Risk;
    case C::Entry: return entry;
    case C::CloseAll: return all;
    case C::NonImmediateClose: return (close || all) && !r.immediately;
    case C::DynamicGlobalExit: return r.pooc_global_full_exit_dynamic_qty;
    case C::ReservationCapture: return r.reservation_expansion.capture().has_value();
    case C::ReservationEligible: return (exit || close) && !r.void_issue;
    case C::ReservationExit: return exit && !r.void_issue;
    case C::ReservationClose: return close && !r.void_issue;
    case C::WaypointMargin: return r.waypoint_margin_call;
    case C::Trail: return r.family == PineOrderFamily::ExitTrail;
    case C::RestedStopForced:
        return p(r.exit_levels.stop) && (entry || (r.family == PineOrderFamily::ExitStop
            && std::isnan(r.exit_levels.trail_points) && std::isnan(r.exit_levels.trail_price)
            && std::isnan(r.exit_levels.trail_offset))) && p(r.forced_execution_price);
    case C::EntryOpeningU2: return entry && r.opening && u2;
    case C::DefaultEntryOpening: return entry && r.opening && !std::isfinite(r.requested_qty);
    case C::ReservationParent:
        return entry && r.opening && !r.projection_over_pyramiding
            && p(p(r.requested_qty) ? std::abs(r.requested_qty) : r.frozen_market_own_units);
    case C::UnpricedEntry: return entry && u2 && trails && birth;
    case C::UnpricedClose: return close && r.oca_name.empty() && u2 && trails && birth;
    case C::ReverseOpeningEntry: return entry && r.opening && r.reverse_to;
    case C::SamePointEligible:
    case C::SamePointMarket:
    case C::SamePointStop:
    case C::SamePointLimit:
        return entry && r.opening
            && r.projection_position_side == static_cast<std::int32_t>(PositionSide::FLAT)
            && p(r.requested_qty)
            && (r.qty_type < 0 || r.qty_type == static_cast<int>(QtyType::FIXED))
            && r.oca_name.empty() && !r.birth.from_fill()
            && !(p(r.exit_levels.stop) && p(r.exit_levels.limit)) && trails
            && (c == C::SamePointEligible
                || (c == C::SamePointMarket && !p(r.exit_levels.stop) && !p(r.exit_levels.limit))
                || (c == C::SamePointStop && p(r.exit_levels.stop))
                || (c == C::SamePointLimit && p(r.exit_levels.limit)));
    case C::PureStopEntry:
        return entry && p(r.exit_levels.stop) && !p(r.exit_levels.limit) && trails;
    case C::PureBuyLimitOpening:
        return entry && r.opening && r.is_long && p(r.exit_levels.limit)
            && !p(r.exit_levels.stop) && trails;
    case C::TouchedStopEntry:
        return entry && p(r.exit_levels.stop) && !std::isfinite(r.exit_levels.limit)
            && !p(r.exit_levels.trail_offset);
    case C::DropLimit:
        return r.family == PineOrderFamily::ExitLimit && !std::isfinite(r.trail_activation_level);
    case C::DropStop:
        return r.family == PineOrderFamily::ExitStop && !std::isfinite(r.trail_activation_level);
    case C::NonPureLimitOpening:
        return [](const PlacementSnapshot& row) {
            const auto finite_positive = [](double x) { return std::isfinite(x) && x > 0.0; };
            // accepted_entries_in_cycle / place_entry: no family restriction.
            return row.opening
                && !(finite_positive(row.exit_levels.limit)
                    && !finite_positive(row.exit_levels.stop)
                    && !finite_positive(row.exit_levels.trail_points)
                    && !finite_positive(row.exit_levels.trail_price)
                    && !finite_positive(row.exit_levels.trail_offset));
        }(r);
    case C::ExitStop:
        return [](const PlacementSnapshot& leg) {
            return leg.family == PineOrderFamily::ExitStop;
        }(r);
    case C::ExitLimit:
        return [](const PlacementSnapshot& leg) {
            return leg.family == PineOrderFamily::ExitLimit;
        }(r);
    case C::PureMarketAddOpening:
        return [](const PlacementSnapshot& add) {
            const auto finite_positive = [](double x) { return std::isfinite(x) && x > 0.0; };
            // pure_market_add: source/from-entry wildcard and side are query
            // arguments; historical cascade reach is deliberately unrestricted.
            return add.opening
                && (add.family == PineOrderFamily::Entry || add.family == PineOrderFamily::Order)
                && !finite_positive(add.exit_levels.limit)
                && !finite_positive(add.exit_levels.stop)
                && !finite_positive(add.exit_levels.trail_points)
                && !finite_positive(add.exit_levels.trail_price)
                && !finite_positive(add.exit_levels.trail_offset)
                && !add.birth.from_fill() && !add.birth.at_terminal_fill();
        }(r);
    case C::DefaultEntryOpeningU2:
        return [](const PlacementSnapshot& row) {
            const auto finite_positive = [](double x) { return std::isfinite(x) && x > 0.0; };
            // apply_reversal_gap_bracket_policy's source-field selection.
            return row.opening && row.family == PineOrderFamily::Entry
                && !std::isfinite(row.requested_qty)
                && !finite_positive(row.exit_levels.limit) && !finite_positive(row.exit_levels.stop);
        }(r);
    case C::TerminalExplicitMarketEntry:
        return [](const PlacementSnapshot& row) {
            const auto finite_positive = [](double x) { return std::isfinite(x) && x > 0.0; };
            // apply_terminal_explicit_market_policy, before its created-bar key.
            return row.opening && row.family == PineOrderFamily::Entry
                && finite_positive(row.requested_qty)
                && !finite_positive(row.exit_levels.limit) && !finite_positive(row.exit_levels.stop)
                && row.oca_name.empty() && row.oca_type == 0
                && !row.birth.from_fill() && !row.birth.at_terminal_fill()
                && !compat::pine::historical_cascade_reach(row.birth_reach);
        }(r);
    case C::Count: break;
    }
    throw std::runtime_error("unmodelled class");
}

// Query keys are selected independently from the model's row fields.
bool model_key(const LiveKey& key, const PlacementSnapshot& r) {
    switch (key.kind) {
    case LiveKey::Kind::All: return true;
    case LiveKey::Kind::Source: return r.source_id == key.first;
    case LiveKey::Kind::FromEntry: return r.from_entry == key.first;
    case LiveKey::Kind::Pair: return r.source_id == key.first && r.from_entry == key.second;
    case LiveKey::Kind::ScriptTime: return r.placement_script_open_ms == key.number;
    case LiveKey::Kind::CreatedBar: return r.projection_created_bar == key.number;
    case LiveKey::Kind::SourceTime:
        return r.source_id == key.first && r.placement_script_open_ms == key.number;
    case LiveKey::Kind::BracketOrigin: return r.bracket_origin == key.origin;
    case LiveKey::Kind::PerEntryOrigin: return r.per_entry_origin == key.incarnation;
    }
    return false;
}

std::vector<H> expected(const PlacementTable& table, const std::vector<H>& live,
                        C c, const LiveKey& key, int side) {
    std::vector<H> result;
    for (const auto& h : live) {
        const auto r = table.find(h.incarnation);
        if (r != table.end() && model_class(c, r->second) && model_key(key, r->second)
            && (side < 0 || static_cast<int>(r->second.is_long) == side))
            result.push_back(h);
    }
    return result;
}

void compare(LiveRowIndex& index, const PlacementTable& table, const std::vector<H>& live) {
    require(index.size() == live.size(), "full-handle membership cardinality");
    require(index.members() == live, "visible forward order");
    std::size_t missing = 0;
    for (const auto& h : live) if (table.find(h.incarnation) == table.end()) ++missing;
    require(index.missing_rows() == missing, "missing rows satisfy no row class");
    // Construct reverse from one owning snapshot, not from two temporaries.
    const auto snapshot = index.members();
    const std::vector<H> reverse(snapshot.rbegin(), snapshot.rend());
    require(std::equal(reverse.begin(), reverse.end(), live.rbegin()), "visible reverse order");
    std::vector<LiveKey> keys{LiveKey{}, LiveKey::source(""), LiveKey::source("absent"),
        LiveKey::from_entry(""), LiveKey::pair("", ""), LiveKey::script_time(-1),
        LiveKey::created_bar(-1), LiveKey::source_time("", -1),
        LiveKey::bracket_origin(H{{"model", 1}, 2}), LiveKey::per_entry_origin(2)};
    for (const auto& h : live) {
        require(index.contains(h), "live membership");
        const auto row = table.find(h.incarnation);
        if (row == table.end()) continue;
        const auto& r = row->second;
        keys.push_back(LiveKey::source(r.source_id));
        keys.push_back(LiveKey::from_entry(r.from_entry));
        keys.push_back(LiveKey::pair(r.source_id, r.from_entry));
        keys.push_back(LiveKey::script_time(r.placement_script_open_ms));
        keys.push_back(LiveKey::created_bar(r.projection_created_bar));
        keys.push_back(LiveKey::source_time(r.source_id, r.placement_script_open_ms));
        keys.push_back(LiveKey::bracket_origin(r.bracket_origin));
        keys.push_back(LiveKey::per_entry_origin(r.per_entry_origin));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (std::size_t ci = 0; ci < static_cast<std::size_t>(C::Count); ++ci) {
        const auto c = static_cast<C>(ci);
        for (const auto& key : keys) for (int side = -1; side <= 1; ++side) {
            const auto wanted = expected(table, live, c, key, side);
            require(index.count(c, key, side) == wanted.size(), "class/key/side count");
            require(index.rows(c, key, side) == wanted, "class/key/side live order");
            auto before = wanted;
            before.erase(std::remove_if(before.begin(), before.end(), [&](const H& h) {
                return table.at(h.incarnation).command_sequence >= 5;
            }), before.end());
            require(index.before_command(c, 5, key, side) == before, "command threshold and live order");
            require(index.count_before_command(c, 5, key, side) == before.size(),
                    "command threshold count");
            for (auto field : {LivePrice::Stop, LivePrice::Limit}) {
                std::vector<double> prices;
                for (const auto& h : wanted) {
                    const auto& r = table.at(h.incarnation);
                    const double price = field == LivePrice::Stop
                        ? r.exit_levels.stop : r.exit_levels.limit;
                    if (std::isfinite(price) && price > 0) prices.push_back(price);
                }
                const auto lo = index.min_price(c, field, key, side);
                const auto hi = index.max_price(c, field, key, side);
                require(lo.has_value() == !prices.empty(), "minimum presence");
                require(hi.has_value() == !prices.empty(), "maximum presence");
                if (!prices.empty()) {
                    require(*lo == *std::min_element(prices.begin(), prices.end()), "minimum value");
                    require(*hi == *std::max_element(prices.begin(), prices.end()), "maximum value");
                }
                std::vector<H> reached;
                for (const auto& h : wanted) {
                    const auto& r = table.at(h.incarnation);
                    const auto price = field == LivePrice::Stop ? r.exit_levels.stop : r.exit_levels.limit;
                    if (std::isfinite(price) && price > 0 && price >= 1 && price <= 100)
                        reached.push_back(h);
                }
                require(index.count_in_range(c, field, 1, 100, key, side) == reached.size(),
                        "inclusive price cardinality");
                require(index.rows_in_range(c, field, 1, 100, key, side) == reached,
                        "price range restores live order");
            }
        }
    }
    for (const auto& from : {"", "f0", "absent"}) {
        auto reserved = expected(table, live, C::ReservationEligible, LiveKey::from_entry(from), -1);
        reserved.erase(std::remove_if(reserved.begin(), reserved.end(), [&](const H& h) {
            const auto& r = table.at(h.incarnation);
            return r.family == PineOrderFamily::Close && r.placement_cycle != 0 && r.placement_cycle < 2;
        }), reserved.end());
        require(index.reservation_rows(from, 2) == reserved, "reservation cycle predicate and live order");
        require(index.reservation_count(from, 2) == reserved.size(), "reservation cycle count");
    }
}

PlacementSnapshot random_row(std::mt19937& rng) {
    PlacementSnapshot r;
    const double values[] = {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        -1, -0.0, 0, 1, 50, 100, 101};
    const auto value = [&] { return values[rng() % 10]; };
    r.family = static_cast<PineOrderFamily>(rng() % 9);
    r.source_id = rng() % 3 ? "s" + std::to_string(rng() % 3) : "";
    r.from_entry = rng() % 3 ? "f" + std::to_string(rng() % 3) : "";
    r.opening = rng() % 2;
    r.is_long = rng() % 2;
    r.immediately = rng() % 2;
    r.void_issue = rng() % 2;
    r.reverse_to = rng() % 2;
    r.waypoint_margin_call = rng() % 2;
    r.pooc_global_full_exit_dynamic_qty = rng() % 2;
    r.projection_over_pyramiding = rng() % 2;
    r.requested_qty = value();
    r.frozen_market_own_units = value();
    r.forced_execution_price = value();
    r.trail_activation_level = value();
    r.qty_type = static_cast<int>(rng() % 4) - 1;
    r.oca_name = rng() % 2 ? "" : "oca";
    r.oca_type = static_cast<int>(rng() % 3);
    r.placement_script_open_ms = static_cast<int>(rng() % 3) - 1;
    r.projection_created_bar = static_cast<int>(rng() % 3) - 1;
    r.projection_position_side = static_cast<int>(rng() % 3) - 1;
    r.command_sequence = rng() % 10;
    r.placement_cycle = rng() % 3;
    r.bracket_origin = H{{"model", rng() % 2}, rng() % 3};
    r.per_entry_origin = rng() % 3;
    r.exit_levels.limit = value();
    r.exit_levels.stop = value();
    r.exit_levels.trail_points = value();
    r.exit_levels.trail_price = value();
    r.exit_levels.trail_offset = value();
    if (rng() % 2) {
        r.reservation_expansion.capture(1, 1, PositionSide::LONG, 1);
        if (rng() % 2) r.reservation_expansion.close_population(2);
    }
    if (rng() % 2)
        r.birth = OrderBirth::fill_evaluation(0, 0,
            BirthCursor::point(BirthCursorDomain::HistoricalPath, rng() % 4, 4),
            100, 1, 1, 1);
    r.birth_reach = rng() % 2 ? compat::pine::HistoricalBirthReach::Standard
        : compat::pine::HistoricalBirthReach::ExtremeWaypoints;
    return r;
}

void stream() {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    LiveHandleSequence sequence;
    std::vector<H> live;
    LiveRowIndex index;
    index.rebuild_from(table, sequence);
    std::mt19937 rng(452391);
    for (unsigned step = 0; step != 160; ++step) {
        const H handle{{"model", rng() % 2}, 1 + rng() % 8};
        const auto found = std::find(live.begin(), live.end(), handle);
        switch (rng() % 8) {
        case 0:
        case 1:
            if (found == live.end()) { live.push_back(handle); sequence.push_back(handle); }
            if (table.contains(handle.incarnation)) table.replace(handle.incarnation, random_row(rng));
            else table.try_emplace(handle.incarnation, random_row(rng));
            // A placement is incarnation-keyed; two full handles can share
            // that row in fixtures. Refresh must update every such member.
            index.refresh(handle);
            break;
        case 2:
            sequence.erase_handle(handle);
            if (found != live.end()) live.erase(found);
            index.erase(handle);
            break;
        case 3:
            if (table.contains(handle.incarnation)) {
                auto& r = table.at(handle.incarnation);
                r.opening = !r.opening;
                r.exit_levels.stop = (step % 2) ? 50 : std::numeric_limits<double>::quiet_NaN();
                index.refresh(handle);
            }
            break;
        case 4:
            table.erase(handle.incarnation);
            index.refresh(handle); // A missing row remains a live member only.
            break;
        case 5:
            if (found != live.end()) {
                live.erase(found); live.push_back(handle);
                sequence.move_to_tail(handle);
                index.refresh(handle);
            }
            break;
        case 6:
            index.rebuild_from(table, sequence);
            break;
        case 7:
            if (step % 7 == 0) {
                table.clear(); sequence.clear(); live.clear(); index.clear();
            }
            break;
        }
        compare(index, table, live);
    }
    // Missed-hook net: a field write without refresh must be detected by an
    // assert-enabled query; it must never silently repair the stale record.
    if (LiveRowIndex::oracle_enabled()) {
        table.clear(); sequence.clear(); index.clear();
        const H h{{"model", 1}, 1};
        PlacementSnapshot r;
        r.opening = true;
        table.try_emplace(1, std::move(r));
        sequence.push_back(h);
        index.insert(h);
        table.at(1).opening = false;
        bool caught = false;
        try { (void)index.count(C::EntryOpening); }
        catch (const coded<std::logic_error>& error) {
            caught = error.run_failure_code() == RunFailureCode::engine_invariant
                && std::string(error.what()).find("EntryOpening") != std::string::npos;
        }
        require(caught, "oracle must name the missed class hook");
    }
}

void edges_and_aliases() {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    LiveHandleSequence sequence;
    std::vector<H> live;
    LiveRowIndex index;
    index.rebuild_from(table, sequence);
    std::array<bool, static_cast<std::size_t>(C::Count)> reached{};
    // Directed source-field rows ensure every class has a positive witness,
    // including classes whose conjunction is rare in the randomized stream.
    for (int family = 0; family != 9; ++family) for (int variant = 0; variant != 4; ++variant) {
        PlacementSnapshot r;
        r.family = static_cast<PineOrderFamily>(family);
        r.opening = true;
        r.is_long = variant % 2 == 0;
        r.reverse_to = true;
        r.waypoint_margin_call = true;
        r.pooc_global_full_exit_dynamic_qty = true;
        r.requested_qty = variant == 3 ? std::numeric_limits<double>::infinity() : 1;
        r.frozen_market_own_units = 2;
        r.forced_execution_price = 100;
        r.reservation_expansion.capture(1, 1, PositionSide::LONG, 1);
        if (variant == 3) r.reservation_expansion.close_population(2);
        r.placement_cycle = variant % 3;
        r.command_sequence = variant;
        if (variant == 0) r.exit_levels.stop = 50;
        if (variant == 2) r.exit_levels.limit = 100;
        const H h{{"edge", 1}, 1 + live.size()};
        for (std::size_t c = 0; c != reached.size(); ++c)
            reached[c] = reached[c] || model_class(static_cast<C>(c), r);
        table.try_emplace(h.incarnation, std::move(r));
        sequence.push_back(h); live.push_back(h); index.insert(h);
    }
    require(std::all_of(reached.begin(), reached.end(), [](bool r) { return r; }),
            "every index class must have a positive independent witness");
    // Existing raw pending-observer fixtures deliberately publish duplicates.
    sequence.push_back(live.front()); live.push_back(live.front()); index.refresh(live.front());
    compare(index, table, live);
    const auto copy = sequence;
    const auto old = live;
    sequence.erase_handle(live.front());
    const auto erased = live.front();
    live.erase(std::remove(live.begin(), live.end(), erased), live.end());
    index.erase(erased);
    require(copy.snapshot() == old, "owning copied sequence survives mutation");
    compare(index, table, live);
    // Trigger stable compaction and compare survivor order after every retire.
    while (!live.empty()) {
        const auto h = live.front();
        sequence.erase_handle(h); live.erase(live.begin()); index.erase(h);
        require(index.members() == live, "compaction must preserve live order");
    }
    // The source directory must distinguish full run identity, preserve all
    // keys owned by one handle, and detach old ownership on reuse/extract.
    LiveRowIndex::SourceMap sources;
    index.bind_source_keys(sources);
    const H a{{"owners", 1}, 7}, b{{"owners", 2}, 7};
    const auto install = [&](std::uint64_t key, H h) {
        sources.insert_or_assign(key, LiveSourceHandle{h, "", "", ""});
        index.refresh_source_key(key);
    };
    install(3, a); install(5, a); install(9, b); install(3, b);
    require(index.source_keys(a) == std::vector<std::uint64_t>{5}, "overwritten key leaves prior owner");
    auto owned = index.source_keys(b);
    std::sort(owned.begin(), owned.end());
    require(owned == std::vector<std::uint64_t>({3, 9}), "same incarnation in another run owns separate keys");
    auto extracted = sources.extract(3);
    index.refresh_source_key(3);
    extracted.mapped() = LiveSourceHandle{a, "", "", ""};
    sources.insert(std::move(extracted));
    index.refresh_source_key(3);
    owned = index.source_keys(a);
    std::sort(owned.begin(), owned.end());
    require(owned == std::vector<std::uint64_t>({3, 5}), "extracted key publishes new ownership");
    sources.erase(5);
    // Wholesale restore is the explicit rebuild boundary, with the SAME
    // canonical container addresses; no query may silently self-repair.
    index.rebuild_from(table, sequence);
    require(index.source_keys(a) == std::vector<std::uint64_t>{3}, "restore rebuilds reverse ownership");
    sources.clear(); index.clear();

    LiveHandleSequence newborns;
    newborns.push_back(a); newborns.push_back(b);
    const auto snapshot = newborns.take();
    require(snapshot == std::vector<H>({a, b}) && newborns.empty(), "newborn take preserves full order");
    newborns.assign(snapshot);
    newborns.stable_sort([](const H& left, const H& right) {
        return left.run.run_number > right.run.run_number;
    });
    require(newborns.snapshot() == std::vector<H>({b, a}), "stable permutation publishes exact order");
}

void origin_incidence() {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    LiveHandleSequence sequence;
    const H a{{"origin", 1}, 1}, b{{"origin", 2}, 1}, c{{"origin", 1}, 2};
    sequence.push_back(a); sequence.push_back(c);
    // No placement rows: origin incidence has no opening/family predicate.
    LiveRowIndex index;
    index.rebuild_from(table, sequence);
    OriginRoster roster;
    roster.push_back(a); roster.push_back(b); roster.push_back(a); roster.push_back(c);
    index.rebuild_origins(7, roster);
    const auto compare_origins = [&] {
        std::vector<std::size_t> positions;
        for (const auto& h : sequence) roster.append_positions_of(h, positions);
        std::sort(positions.rbegin(), positions.rend());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        require(index.origin_positions(7, roster) == positions,
                "origin positions use full handles and descending unique global positions");
    };
    compare_origins();
    sequence.erase_handle(a); index.erase(a); compare_origins();
    sequence.push_back(b); index.insert(b); compare_origins();
    roster.retire_before(2); index.retire_origins_before(7, roster.base()); compare_origins();
    roster.push_back(b); index.append_origin(7, roster.size() - 1, b); compare_origins();
    sequence.push_back(b); index.refresh(b); compare_origins(); // aliases deduplicate positions
    index.rebuild_from(table, sequence); compare_origins();
    roster = OriginRoster::from_retired(roster.size(), roster.digest());
    index.rebuild_origins(7, roster); compare_origins();
}

void i2_predicates() {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    const H h{{"i2-predicate", 1}, 1};
    table.try_emplace(h.incarnation, PlacementSnapshot{});
    LiveHandleSequence sequence;
    sequence.push_back(h);
    LiveRowIndex index;
    index.rebuild_from(table, sequence);
    const auto check = [&](C c, const PlacementSnapshot& r, bool matches) {
        auto replacement = r;
        table.replace(h.incarnation, std::move(replacement));
        index.refresh(h);
        // A single found row contributes either its one handle or no handle.
        // These expected values are hand truth-table arithmetic, independent
        // of both the production predicate table and the test's model switch.
        const std::vector<H> wanted = matches ? std::vector<H>{h} : std::vector<H>{};
        require(index.rows(c) == wanted, LiveRowIndex::class_name(c));
        require(index.count(c) == wanted.size(), "I2 directed predicate count");
        require(model_class(c, r) == matches, "I2 independent predicate witness");
    };
    for (int family = 0; family != 9; ++family) for (bool is_long : {false, true}) {
        PlacementSnapshot r;
        r.family = static_cast<PineOrderFamily>(family);
        r.is_long = is_long;
        r.opening = true;
        r.exit_levels.limit = 50;
        // Every family/side is a pure limit here, including Order and Close.
        check(C::NonPureLimitOpening, r, false);
        r.exit_levels.stop = 1;
        check(C::NonPureLimitOpening, r, true);
        r.opening = false;
        check(C::NonPureLimitOpening, r, false);
        r.opening = true;
        r.exit_levels.stop = std::numeric_limits<double>::quiet_NaN();
        r.exit_levels.trail_offset = 1;
        check(C::NonPureLimitOpening, r, true);
        r.exit_levels.trail_offset = 0;
        check(C::NonPureLimitOpening, r, false);
        r.exit_levels.limit = 0;
        check(C::NonPureLimitOpening, r, true);

        // A closing void leg with finite activation must remain selectable;
        // Opening/ReservationExit/Drop* each reject this positive witness.
        r.opening = false;
        r.void_issue = true;
        r.trail_activation_level = 75;
        r.exit_levels.stop = 50;
        r.exit_levels.limit = 100;
        check(C::ExitStop, r, r.family == PineOrderFamily::ExitStop);
        check(C::ExitLimit, r, r.family == PineOrderFamily::ExitLimit);
    }
    for (auto family : {PineOrderFamily::Entry, PineOrderFamily::Order}) {
        for (bool is_long : {false, true}) {
            PlacementSnapshot base;
            base.family = family;
            base.is_long = is_long;
            base.opening = true;
            base.birth_reach = compat::pine::HistoricalBirthReach::ExtremeWaypoints;
            const double absent[] = {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                -1, -0.0, 0};
            for (const double x : absent) {
                auto r = base;
                r.exit_levels.limit = r.exit_levels.stop = r.exit_levels.trail_points
                    = r.exit_levels.trail_price = r.exit_levels.trail_offset = x;
                check(C::PureMarketAddOpening, r, true);
            }
            for (int field = 0; field != 5; ++field) {
                auto r = base;
                double* levels[] = {&r.exit_levels.limit, &r.exit_levels.stop,
                    &r.exit_levels.trail_points, &r.exit_levels.trail_price, &r.exit_levels.trail_offset};
                *levels[field] = 1;
                check(C::PureMarketAddOpening, r, false);
            }
            auto r = base;
            r.opening = false;
            check(C::PureMarketAddOpening, r, false);
            r = base;
            r.family = PineOrderFamily::Close;
            check(C::PureMarketAddOpening, r, false);
            for (unsigned point : {0U, 3U}) {
                r = base;
                r.birth = OrderBirth::fill_evaluation(0, 0,
                    BirthCursor::point(BirthCursorDomain::HistoricalPath, point, 4), 100, 1, 1, 1);
                check(C::PureMarketAddOpening, r, false);
            }
        }
    }
    PlacementSnapshot r;
    r.opening = true;
    r.requested_qty = std::numeric_limits<double>::infinity();
    // Neither default U2 nor terminal-explicit selection forbids trails.
    r.exit_levels.trail_points = 1;
    check(C::DefaultEntryOpeningU2, r, true);
    r.exit_levels.stop = 1;
    check(C::DefaultEntryOpeningU2, r, false);
    r.exit_levels.stop = 0;
    r.family = PineOrderFamily::Order;
    check(C::DefaultEntryOpeningU2, r, false);
    r.family = PineOrderFamily::Entry;
    r.requested_qty = 1;
    check(C::DefaultEntryOpeningU2, r, false);
    r.qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    r.projection_position_side = static_cast<std::int32_t>(PositionSide::LONG);
    const auto terminal = r;
    check(C::TerminalExplicitMarketEntry, r, true);
    r.oca_type = 1;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.oca_name = "group";
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.birth_reach = compat::pine::HistoricalBirthReach::ExtremeWaypoints;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.family = PineOrderFamily::Order;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.opening = false;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.requested_qty = 0;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.exit_levels.limit = 1;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal; r.exit_levels.stop = 1;
    check(C::TerminalExplicitMarketEntry, r, false);
    r = terminal;
    r.birth = OrderBirth::fill_evaluation(0, 0,
        BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4), 100, 1, 1, 1);
    check(C::TerminalExplicitMarketEntry, r, false);
    if (LiveRowIndex::oracle_enabled()) {
        check(C::TerminalExplicitMarketEntry, terminal, true);
        table.at(h.incarnation).oca_type = 1; // Deliberately omit publication.
        bool caught = false;
        try { (void)index.count(C::TerminalExplicitMarketEntry); }
        catch (const coded<std::logic_error>& error) {
            caught = error.run_failure_code() == RunFailureCode::engine_invariant
                && std::string(error.what()).find("TerminalExplicitMarketEntry") != std::string::npos;
        }
        require(caught, "I2 oracle must name the missed OCA-type publication");
        index.refresh(h);
    }
}

void i2_ordered_selections() {
    PlacementTable table;
    table.fixture_use_memory_auxiliary_tables();
    LiveHandleSequence sequence;
    LiveRowIndex index;
    index.rebuild_from(table, sequence);
    const auto append = [&](PlacementSnapshot r) {
        const H h{{"i2-order", 1}, 1 + sequence.size()};
        table.try_emplace(h.incarnation, std::move(r));
        sequence.push_back(h);
        index.insert(h);
        return h;
    };
    PlacementSnapshot r;
    r.family = PineOrderFamily::ExitStop;
    r.is_long = true;
    r.void_issue = true;
    r.trail_activation_level = 75;
    r.source_id = "exit";
    r.from_entry = "named";
    r.exit_levels.stop = 100;
    const auto high = append(r);
    r.from_entry = "";
    r.exit_levels.stop = 50;
    const auto low = append(r);
    // Live order deliberately opposes price order; both inclusive endpoints
    // match. Exact empty from-entry selects only the second closing leg.
    require(index.rows_in_range(C::ExitStop, LivePrice::Stop, 50, 100, {}, 1)
                == std::vector<H>({high, low}), "I2 closing stops retain live order");
    require(index.rows(C::ExitStop, LiveKey::pair("exit", ""), 1) == std::vector<H>{low},
            "I2 closing-stop exact empty pair");
    r.family = PineOrderFamily::ExitLimit;
    r.is_long = false;
    r.exit_levels.limit = 50;
    const auto limit = append(r);
    require(index.rows_in_range(C::ExitLimit, LivePrice::Limit, 50, 50, {}, 0)
                == std::vector<H>{limit}, "I2 closing-limit side and inclusive price");
    r.family = PineOrderFamily::ExitTrail;
    const auto trail = append(r);
    require(index.rows(C::Trail) == std::vector<H>{trail}, "I2 reuses unrestricted Trail");

    r = PlacementSnapshot{};
    r.opening = true;
    r.is_long = true;
    r.birth_reach = compat::pine::HistoricalBirthReach::ExtremeWaypoints;
    r.source_id = "named";
    r.from_entry = "different";
    const auto entry = append(r);
    r.family = PineOrderFamily::Order;
    r.is_long = false;
    r.source_id = "own";
    const auto order = append(r);
    r.is_long = true;
    r.source_id = "";
    const auto empty = append(r);
    require(index.rows(C::PureMarketAddOpening) == std::vector<H>({entry, order, empty}),
            "I2 market-add wildcard includes Entry, Order and cascade reach");
    require(index.rows(C::PureMarketAddOpening, LiveKey::source("named"), 1) == std::vector<H>{entry},
            "I2 from-entry argument selects source_id, not row.from_entry");
    require(index.rows(C::PureMarketAddOpening, LiveKey::source(""), 1) == std::vector<H>{empty},
            "I2 market-add exact empty source is not a wildcard");
    sequence.move_to_tail(order); index.refresh(order);
    require(index.rows(C::PureMarketAddOpening) == std::vector<H>({entry, empty, order}),
            "I2 market-add enumeration follows live reorder");

    r = PlacementSnapshot{};
    r.opening = true;
    r.is_long = false;
    r.family = PineOrderFamily::Risk;
    r.source_id = "own";
    append(r);
    r.family = PineOrderFamily::Close;
    r.source_id = "foreign";
    append(r);
    r.family = PineOrderFamily::Order;
    r.exit_levels.limit = 50;
    append(r);
    // Short non-pure rows: own Order + own Risk + foreign Close. The foreign
    // pure limit contributes nothing: (3 total) - (2 own) = 1.
    require(index.count(C::NonPureLimitOpening, {}, 0)
                - index.count(C::NonPureLimitOpening, LiveKey::source("own"), 0) == 1,
            "I2 cap subtraction covers all opening families and excludes pure limits");
    compare(index, table, sequence.snapshot());
}
}

int main() {
    try {
        i2_predicates(); i2_ordered_selections();
        edges_and_aliases(); origin_incidence(); stream(); return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "live row index: %s\n", error.what());
        return 1;
    }
}
