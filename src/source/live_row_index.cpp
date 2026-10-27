#include <pineforge/source/detail/live_row_index.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace pineforge::source::detail {
namespace {
using H = native_order::RequestHandle;
using R = PlacementSnapshot;
using C = LiveClass;
bool positive(double x) { return std::isfinite(x) && x > 0.0; }
bool entry(const R& r) { return r.family == PineOrderFamily::Entry; }
bool close(const R& r) { return r.family == PineOrderFamily::Close; }
bool close_all(const R& r) { return r.family == PineOrderFamily::CloseAll; }
bool exit(const R& r) {
    return r.family == PineOrderFamily::ExitLimit || r.family == PineOrderFamily::ExitStop
        || r.family == PineOrderFamily::ExitTrail;
}
bool u2(const R& r) { return !positive(r.exit_levels.limit) && !positive(r.exit_levels.stop); }
bool no_trail(const R& r) {
    return !positive(r.exit_levels.trail_points) && !positive(r.exit_levels.trail_price)
        && !positive(r.exit_levels.trail_offset);
}
bool ordinary_birth(const R& r) {
    return !r.birth.from_fill() && !r.birth.at_terminal_fill()
        && !compat::pine::historical_cascade_reach(r.birth_reach);
}
bool same_point(const R& r) {
    return entry(r) && r.opening
        && r.projection_position_side == static_cast<std::int32_t>(PositionSide::FLAT)
        && positive(r.requested_qty)
        && (r.qty_type < 0 || r.qty_type == static_cast<int>(QtyType::FIXED))
        && r.oca_name.empty() && !r.birth.from_fill()
        && !(positive(r.exit_levels.stop) && positive(r.exit_levels.limit)) && no_trail(r);
}
struct ClassDefinition { const char* name; bool (*matches)(const R&); };
// One additive production table. Keep its order identical to LiveClass.
const ClassDefinition classes[] = {
    {"Placed", [](const R&) { return true; }},
    {"Opening", [](const R& r) { return r.opening; }},
    {"EntryLikeOpening", [](const R& r) {
        return r.opening && (entry(r) || r.family == PineOrderFamily::Order); }},
    {"EntryOpening", [](const R& r) { return entry(r) && r.opening; }},
    {"OrderOpening", [](const R& r) { return r.family == PineOrderFamily::Order && r.opening; }},
    {"CloseOpening", [](const R& r) { return close(r) && r.opening; }},
    {"CloseAllOpening", [](const R& r) { return close_all(r) && r.opening; }},
    {"ExitLimitOpening", [](const R& r) { return r.family == PineOrderFamily::ExitLimit && r.opening; }},
    {"ExitStopOpening", [](const R& r) { return r.family == PineOrderFamily::ExitStop && r.opening; }},
    {"ExitTrailOpening", [](const R& r) { return r.family == PineOrderFamily::ExitTrail && r.opening; }},
    {"MarginOpening", [](const R& r) { return r.family == PineOrderFamily::Margin && r.opening; }},
    {"RiskOpening", [](const R& r) { return r.family == PineOrderFamily::Risk && r.opening; }},
    {"Entry", [](const R& r) { return entry(r); }},
    {"CloseAll", [](const R& r) { return close_all(r); }},
    {"NonImmediateClose", [](const R& r) { return (close(r) || close_all(r)) && !r.immediately; }},
    {"DynamicGlobalExit", [](const R& r) { return r.pooc_global_full_exit_dynamic_qty; }},
    {"ReservationCapture", [](const R& r) { return r.reservation_expansion.capture().has_value(); }},
    {"ReservationEligible", [](const R& r) { return (exit(r) || close(r)) && !r.void_issue; }},
    {"ReservationExit", [](const R& r) { return exit(r) && !r.void_issue; }},
    {"ReservationClose", [](const R& r) { return close(r) && !r.void_issue; }},
    {"WaypointMargin", [](const R& r) { return r.waypoint_margin_call; }},
    {"Trail", [](const R& r) { return r.family == PineOrderFamily::ExitTrail; }},
    {"RestedStopForced", [](const R& r) {
        return positive(r.exit_levels.stop) && (entry(r)
            || (r.family == PineOrderFamily::ExitStop && std::isnan(r.exit_levels.trail_points)
                && std::isnan(r.exit_levels.trail_price) && std::isnan(r.exit_levels.trail_offset)))
            && positive(r.forced_execution_price); }},
    {"EntryOpeningU2", [](const R& r) { return entry(r) && r.opening && u2(r); }},
    {"DefaultEntryOpening", [](const R& r) {
        return entry(r) && r.opening && !std::isfinite(r.requested_qty); }},
    {"ReservationParent", [](const R& r) {
        return entry(r) && r.opening && !r.projection_over_pyramiding
            && positive(positive(r.requested_qty) ? std::abs(r.requested_qty)
                                                 : r.frozen_market_own_units); }},
    {"UnpricedEntry", [](const R& r) {
        return entry(r) && u2(r) && no_trail(r) && ordinary_birth(r); }},
    {"UnpricedClose", [](const R& r) {
        return close(r) && r.oca_name.empty() && u2(r) && no_trail(r) && ordinary_birth(r); }},
    {"ReverseOpeningEntry", [](const R& r) { return entry(r) && r.opening && r.reverse_to; }},
    {"SamePointEligible", [](const R& r) { return same_point(r); }},
    {"SamePointMarket", [](const R& r) { return same_point(r) && u2(r); }},
    {"SamePointStop", [](const R& r) { return same_point(r) && positive(r.exit_levels.stop); }},
    {"SamePointLimit", [](const R& r) { return same_point(r) && positive(r.exit_levels.limit); }},
    {"PureStopEntry", [](const R& r) {
        return entry(r) && positive(r.exit_levels.stop) && !positive(r.exit_levels.limit)
            && no_trail(r); }},
    {"PureBuyLimitOpening", [](const R& r) {
        return entry(r) && r.opening && r.is_long && positive(r.exit_levels.limit)
            && !positive(r.exit_levels.stop) && no_trail(r); }},
    {"TouchedStopEntry", [](const R& r) {
        return entry(r) && positive(r.exit_levels.stop) && !std::isfinite(r.exit_levels.limit)
            && !positive(r.exit_levels.trail_offset); }},
    {"DropLimit", [](const R& r) {
        return r.family == PineOrderFamily::ExitLimit && !std::isfinite(r.trail_activation_level); }},
    {"DropStop", [](const R& r) {
        return r.family == PineOrderFamily::ExitStop && !std::isfinite(r.trail_activation_level); }},
};
static_assert(sizeof(classes) / sizeof(classes[0]) == static_cast<std::size_t>(C::Count));
static_assert(static_cast<std::size_t>(C::Count) <= 64);

[[noreturn]] void mismatch(const char* name) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
        std::string("live-row index invariant: ") + name);
}
void side_check(int side) {
    if (side < -1 || side > 1) throw std::invalid_argument("live-row side must be -1, 0 or 1");
}
bool full_less(const H& a, const H& b) {
    if (a.run.session_key != b.run.session_key) {
        if (a.run.session_key.string() != b.run.session_key.string())
            return a.run.session_key < b.run.session_key;
        // Keep comparator equivalence identical to RequestHandle::operator==
        // even if equal text arrives from separate interning domains.
        return std::less<const std::string*>{}(&a.run.session_key.string(),
                                               &b.run.session_key.string());
    }
    return std::tie(a.run.run_number, a.incarnation)
        < std::tie(b.run.run_number, b.incarnation);
}
template<class Ordered>
std::vector<H> ordered_values(const Ordered& ordered) {
    std::vector<H> result;
    result.reserve(ordered.size());
    for (const auto& item : ordered) result.push_back(item.second);
    return result;
}

#ifndef NDEBUG
// Independent oracle, copied from the original loop conditions. It reads
// canonical rows; neither class table, Record, nor Bucket participates.
bool oracle_class(C c, const R& r) {
    const auto p = [](double x) { return std::isfinite(x) && x > 0; };
    const bool e = r.family == PineOrderFamily::Entry;
    const bool o = r.family == PineOrderFamily::Order;
    const bool cl = r.family == PineOrderFamily::Close;
    const bool ca = r.family == PineOrderFamily::CloseAll;
    switch (c) {
    case C::Placed: return true;
    case C::Opening: return r.opening;
    case C::EntryLikeOpening: return r.opening && (e || o);
    case C::EntryOpening: return r.opening && e;
    case C::OrderOpening: return r.opening && o;
    case C::CloseOpening: return r.opening && cl;
    case C::CloseAllOpening: return r.opening && ca;
    case C::ExitLimitOpening: return r.opening && r.family == PineOrderFamily::ExitLimit;
    case C::ExitStopOpening: return r.opening && r.family == PineOrderFamily::ExitStop;
    case C::ExitTrailOpening: return r.opening && r.family == PineOrderFamily::ExitTrail;
    case C::MarginOpening: return r.opening && r.family == PineOrderFamily::Margin;
    case C::RiskOpening: return r.opening && r.family == PineOrderFamily::Risk;
    case C::Entry: return e;
    case C::CloseAll: return ca;
    case C::NonImmediateClose: return (cl || ca) && !r.immediately;
    case C::DynamicGlobalExit: return r.pooc_global_full_exit_dynamic_qty;
    case C::ReservationCapture: return r.reservation_expansion.capture().has_value();
    case C::ReservationEligible:
        return (cl || r.family == PineOrderFamily::ExitLimit || r.family == PineOrderFamily::ExitStop
            || r.family == PineOrderFamily::ExitTrail) && !r.void_issue;
    case C::ReservationExit:
        return (r.family == PineOrderFamily::ExitLimit || r.family == PineOrderFamily::ExitStop
            || r.family == PineOrderFamily::ExitTrail) && !r.void_issue;
    case C::ReservationClose: return cl && !r.void_issue;
    case C::WaypointMargin: return r.waypoint_margin_call;
    case C::Trail: return r.family == PineOrderFamily::ExitTrail;
    case C::RestedStopForced:
        return p(r.exit_levels.stop) && (e || (r.family == PineOrderFamily::ExitStop
            && std::isnan(r.exit_levels.trail_points) && std::isnan(r.exit_levels.trail_price)
            && std::isnan(r.exit_levels.trail_offset))) && p(r.forced_execution_price);
    case C::EntryOpeningU2:
        return e && r.opening && !p(r.exit_levels.limit) && !p(r.exit_levels.stop);
    case C::DefaultEntryOpening: return e && r.opening && !std::isfinite(r.requested_qty);
    case C::ReservationParent:
        return e && r.opening && !r.projection_over_pyramiding
            && p(p(r.requested_qty) ? std::abs(r.requested_qty) : r.frozen_market_own_units);
    case C::UnpricedEntry:
    case C::UnpricedClose:
        return (c == C::UnpricedEntry ? e : cl && r.oca_name.empty())
            && !p(r.exit_levels.limit) && !p(r.exit_levels.stop)
            && !p(r.exit_levels.trail_points) && !p(r.exit_levels.trail_price)
            && !p(r.exit_levels.trail_offset) && !r.birth.from_fill()
            && !r.birth.at_terminal_fill()
            && !compat::pine::historical_cascade_reach(r.birth_reach);
    case C::ReverseOpeningEntry: return e && r.opening && r.reverse_to;
    case C::SamePointEligible:
    case C::SamePointMarket:
    case C::SamePointStop:
    case C::SamePointLimit:
        return e && r.opening
            && r.projection_position_side == static_cast<std::int32_t>(PositionSide::FLAT)
            && p(r.requested_qty)
            && (r.qty_type < 0 || r.qty_type == static_cast<int>(QtyType::FIXED))
            && r.oca_name.empty() && !r.birth.from_fill()
            && !(p(r.exit_levels.stop) && p(r.exit_levels.limit))
            && !p(r.exit_levels.trail_points) && !p(r.exit_levels.trail_price)
            && !p(r.exit_levels.trail_offset)
            && (c == C::SamePointEligible
                || (c == C::SamePointMarket && !p(r.exit_levels.stop) && !p(r.exit_levels.limit))
                || (c == C::SamePointStop && p(r.exit_levels.stop))
                || (c == C::SamePointLimit && p(r.exit_levels.limit)));
    case C::PureStopEntry:
        return e && p(r.exit_levels.stop) && !p(r.exit_levels.limit)
            && !p(r.exit_levels.trail_points) && !p(r.exit_levels.trail_price)
            && !p(r.exit_levels.trail_offset);
    case C::PureBuyLimitOpening:
        return e && r.opening && r.is_long && p(r.exit_levels.limit) && !p(r.exit_levels.stop)
            && !p(r.exit_levels.trail_points) && !p(r.exit_levels.trail_price)
            && !p(r.exit_levels.trail_offset);
    case C::TouchedStopEntry:
        return e && p(r.exit_levels.stop) && !std::isfinite(r.exit_levels.limit)
            && !p(r.exit_levels.trail_offset);
    case C::DropLimit:
        return r.family == PineOrderFamily::ExitLimit && !std::isfinite(r.trail_activation_level);
    case C::DropStop:
        return r.family == PineOrderFamily::ExitStop && !std::isfinite(r.trail_activation_level);
    case C::Count: break;
    }
    mismatch("unknown class");
}
bool oracle_key(const LiveKey& k, const R& r) {
    switch (k.kind) {
    case LiveKey::Kind::All: return true;
    case LiveKey::Kind::Source: return r.source_id == k.first;
    case LiveKey::Kind::FromEntry: return r.from_entry == k.first;
    case LiveKey::Kind::Pair: return r.source_id == k.first && r.from_entry == k.second;
    case LiveKey::Kind::ScriptTime: return r.placement_script_open_ms == k.number;
    case LiveKey::Kind::CreatedBar: return r.projection_created_bar == k.number;
    case LiveKey::Kind::SourceTime:
        return r.source_id == k.first && r.placement_script_open_ms == k.number;
    case LiveKey::Kind::BracketOrigin: return r.bracket_origin == k.origin;
    case LiveKey::Kind::PerEntryOrigin: return r.per_entry_origin == k.incarnation;
    }
    return false;
}
std::vector<H> oracle_rows(const PlacementTable* table, const LiveHandleSequence* sequence,
                          C c, const LiveKey& key, int side) {
    std::vector<H> result;
    if (!table || !sequence) return result;
    for (const auto& h : *sequence) {
        const auto found = table->find(h.incarnation);
        if (found == table->end()) continue;
        const auto& r = found->second;
        if (oracle_class(c, r) && oracle_key(key, r)
            && (side < 0 || static_cast<int>(r.is_long) == side)) result.push_back(h);
    }
    return result;
}
#endif
}

void LiveHandleSequence::push_back(const Handle& h) {
    if (next_order_ == std::numeric_limits<std::uint64_t>::max()) mismatch("live sequence ordinal exhausted");
    auto inserted = positions_.try_emplace(h);
    auto& positions = inserted.first->second;
    try { positions.push_back(slots_.size()); }
    catch (...) { if (inserted.second) positions_.erase(inserted.first); throw; }
    try { slots_.push_back({h, next_order_ + 1, true, positions.size() - 1}); }
    catch (...) {
        positions.pop_back();
        if (positions.empty()) positions_.erase(inserted.first);
        throw;
    }
    ++next_order_;
    ++live_count_;
    snapshot_valid_ = false;
}
std::size_t LiveHandleSequence::erase_handle(const Handle& h) noexcept {
    const auto found = positions_.find(h);
    if (found == positions_.end()) return 1;
    std::size_t work = found->second.size();
    for (const auto position : found->second) slots_[position].live = false;
    live_count_ -= found->second.size();
    positions_.erase(found);
    snapshot_valid_ = false;
    return work + compact_if_needed();
}
std::size_t LiveHandleSequence::compact_if_needed() noexcept {
    std::size_t work = 0;
    if (positions_.empty()) {
        slots_.clear();
    } else if (slots_.size() - live_count_ > slots_.size() / 2) {
        // Stable in-place compaction, no allocation and no survivor reordering.
        std::size_t target = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            ++work;
            if (!slots_[i].live) continue;
            if (target != i) slots_[target] = slots_[i];
            positions_.find(slots_[target].handle)->second[slots_[target].occurrence] = target;
            ++target;
        }
        slots_.resize(target);
    }
    return work;
}
void LiveHandleSequence::move_to_tail(const Handle& h) {
    const Handle target = h; // The operand may itself refer into slots_.
    if (!contains(target)) return;
    // Append first. push_back rolls back its own allocations; no allocation
    // follows, so an unsuccessful move leaves the original rows untouched.
    push_back(target);
    auto& positions = positions_.find(target)->second;
    for (std::size_t i = 0; i + 1 < positions.size(); ++i) {
        slots_[positions[i]].live = false;
        --live_count_;
    }
    const auto tail = positions.back();
    positions.resize(1);
    positions.front() = tail;
    slots_[tail].occurrence = 0;
    (void)compact_if_needed();
}
void LiveHandleSequence::clear() noexcept {
    slots_.clear(); positions_.clear(); snapshot_.clear();
    snapshot_valid_ = true; next_order_ = 0; live_count_ = 0;
}
std::vector<std::uint64_t> LiveHandleSequence::orders(const Handle& h) const {
    std::vector<std::uint64_t> result;
    for (const auto position : positions_.at(h)) result.push_back(slots_[position].order);
    return result;
}
void LiveHandleSequence::assign(const std::vector<Handle>& handles) {
    LiveHandleSequence replacement;
    for (const auto& h : handles) replacement.push_back(h);
    *this = std::move(replacement);
}
const std::vector<LiveHandleSequence::Handle>& LiveHandleSequence::snapshot() const {
    if (!snapshot_valid_) {
        snapshot_.clear(); snapshot_.reserve(size());
        for (const auto& h : *this) snapshot_.push_back(h);
        snapshot_valid_ = true;
    }
    return snapshot_;
}
std::vector<LiveHandleSequence::Handle> LiveHandleSequence::take() {
    (void)snapshot();
    auto result = std::move(snapshot_);
    clear();
    return result;
}

LiveKey LiveKey::source(const std::string& id) { LiveKey k; k.kind = Kind::Source; k.first = id; return k; }
LiveKey LiveKey::from_entry(const std::string& id) { LiveKey k; k.kind = Kind::FromEntry; k.first = id; return k; }
LiveKey LiveKey::pair(const std::string& a, const std::string& b) {
    LiveKey k; k.kind = Kind::Pair; k.first = a; k.second = b; return k;
}
LiveKey LiveKey::script_time(std::int64_t time) { LiveKey k; k.kind = Kind::ScriptTime; k.number = time; return k; }
LiveKey LiveKey::created_bar(std::int32_t bar) { LiveKey k; k.kind = Kind::CreatedBar; k.number = bar; return k; }
LiveKey LiveKey::source_time(const std::string& id, std::int64_t time) {
    auto k = source(id); k.kind = Kind::SourceTime; k.number = time; return k;
}
LiveKey LiveKey::bracket_origin(H h) { LiveKey k; k.kind = Kind::BracketOrigin; k.origin = h; return k; }
LiveKey LiveKey::per_entry_origin(std::uint64_t h) {
    LiveKey k; k.kind = Kind::PerEntryOrigin; k.incarnation = h; return k;
}
bool LiveKey::operator<(const LiveKey& b) const {
    const auto a_prefix = std::tie(kind, first, second, number, incarnation);
    const auto b_prefix = std::tie(b.kind, b.first, b.second, b.number, b.incarnation);
    return a_prefix < b_prefix || (a_prefix == b_prefix && full_less(origin, b.origin));
}
bool LiveKey::operator==(const LiveKey& b) const { return !(*this < b) && !(b < *this); }

const char* LiveRowIndex::class_name(C c) {
    const auto i = static_cast<std::size_t>(c);
    if (i >= kClasses) throw std::invalid_argument("invalid live-row class");
    return classes[i].name;
}
bool LiveRowIndex::oracle_enabled() noexcept {
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}
bool LiveRowIndex::bound_to(const PlacementTable& table, const LiveHandleSequence& sequence) const noexcept {
    return table_ == &table && sequence_ == &sequence;
}
void LiveRowIndex::clear() {
    records_.clear(); incarnations_.clear(); occurrences_ = 0;
    ordered_members_.clear();
    source_owners_.clear(); keys_by_handle_.clear();
    origins_.clear(); active_origins_.clear(); origin_incidence_.clear();
    for (auto& bucket : buckets_) bucket.clear();
    verify();
}
void LiveRowIndex::rebuild_from(const PlacementTable& table, const LiveHandleSequence& sequence) {
    table_ = &table; sequence_ = &sequence;
    records_.clear(); incarnations_.clear(); occurrences_ = 0;
    ordered_members_.clear();
    active_origins_.clear();
    for (auto& bucket : buckets_) bucket.clear();
    for (const auto& h : sequence) publish(h);
    if (sources_) {
        const auto* canonical = sources_;
        sources_ = nullptr;
        bind_source_keys(*canonical);
    }
    verify();
}
LiveRowIndex::Record LiveRowIndex::project(H h) const {
    Record out;
    out.orders = sequence_->orders(h);
    const auto found = table_->find(h.incarnation);
    if (found == table_->end()) return out;
    const auto& r = found->second;
    for (std::size_t c = 0; c < kClasses; ++c)
        if (classes[c].matches(r)) out.signature |= std::uint64_t{1} << c;
    out.is_long = r.is_long;
    out.command_sequence = r.command_sequence;
    out.cycle = r.placement_cycle;
    out.keys = {{LiveKey{}, LiveKey::source(r.source_id), LiveKey::from_entry(r.from_entry),
        LiveKey::pair(r.source_id, r.from_entry), LiveKey::script_time(r.placement_script_open_ms),
        LiveKey::created_bar(r.projection_created_bar),
        LiveKey::source_time(r.source_id, r.placement_script_open_ms),
        LiveKey::bracket_origin(r.bracket_origin), LiveKey::per_entry_origin(r.per_entry_origin)}};
    out.prices = {{positive(r.exit_levels.stop) ? r.exit_levels.stop : 0.0,
                   positive(r.exit_levels.limit) ? r.exit_levels.limit : 0.0}};
    return out;
}
void LiveRowIndex::add_buckets(H h, const Record& r, std::uint64_t mask) {
    for (const auto order : r.orders) {
        for (std::size_t c = 0; c < kClasses; ++c) {
            if (!(r.signature & mask & (std::uint64_t{1} << c))) continue;
            for (const auto& key : r.keys) {
                auto& b = buckets_[c][key];
                b.rows.emplace(order, h);
                b.sides[r.is_long].emplace(order, h);
                for (std::size_t field = 0; field < 2; ++field)
                    if (positive(r.prices[field]))
                        b.prices[field][r.is_long][r.prices[field]].emplace(order, h);
                b.commands[r.command_sequence].emplace(order, h);
                b.cycles[r.cycle].emplace(order, h);
            }
        }
    }
}
void LiveRowIndex::remove_buckets(const Record& r, std::uint64_t mask) {
    for (const auto order : r.orders) {
        for (std::size_t c = 0; c < kClasses; ++c) {
            if (!(r.signature & mask & (std::uint64_t{1} << c))) continue;
            for (const auto& key : r.keys) {
                auto found = buckets_[c].find(key);
                if (found == buckets_[c].end()) mismatch(classes[c].name);
                auto& b = found->second;
                b.rows.erase(order);
                b.sides[r.is_long].erase(order);
                for (std::size_t field = 0; field < 2; ++field) {
                    if (!positive(r.prices[field])) continue;
                    auto& prices = b.prices[field][r.is_long];
                    auto price = prices.find(r.prices[field]);
                    price->second.erase(order);
                    if (price->second.empty()) prices.erase(price);
                }
                auto command = b.commands.find(r.command_sequence);
                command->second.erase(order);
                if (command->second.empty()) b.commands.erase(command);
                auto cycle = b.cycles.find(r.cycle);
                cycle->second.erase(order);
                if (cycle->second.empty()) b.cycles.erase(cycle);
                if (b.rows.empty()) buckets_[c].erase(found);
            }
        }
    }
}
void LiveRowIndex::remove(H h) {
    const auto found = records_.find(h);
    if (found == records_.end()) return;
    remove_buckets(found->second);
    for (const auto order : found->second.orders) ordered_members_.erase(order);
    set_origin_live(h, false);
    occurrences_ -= found->second.orders.size();
    records_.erase(found);
    auto incidence = incarnations_.find(h.incarnation);
    incidence->second.erase(h);
    if (incidence->second.empty()) incarnations_.erase(incidence);
}
void LiveRowIndex::publish(H h) {
    if (!sequence_->contains(h)) { remove(h); return; }
    auto updated = project(h);
    const auto old = records_.find(h);
    if (old != records_.end()) {
        const auto& before = old->second;
        if (before.orders == updated.orders
            && before.is_long == updated.is_long && before.keys == updated.keys
            && before.prices == updated.prices && before.command_sequence == updated.command_sequence
            && before.cycle == updated.cycle) {
            // Boolean/class edits touch only their signature difference.
            // Unchanged class, identity and price buckets keep their nodes.
            if (before.signature == updated.signature) return;
            remove_buckets(before, before.signature & ~updated.signature);
            add_buckets(h, updated, updated.signature & ~before.signature);
            old->second = std::move(updated);
            return;
        }
        remove_buckets(before);
        for (const auto order : before.orders) ordered_members_.erase(order);
        occurrences_ -= before.orders.size();
        old->second = updated;
    } else {
        records_.emplace(h, updated);
        incarnations_[h.incarnation].insert(h);
        set_origin_live(h, true);
    }
    occurrences_ += updated.orders.size();
    for (const auto order : updated.orders) ordered_members_.emplace(order, h);
    add_buckets(h, updated);
}
void LiveRowIndex::refresh(H h) {
    if (!table_ || !sequence_) mismatch("unbound refresh");
    // The shared placement key is incarnation-only; handle membership is full.
    std::vector<H> affected;
    if (const auto found = incarnations_.find(h.incarnation); found != incarnations_.end())
        affected.assign(found->second.begin(), found->second.end());
    if (sequence_->contains(h)
        && std::find(affected.begin(), affected.end(), h) == affected.end()) affected.push_back(h);
    for (const auto& live : affected) publish(live);
    verify();
}
void LiveRowIndex::insert(H h) { refresh(h); }
void LiveRowIndex::erase(H h) { remove(h); verify(); }
void LiveRowIndex::refresh_incarnation(std::uint64_t incarnation) {
    const auto found = incarnations_.find(incarnation);
    if (found != incarnations_.end()) {
        // publish does not add/remove incidence when the sequence is unchanged.
        const std::vector<H> handles(found->second.begin(), found->second.end());
        for (const auto& h : handles) publish(h);
    }
    verify();
}
void LiveRowIndex::bind_source_keys(const SourceMap& source) {
    if (sources_ == &source) return;
    sources_ = &source;
    source_owners_.clear(); keys_by_handle_.clear();
    for (const auto& row : source) {
        source_owners_.emplace(row.first, row.second);
        keys_by_handle_[row.second].insert(row.first);
    }
    verify();
}
void LiveRowIndex::refresh_source_key(std::uint64_t key) {
    if (!sources_) mismatch("unbound source keys");
    if (const auto old = source_owners_.find(key); old != source_owners_.end()) {
        auto reverse = keys_by_handle_.find(old->second);
        reverse->second.erase(key);
        if (reverse->second.empty()) keys_by_handle_.erase(reverse);
        source_owners_.erase(old);
    }
    if (const auto now = sources_->find(key); now != sources_->end()) {
        source_owners_.emplace(key, now->second);
        keys_by_handle_[now->second].insert(key);
    }
    verify();
}
std::vector<std::uint64_t> LiveRowIndex::source_keys(H h) const {
    std::vector<std::uint64_t> result;
    if (const auto found = keys_by_handle_.find(h); found != keys_by_handle_.end())
        result.assign(found->second.begin(), found->second.end());
#ifndef NDEBUG
    std::vector<std::uint64_t> old;
    if (sources_) for (const auto& row : *sources_) if (row.second == h) old.push_back(row.first);
    auto sorted = result;
    std::sort(sorted.begin(), sorted.end()); std::sort(old.begin(), old.end());
    if (sorted != old) mismatch("source key ownership");
#endif
    return result;
}
void LiveRowIndex::set_origin_live(H h, bool live) {
    const auto found = origin_incidence_.find(h);
    if (found == origin_incidence_.end()) return;
    for (const auto& position : found->second) {
        if (live) active_origins_[position.first][position.second] = h;
        else if (auto active = active_origins_.find(position.first); active != active_origins_.end()) {
            active->second.erase(position.second);
            if (active->second.empty()) active_origins_.erase(active);
        }
    }
}
void LiveRowIndex::clear_origins() {
    origins_.clear(); active_origins_.clear(); origin_incidence_.clear();
    verify();
}
void LiveRowIndex::rebuild_origins(std::uint64_t cohort, const OriginRoster& roster) {
    retire_origins_before(cohort, std::numeric_limits<std::size_t>::max());
    std::size_t global = roster.base();
    for (const auto& h : roster) {
        origins_[cohort].emplace(global, h);
        origin_incidence_[h].emplace(cohort, global);
        if (records_.find(h) != records_.end()) active_origins_[cohort].emplace(global, h);
        ++global;
    }
    verify();
}
void LiveRowIndex::append_origin(std::uint64_t cohort, std::size_t global, H h) {
    auto& roster = origins_[cohort];
    if (const auto before = roster.find(global); before != roster.end()) {
        const auto reverse = origin_incidence_.find(before->second);
        reverse->second.erase({cohort, global});
        if (reverse->second.empty()) origin_incidence_.erase(reverse);
        if (auto active = active_origins_.find(cohort); active != active_origins_.end()) {
            active->second.erase(global);
            if (active->second.empty()) active_origins_.erase(active);
        }
    }
    roster[global] = h;
    origin_incidence_[h].emplace(cohort, global);
    if (records_.find(h) != records_.end()) active_origins_[cohort][global] = h;
    verify();
}
void LiveRowIndex::retire_origins_before(std::uint64_t cohort, std::size_t exclusive) {
    if (const auto found = origins_.find(cohort); found != origins_.end()) {
        auto& roster = found->second;
        for (auto at = roster.begin(); at != roster.end() && at->first < exclusive;) {
            auto reverse = origin_incidence_.find(at->second);
            reverse->second.erase({cohort, at->first});
            if (reverse->second.empty()) origin_incidence_.erase(reverse);
            if (auto active = active_origins_.find(cohort); active != active_origins_.end()) {
                active->second.erase(at->first);
                if (active->second.empty()) active_origins_.erase(active);
            }
            at = roster.erase(at);
        }
        if (roster.empty()) origins_.erase(found);
    }
    verify();
}
std::vector<std::size_t> LiveRowIndex::origin_positions(std::uint64_t cohort, const OriginRoster& roster) const {
    std::vector<std::size_t> result;
    if (const auto found = active_origins_.find(cohort); found != active_origins_.end())
        for (auto at = found->second.rbegin(); at != found->second.rend(); ++at) result.push_back(at->first);
#ifndef NDEBUG
    // Exact old loop text, including incarnation lookup followed by full
    // equality inside append_positions_of; no opening or placement filter.
    std::vector<std::size_t> old;
    if (sequence_) for (const auto& handle : *sequence_) roster.append_positions_of(handle, old);
    std::sort(old.rbegin(), old.rend());
    old.erase(std::unique(old.begin(), old.end()), old.end());
    if (result != old) mismatch("OriginPositions");
#else
    (void)roster;
#endif
    return result;
}

const LiveRowIndex::Bucket* LiveRowIndex::bucket(C c, const LiveKey& key) const {
    (void)class_name(c);
    const auto& directory = buckets_[static_cast<std::size_t>(c)];
    const auto found = directory.find(key);
    return found == directory.end() ? nullptr : &found->second;
}
void LiveRowIndex::check_query(C c, const LiveKey& key, int side, const std::vector<H>& result) const {
#ifndef NDEBUG
    if (result != oracle_rows(table_, sequence_, c, key, side)) mismatch(class_name(c));
#else
    (void)c; (void)key; (void)side; (void)result;
#endif
}
bool LiveRowIndex::contains(H h) const {
    const bool result = records_.find(h) != records_.end();
#ifndef NDEBUG
    bool old = false;
    if (sequence_) for (const auto& live : *sequence_) if (live == h) { old = true; break; }
    if (old != result) mismatch("membership");
#endif
    return result;
}
std::size_t LiveRowIndex::size() const {
#ifndef NDEBUG
    if (occurrences_ != (sequence_ ? sequence_->size() : 0)) mismatch("membership size");
#endif
    return occurrences_;
}
std::vector<H> LiveRowIndex::members() const {
    auto result = ordered_values(ordered_members_);
#ifndef NDEBUG
    const std::vector<H> old = sequence_ ? sequence_->snapshot() : std::vector<H>{};
    if (result != old) mismatch("membership order");
#endif
    return result;
}
std::size_t LiveRowIndex::missing_rows() const {
    const auto placed = count(C::Placed);
    const auto result = size() - placed;
#ifndef NDEBUG
    std::size_t old = 0;
    if (sequence_) for (const auto& h : *sequence_) if (table_->find(h.incarnation) == table_->end()) ++old;
    if (result != old) mismatch("missing placement");
#endif
    return result;
}
std::size_t LiveRowIndex::count(C c, const LiveKey& key, int side) const {
    side_check(side);
    const auto* b = bucket(c, key);
    const auto result = b ? (side < 0 ? b->rows.size() : b->sides[side].size()) : 0;
#ifndef NDEBUG
    if (result != oracle_rows(table_, sequence_, c, key, side).size()) mismatch(class_name(c));
#endif
    return result;
}
std::vector<H> LiveRowIndex::rows(C c, const LiveKey& key, int side) const {
    side_check(side);
    std::vector<H> result;
    if (const auto* b = bucket(c, key)) {
        result = ordered_values(side < 0 ? b->rows : b->sides[side]);
    }
    check_query(c, key, side, result);
    return result;
}
std::optional<double> LiveRowIndex::min_price(C c, LivePrice field, const LiveKey& key, int side) const {
    side_check(side);
    std::optional<double> result;
    if (const auto* b = bucket(c, key)) for (int s = 0; s < 2; ++s) {
        if (side >= 0 && s != side) continue;
        const auto& prices = b->prices[static_cast<std::size_t>(field)][s];
        if (!prices.empty() && (!result || prices.begin()->first < *result)) result = prices.begin()->first;
    }
#ifndef NDEBUG
    std::optional<double> old;
    for (const auto& h : oracle_rows(table_, sequence_, c, key, side)) {
        const auto& r = table_->at(h.incarnation);
        const auto x = field == LivePrice::Stop ? r.exit_levels.stop : r.exit_levels.limit;
        if (std::isfinite(x) && x > 0 && (!old || x < *old)) old = x;
    }
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::optional<double> LiveRowIndex::max_price(C c, LivePrice field, const LiveKey& key, int side) const {
    side_check(side);
    std::optional<double> result;
    if (const auto* b = bucket(c, key)) for (int s = 0; s < 2; ++s) {
        if (side >= 0 && s != side) continue;
        const auto& prices = b->prices[static_cast<std::size_t>(field)][s];
        if (!prices.empty() && (!result || prices.rbegin()->first > *result)) result = prices.rbegin()->first;
    }
#ifndef NDEBUG
    std::optional<double> old;
    for (const auto& h : oracle_rows(table_, sequence_, c, key, side)) {
        const auto& r = table_->at(h.incarnation);
        const auto x = field == LivePrice::Stop ? r.exit_levels.stop : r.exit_levels.limit;
        if (std::isfinite(x) && x > 0 && (!old || x > *old)) old = x;
    }
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::size_t LiveRowIndex::count_in_range(C c, LivePrice field, double low, double high,
                                       const LiveKey& key, int side) const {
    side_check(side);
    std::size_t result = 0;
    if (low <= high) if (const auto* b = bucket(c, key)) for (int s = 0; s < 2; ++s) {
        if (side >= 0 && side != s) continue;
        const auto& prices = b->prices[static_cast<std::size_t>(field)][s];
        for (auto it = prices.lower_bound(low); it != prices.end() && it->first <= high; ++it)
            result += it->second.size();
    }
#ifndef NDEBUG
    std::size_t old = 0;
    for (const auto& h : oracle_rows(table_, sequence_, c, key, side)) {
        const auto& r = table_->at(h.incarnation);
        const auto x = field == LivePrice::Stop ? r.exit_levels.stop : r.exit_levels.limit;
        if (std::isfinite(x) && x > 0 && x >= low && x <= high) ++old;
    }
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::vector<H> LiveRowIndex::rows_in_range(C c, LivePrice field, double low, double high,
                                        const LiveKey& key, int side) const {
    side_check(side);
    Ordered ordered;
    if (low <= high) if (const auto* b = bucket(c, key)) for (int s = 0; s < 2; ++s) {
        if (side >= 0 && side != s) continue;
        const auto& prices = b->prices[static_cast<std::size_t>(field)][s];
        for (auto it = prices.lower_bound(low); it != prices.end() && it->first <= high; ++it)
            ordered.insert(it->second.begin(), it->second.end());
    }
    auto result = ordered_values(ordered);
#ifndef NDEBUG
    std::vector<H> old;
    for (const auto& h : oracle_rows(table_, sequence_, c, key, side)) {
        const auto& r = table_->at(h.incarnation);
        const auto x = field == LivePrice::Stop ? r.exit_levels.stop : r.exit_levels.limit;
        if (std::isfinite(x) && x > 0 && x >= low && x <= high) old.push_back(h);
    }
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::vector<H> LiveRowIndex::before_command(C c, std::uint64_t exclusive,
                                          const LiveKey& key, int side) const {
    side_check(side);
    Ordered ordered;
    if (const auto* b = bucket(c, key))
        for (auto it = b->commands.begin(); it != b->commands.end() && it->first < exclusive; ++it)
            for (const auto& row : it->second)
                if (side < 0 || static_cast<int>(records_.at(row.second).is_long) == side)
                    ordered.insert(row);
    auto result = ordered_values(ordered);
#ifndef NDEBUG
    auto old = oracle_rows(table_, sequence_, c, key, side);
    old.erase(std::remove_if(old.begin(), old.end(), [&](const H& h) {
        return table_->at(h.incarnation).command_sequence >= exclusive;
    }), old.end());
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::size_t LiveRowIndex::count_before_command(C c, std::uint64_t exclusive,
                                              const LiveKey& key, int side) const {
    side_check(side);
    std::size_t result = 0;
    if (const auto* b = bucket(c, key)) {
        for (auto it = b->commands.begin(); it != b->commands.end() && it->first < exclusive; ++it) {
            if (side < 0) result += it->second.size();
            else for (const auto& row : it->second)
                if (static_cast<int>(records_.at(row.second).is_long) == side) ++result;
        }
    }
#ifndef NDEBUG
    std::size_t old = 0;
    for (const auto& h : oracle_rows(table_, sequence_, c, key, side))
        if (table_->at(h.incarnation).command_sequence < exclusive) ++old;
    if (result != old) mismatch(class_name(c));
#endif
    return result;
}
std::vector<H> LiveRowIndex::reservation_rows(const std::string& from, std::int64_t current) const {
    Ordered ordered;
    if (const auto* b = bucket(C::ReservationExit, LiveKey::from_entry(from)))
        ordered.insert(b->rows.begin(), b->rows.end());
    if (const auto* b = bucket(C::ReservationClose, LiveKey::from_entry(from))) {
        if (const auto zero = b->cycles.find(0); zero != b->cycles.end())
            ordered.insert(zero->second.begin(), zero->second.end());
        for (auto it = b->cycles.lower_bound(current); it != b->cycles.end(); ++it)
            if (it->first != 0) ordered.insert(it->second.begin(), it->second.end());
    }
    auto result = ordered_values(ordered);
#ifndef NDEBUG
    auto old = oracle_rows(table_, sequence_, C::ReservationEligible, LiveKey::from_entry(from), -1);
    old.erase(std::remove_if(old.begin(), old.end(), [&](const H& h) {
        const auto& r = table_->at(h.incarnation);
        return r.family == PineOrderFamily::Close && r.placement_cycle != 0 && r.placement_cycle < current;
    }), old.end());
    if (result != old) mismatch(class_name(C::ReservationEligible));
#endif
    return result;
}
std::size_t LiveRowIndex::reservation_count(const std::string& from, std::int64_t current) const {
    std::size_t result = 0;
    if (const auto* b = bucket(C::ReservationExit, LiveKey::from_entry(from))) result += b->rows.size();
    if (const auto* b = bucket(C::ReservationClose, LiveKey::from_entry(from))) {
        if (const auto zero = b->cycles.find(0); zero != b->cycles.end()) result += zero->second.size();
        for (auto it = b->cycles.lower_bound(current); it != b->cycles.end(); ++it)
            if (it->first != 0) result += it->second.size();
    }
#ifndef NDEBUG
    std::size_t old = 0;
    for (const auto& h : oracle_rows(table_, sequence_, C::ReservationEligible, LiveKey::from_entry(from), -1)) {
        const auto& r = table_->at(h.incarnation);
        if (r.family != PineOrderFamily::Close || r.placement_cycle == 0 || r.placement_cycle >= current) ++old;
    }
    if (result != old) mismatch(class_name(C::ReservationEligible));
#endif
    return result;
}
void LiveRowIndex::verify() const {
#ifndef NDEBUG
    for (const auto& cohort : origins_) for (const auto& position : cohort.second) {
        const auto reverse = origin_incidence_.find(position.second);
        if (reverse == origin_incidence_.end() || !reverse->second.count({cohort.first, position.first}))
            mismatch("OriginPositions incidence");
        const auto active = active_origins_.find(cohort.first);
        const bool live = records_.find(position.second) != records_.end();
        const bool indexed = active != active_origins_.end() && active->second.count(position.first);
        if (live != indexed || (indexed && active->second.at(position.first) != position.second))
            mismatch("OriginPositions live membership");
    }
    if (sources_) {
        if (sources_->size() != source_owners_.size()) mismatch("source key ownership size");
        for (const auto& row : *sources_) {
            const auto forward = source_owners_.find(row.first);
            const auto reverse = keys_by_handle_.find(row.second);
            if (forward == source_owners_.end() || forward->second != row.second
                || reverse == keys_by_handle_.end() || !reverse->second.count(row.first))
                mismatch("source key ownership");
        }
    }
    if (!table_ || !sequence_) {
        if (!records_.empty()) mismatch("unbound records");
        return;
    }
    if (occurrences_ != sequence_->size()) mismatch("membership size");
    (void)members(); // Compares the full forward order, including fixture aliases.
    for (const auto& item : records_) {
        const auto& h = item.first;
        const auto found = records_.find(h);
        if (found == records_.end()) mismatch("membership");
        const auto& cached = found->second;
        if (!sequence_->contains(h) || cached.orders != sequence_->orders(h)) mismatch("membership order");
        const auto row = table_->find(h.incarnation);
        for (std::size_t c = 0; c < kClasses; ++c) {
            const bool old = row != table_->end() && oracle_class(static_cast<C>(c), row->second);
            if (old != static_cast<bool>(cached.signature & (std::uint64_t{1} << c)))
                mismatch(classes[c].name);
        }
        if (row == table_->end()) continue;
        const auto& r = row->second;
        if (cached.is_long != r.is_long || cached.command_sequence != r.command_sequence
            || cached.cycle != r.placement_cycle) mismatch("Placed side/command/cycle");
        for (const auto& key : cached.keys) if (!oracle_key(key, r)) mismatch("Placed identity key");
        if (cached.prices[0] != (std::isfinite(r.exit_levels.stop) && r.exit_levels.stop > 0 ? r.exit_levels.stop : 0)
            || cached.prices[1] != (std::isfinite(r.exit_levels.limit) && r.exit_levels.limit > 0 ? r.exit_levels.limit : 0))
            mismatch("Placed positive prices");
    }
    for (std::size_t c = 0; c < kClasses; ++c) {
        const auto expected = oracle_rows(table_, sequence_, static_cast<C>(c), {}, -1);
        const auto* b = bucket(static_cast<C>(c), {});
        if ((b ? ordered_values(b->rows) : std::vector<H>{}) != expected) mismatch(classes[c].name);
    }
#endif
}

} // namespace pineforge::source::detail
