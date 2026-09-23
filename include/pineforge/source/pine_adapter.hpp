#pragma once

// Pine-only lowering facts. This header deliberately sits above the generic
// native-host surface: no native kernel type learns a Pine source identifier,
// sizing convention, or lifecycle vocabulary from it.
#include <pineforge/native_host.hpp>
#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/compat/pine/intraday_cap.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
#include <pineforge/compat/pine/reservation_expansion.hpp>
#include <pineforge/source/market_admission.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pineforge::source {

// A2: Pine entry ids and from_entry values are source strings, never a
// kernel-owned identifier type.
using SourceId = std::string;

class PineStrategyHost;
class PineScheduler;

inline constexpr char kSourceAdapterDomain[] = "pineforge-source-adapter/v3";

// The adapter's forced-close vocabulary. These strings are TradingView report
// shape, so they are written and read entirely inside the source layer: the
// kernel records execution::CloseCause instead of decoding them (R5 lane L12,
// 2.ii l).
inline constexpr char kMarginCallLabel[] = "__margin_call__";
inline constexpr char kIntradayLossComment[] = "Close Position (Max intraday Loss)";
// The published C contract matched this PREFIX, not the whole comment, so the
// classifier keeps matching the prefix.
inline constexpr char kFillCapCommentPrefix[] =
    "Close Position (Max number of filled orders";

// TradingView's calc_on_order_fills cascade guard. It is a Pine literal, not a
// kernel default: project() hands it to the generic cadence as
// NativeRunSpec::max_recalculations_per_point, and the adapter's own first-open
// execution chain keeps refusing at the same count.
inline constexpr std::uint32_t kCoofLoopGuard = 1U << 20;

struct PineStrategyConfig {
    bool process_orders_on_close = false;
    bool calc_on_order_fills = false;
    double initial_capital = 1000000.0;
    int default_qty_type = static_cast<int>(QtyType::FIXED);
    double default_qty_value = 1.0;
    int pyramiding = 1;
    double commission_value = 0.0;
    int commission_type = static_cast<int>(CommissionType::PERCENT);
    int slippage = 0;
    double margin_long = 100.0;
    double margin_short = 100.0;
    bool close_entries_rule_any = false;
    bool src_series_active = false;
};

struct StrategyOverrides {
    double initial_capital = std::numeric_limits<double>::quiet_NaN();
    double commission_value = std::numeric_limits<double>::quiet_NaN();
    double default_qty_value = std::numeric_limits<double>::quiet_NaN();
    int pyramiding = -1;
    int slippage = -1;
    int commission_type = -1;
    int default_qty_type = -1;
    int process_orders_on_close = -1;
    int calc_on_order_fills = -1;
    int close_entries_rule = -1;
};

// Value snapshot supplied by PineStrategyHost at the begin boundary. It owns
// every source-side value the adapter needs to form a run spec; none of these
// values are retained by generic native code.
struct StagedConfiguration {
    SymInfo syminfo{};
    InputsMap inputs{};
    std::string chart_timezone{};
    double account_fx = 1.0;
    std::vector<std::int64_t> account_fx_effective_from_ms{};
    std::vector<double> account_fx_per_quote{};
    std::optional<double> quantity_grid{};
};

enum class PineOrderFamily : std::uint8_t {
    Entry = 0,
    Close = 1,
    CloseAll = 2,
    ExitLimit = 3,
    ExitStop = 4,
    ExitTrail = 5,
    Order = 6,
    Margin = 7,
    Risk = 8,
};

struct PineExitLevels {
    double limit = std::numeric_limits<double>::quiet_NaN();
    double stop = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();
};

enum class PineCancellationCause : std::int32_t {
    None = 0,
    Replacement = 1,
    Dependency = 2,
    Admission = 3,
    Explicit = 4,
};

struct PineCancellationReceipt {
    PineCancellationCause cause = PineCancellationCause::None;
    std::int32_t state = 0;
    std::int32_t close_claim_release = 0;
    std::uint64_t source_incarnation = 0;
    std::int64_t source_sequence = 0;
    std::uint64_t target_incarnation = 0;
    std::int64_t target_owner = 0;
    std::uint64_t target_revision = 0;
    double close_claim_consumed = std::numeric_limits<double>::quiet_NaN();
    double close_claim_retired = std::numeric_limits<double>::quiet_NaN();
};

struct PineSizingSnapshot {
    double equity = std::numeric_limits<double>::quiet_NaN();
    double price = std::numeric_limits<double>::quiet_NaN();
    double fx = std::numeric_limits<double>::quiet_NaN();
    double mark = std::numeric_limits<double>::quiet_NaN();
    double frozen_units = std::numeric_limits<double>::quiet_NaN();
    bool at_fill = false;
};

// Source placement evidence keyed by the native request handle, read by the
// const terms/precommit/projection methods. It is written at the
// submit/replace/applied boundaries, and a retained row is also rewritten in
// place after them -- a filled entry's row when its first exit is attached,
// and the adapter's whole-table scans (suspending, reviving or preserving
// brackets) -- so a row is not immutable evidence of its placement.
struct PlacementSnapshot {
    PineOrderFamily family = PineOrderFamily::Entry;
    SourceId source_id{};
    SourceId from_entry{};
    std::string comment{};
    std::string oca_name{};
    int oca_type = 0;
    int qty_type = -1;
    double requested_qty = std::numeric_limits<double>::quiet_NaN();
    // Live projection of a generic OCA reduction.  The original source
    // operand above remains immutable; this receipt-backed value is only for
    // the public pending projection.
    double projection_remaining_qty = std::numeric_limits<double>::quiet_NaN();
    double qty_percent = std::numeric_limits<double>::quiet_NaN();
    bool is_long = true;
    bool immediately = false;
    bool opening = false;
    bool deferred_cohort = false;
    bool reservation_deferred_to_pending_entry = false;
    bool fixed_exit_reservation = false;
    bool frozen_market_instruction = false;
    double frozen_market_own_units = std::numeric_limits<double>::quiet_NaN();
    double frozen_market_transaction_units = std::numeric_limits<double>::quiet_NaN();
    bool frozen_market_targeted_close = false;
    bool frozen_market_target_was_long = false;
    // Command-boundary facts read immutably by the fill-time terms and
    // precommit policies.
    bool direction_gate = false;
    bool affordability_policy_active = false;
    bool affordability_close_only = false;
    bool rounded_signal_cost_close_only = false;
    bool affordability_keep_mc_close_surplus = false;
    bool reverse_to = false;
    bool replaced_opening = false;
    bool replacement_predecessor_market = false;
    bool terms_priced_reverse = false;
    double frozen_reversal_transaction = std::numeric_limits<double>::quiet_NaN();
    std::int64_t placement_cycle = 0;
    std::uint64_t sequential_group = 0;
    std::uint8_t sequential_rank = 0;
    bool has_full_entry_bracket = false;
    native_order::RequestHandle paired_reversal_parent{};
    native_order::RequestHandle preserved_by_close_all{};
    std::int32_t preserved_close_all_bar = -1;
    // Explicit bracket legs retain the source opening provenance that caused
    // their submission.  A pending parent rejected at a later candidate can
    // then retire only its own deferred legs.
    native_order::RequestHandle bracket_origin{};
    std::uint64_t source_sequence = 0;
    std::uint64_t command_ordinal = 0;
    std::uint64_t placement_open_epoch = 0;
    // Command-boundary order is retained separately from native submission
    // order: deferred source commands may materialize after a later command.
    std::uint64_t command_sequence = 0;
    // Source close-callsite lowering facts. They describe one admitted
    // script-evaluation command; the native request remains the sole
    // executable order.
    std::uint64_t close_callsite_token = 0;
    std::uint32_t close_batch_calls = 0;
    SourceId close_first_id{};
    double close_first_target = 0.0;
    bool close_first_ledger_consumed = false;
    bool close_first_carry_valid = false;
    double close_first_carry_qty = 0.0;
    bool close_retire_ledger_whole = false;
    double close_pending_later_qty = 0.0;
    std::int64_t placement_script_open_ms = 0;
    std::int64_t placement_sub_open_ms = 0;
    // Immutable C-row projection facts.  These are source placement facts,
    // not a second executable order: the native request remains the sole
    // owner of matching, trigger state, and terminal receipts.
    std::int32_t projection_created_bar = -1;
    // Set when a source command pinned projection_created_bar itself, so a
    // later submit of the same instance (a bracket leg materialized when its
    // parent entry applies) must not re-stamp it to the submitting bar.
    bool projection_created_bar_pinned = false;
    // Set when a carried bracket leg is force-executed at its own level on the
    // bar its parent entry opened (ab9714be src/source/pine_fills.cpp:7695-7728).
    // The close of such a leg must not inherit the exit bar's path sample: the
    // owner books that fill at step 1, before update_per_trade_extremes().
    bool post_parent_calc_level_fill = false;
    std::int32_t projection_position_side = static_cast<std::int32_t>(PositionSide::FLAT);
    bool projection_after_close = false;
    bool projection_over_pyramiding = false;
    bool projection_opposite_market_predecessor = false;
    std::uint64_t projection_predecessor = 0;
    std::uint64_t recreated_after_named_cancelled_entry_incarnation = 0;
    std::uint64_t named_cancel_surviving_exit_incarnation = 0;
    bool retained_parent_topology = false;
    // A flat child declared before a fresh priced parent was already visited
    // (and skipped) by the legacy whole-bar broker pass.  Keep that source
    // chronology outside the native path matcher until the post-calculation
    // point of the parent's fill bar.
    bool defer_until_post_parent_calculation = false;
    bool projection_predecessor_market = false;
    bool projection_predecessor_exit = false;
    bool projection_created_during_coof = false;
    bool projection_coof_at_terminal = false;
    bool projection_coof_mid_bar = false;
    // An adapter-owned immediate source policy may resolve a generic
    // host-sized close at a previously established broker path price.  The
    // request remains owned and settled by the native core; only its
    // immutable terms fact is source-specific.
    double forced_execution_price = std::numeric_limits<double>::quiet_NaN();
    double projection_tv_carry_qty = 0.0;
    double projection_default_stop_equity = std::numeric_limits<double>::quiet_NaN();
    double projection_default_stop_signal_close = std::numeric_limits<double>::quiet_NaN();
    double projection_explicit_equity = std::numeric_limits<double>::quiet_NaN();
    double projection_explicit_signal_close = std::numeric_limits<double>::quiet_NaN();
    double projection_affordability_equity = std::numeric_limits<double>::quiet_NaN();
    double projection_affordability_signal_price = std::numeric_limits<double>::quiet_NaN();
    double projection_affordability_held_qty = std::numeric_limits<double>::quiet_NaN();
    PineSizingSnapshot sizing{};
    PineExitLevels exit_levels{};
    // Resolved absolute trail activation used by the source fill policy when
    // trail_points is lowered after its parent opening becomes live.
    double trail_activation_level = std::numeric_limits<double>::quiet_NaN();
    // A source reissue that changes only trail_offset keeps the already-seen
    // raw best while the generic request itself remains live.
    double retained_trail_best = std::numeric_limits<double>::quiet_NaN();
    // Immutable source command observation used by the public admission
    // journal/mirror. It never owns or drives matching.
    MarketAdmissionDraft market_admission{};
    // L4c policy receipts. They are immutable placement/live facts owned by
    // the adapter, never a second executable pending-order representation.
    OrderBirth birth{};
    compat::pine::HistoricalBirthReach birth_reach =
        compat::pine::HistoricalBirthReach::Standard;
    ExitLegActivation leg_activation{};
    compat::pine::ExitActivationPolicy exit_activation{};
    exit_legs::Lifecycle legs{};
    bool restored_after_margin = false;
    ReservationExpansion reservation_expansion{};
    ReservationGrowthSource reservation_growth_source{};
    std::uint64_t reservation_growth_owner_incarnation = 0;
    bool stop_limit_activated = false;
    std::int32_t coof_cascade_seg_i = -1;
    bool coof_cascade_inflight_fires = false;
    bool paired_flat_market_candidate = false;
    double paired_flat_market_own_qty = std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_close = std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_equity = std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_margin_pct = std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_pointvalue = std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_fx = std::numeric_limits<double>::quiet_NaN();
    std::int64_t paired_flat_market_peer_seq = 0;
    double paired_flat_market_transaction_qty = std::numeric_limits<double>::quiet_NaN();
    std::int32_t signal_close_mc_bar = -1;
    std::uint64_t signal_close_mc_entry_incarnation = 0;
    std::uint64_t signal_close_mc_fill_seq = 0;
    double signal_close_mc_remaining_qty = std::numeric_limits<double>::quiet_NaN();
    bool pooc_global_full_exit_dynamic_qty = false;
    bool pooc_global_full_exit_tracks_bound_adds = false;
    bool pooc_global_full_exit_bound_add = false;
    PineCancellationReceipt cancellation{};
};

// Source placement handles are monotonically allocated by the native core.
// Retain their evidence in incarnation order rather than in a
// node-per-row hash table: historical re-issued brackets then remain cheap to
// append and lookup without changing the observable key/value collection.
class PlacementTable {
public:
    template<bool IsConst>
    class Iterator {
        using Owner = std::conditional_t<IsConst, const PlacementTable, PlacementTable>;
        using Snapshot = std::conditional_t<IsConst, const PlacementSnapshot, PlacementSnapshot>;
        struct Reference {
            Reference(std::uint64_t key, Snapshot& value) : first(key), second(value) {}

            std::uint64_t first = 0;
            Snapshot& second;
        };

    public:
        // libstdc++ dispatches std::find_if/any_of on iterator_traits; a
        // proxy iterator must still publish the five Cpp17 typedefs (libc++
        // tolerated their absence, GCC on the Cloud Run runner did not).
        using iterator_category = std::forward_iterator_tag;
        using value_type = Reference;
        using difference_type = std::ptrdiff_t;
        using pointer = Reference*;
        using reference = Reference;

        Iterator() = default;

        Reference operator*() const { return {static_cast<std::uint64_t>(index_ + 1U),
                                               *owner_->slots_[index_]}; }
        Reference* operator->() const {
            reference_.emplace(static_cast<std::uint64_t>(index_ + 1U),
                               *owner_->slots_[index_]);
            return &*reference_;
        }
        Iterator& operator++() {
            ++index_;
            skip_empty();
            return *this;
        }
        Iterator operator++(int) {
            Iterator before = *this;
            ++*this;
            return before;
        }
        bool operator==(const Iterator& other) const noexcept {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const Iterator& other) const noexcept { return !(*this == other); }

    private:
        friend class PlacementTable;
        Iterator(Owner* owner, std::size_t index) : owner_(owner), index_(index) { skip_empty(); }
        void skip_empty() {
            while (owner_ && index_ < owner_->slots_.size() && !owner_->slots_[index_]) ++index_;
        }

        Owner* owner_ = nullptr;
        std::size_t index_ = 0;
        mutable std::optional<Reference> reference_;
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    std::size_t size() const noexcept { return size_; }
    std::size_t max_size() const noexcept { return slots_.max_size(); }
    // Largest incarnation ever retained. Incarnations are monotone, so a
    // handle above this mark at an observation time was unknown to every
    // adapter collection populated before that observation.
    std::uint64_t high_water() const noexcept {
        return static_cast<std::uint64_t>(slots_.size());
    }
    void reserve(std::size_t count) { slots_.reserve(count); }
    void clear() noexcept {
        slots_.clear();
        size_ = 0;
        sequence_index_.clear();
        sequence_indexed_slots_ = 0;
    }

    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, slots_.size()); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, slots_.size()); }

    iterator find(std::uint64_t incarnation) noexcept {
        if (incarnation == 0 || incarnation > slots_.size() || !slots_[incarnation - 1U]) return end();
        return iterator(this, static_cast<std::size_t>(incarnation - 1U));
    }
    const_iterator find(std::uint64_t incarnation) const noexcept {
        if (incarnation == 0 || incarnation > slots_.size() || !slots_[incarnation - 1U]) return end();
        return const_iterator(this, static_cast<std::size_t>(incarnation - 1U));
    }

    PlacementSnapshot& at(std::uint64_t incarnation) {
        const auto found = find(incarnation);
        if (found == end()) throw std::out_of_range("source placement handle is absent");
        return found->second;
    }
    const PlacementSnapshot& at(std::uint64_t incarnation) const {
        const auto found = find(incarnation);
        if (found == end()) throw std::out_of_range("source placement handle is absent");
        return found->second;
    }

    template<class... Args>
    std::pair<iterator, bool> try_emplace(std::uint64_t incarnation, Args&&... args) {
        if (incarnation == 0 || incarnation > slots_.max_size()) {
            throw std::length_error("source placement incarnation is out of range");
        }
        const auto index = static_cast<std::size_t>(incarnation - 1U);
        if (index >= slots_.size()) slots_.resize(index + 1U);
        auto& slot = slots_[index];
        if (slot) return {iterator(this, index), false};
        slot.emplace(std::forward<Args>(args)...);
        ++size_;
        // A late handle filling a slot below the index watermark is folded
        // now; the minima are order-independent.
        if (index < sequence_indexed_slots_) {
            try {
                fold_sequence(*slot);
            } catch (...) {
                sequence_index_.clear();
                sequence_indexed_slots_ = 0;
                throw;
            }
        }
        return {iterator(this, index), true};
    }

    // Replaces an existing row wholesale. Its identity fields may change, so
    // the derived sequence index is rebuilt on the next lookup.
    void replace(std::uint64_t incarnation, PlacementSnapshot&& snapshot) {
        at(incarnation) = std::move(snapshot);
        sequence_index_.clear();
        sequence_indexed_slots_ = 0;
    }

    // Smallest command_sequence among the rows whose source_id is `source_id`:
    // over the rows bound to `from_entry` when any is, else over all of them;
    // UINT64_MAX when no row carries the id. Equal to a full scan of the
    // table, but served from a derived per-id index extended over the rows
    // appended since the previous lookup, so a per-bar caller no longer pays
    // O(rows) per call. The index relies on a stored row's source_id /
    // from_entry / command_sequence never being written in place (only
    // replace() rewrites a row); it is a pure lookup over the hashed rows,
    // never next-decision state.
    std::uint64_t command_sequence_for(const std::string& source_id,
                                       const std::string& from_entry) const {
        for (; sequence_indexed_slots_ < slots_.size(); ++sequence_indexed_slots_) {
            const auto& slot = slots_[sequence_indexed_slots_];
            if (slot) fold_sequence(*slot);
        }
        constexpr auto absent = std::numeric_limits<std::uint64_t>::max();
        const auto minima = sequence_index_.find(source_id);
        if (minima == sequence_index_.end()) return absent;
        const auto paired = minima->second.by_from_entry.find(from_entry);
        if (paired != minima->second.by_from_entry.end() && paired->second != absent)
            return paired->second;
        return minima->second.any;
    }

private:
    struct SequenceMinima {
        std::uint64_t any = std::numeric_limits<std::uint64_t>::max();
        std::unordered_map<std::string, std::uint64_t> by_from_entry;
    };

    void fold_sequence(const PlacementSnapshot& row) const {
        auto& minima = sequence_index_[row.source_id];
        minima.any = std::min(minima.any, row.command_sequence);
        const auto inserted = minima.by_from_entry.emplace(row.from_entry, row.command_sequence);
        if (!inserted.second)
            inserted.first->second = std::min(inserted.first->second, row.command_sequence);
    }

    std::vector<std::optional<PlacementSnapshot>> slots_;
    std::size_t size_ = 0;
    // Derived lookup cache for command_sequence_for(); see there.
    mutable std::unordered_map<std::string, SequenceMinima> sequence_index_;
    mutable std::size_t sequence_indexed_slots_ = 0;
};

// Append-only roster of the opening requests a source id ever accepted, in
// acceptance order. Over a long run it holds every historical entry, while
// the pending-origin and live-exposure queries only concern the few handles
// still live; the per-incarnation position index lets those queries visit
// just their positions, in roster order, instead of the whole history.
class OriginRoster {
public:
    using const_iterator = std::vector<native_order::RequestHandle>::const_iterator;
    using const_reverse_iterator =
        std::vector<native_order::RequestHandle>::const_reverse_iterator;

    void push_back(const native_order::RequestHandle& handle) {
        auto& positions = positions_[handle.incarnation];
        positions.push_back(items_.size());
        try {
            items_.push_back(handle);
        } catch (...) {
            positions.pop_back();
            throw;
        }
    }
    const std::vector<native_order::RequestHandle>& members() const noexcept { return items_; }
    const native_order::RequestHandle& operator[](std::size_t index) const { return items_[index]; }
    const native_order::RequestHandle& back() const { return items_.back(); }
    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    const_reverse_iterator rbegin() const noexcept { return items_.rbegin(); }
    const_reverse_iterator rend() const noexcept { return items_.rend(); }

    // Ascending roster positions of the entries equal to `handle`.
    void append_positions_of(const native_order::RequestHandle& handle,
                             std::vector<std::size_t>& out) const {
        const auto found = positions_.find(handle.incarnation);
        if (found == positions_.end()) return;
        for (const auto position : found->second)
            if (items_[position] == handle) out.push_back(position);
    }
    // Ascending roster positions of the entries carrying `incarnation`, or
    // null when none does. Allocation-free.
    const std::vector<std::size_t>* positions_of(std::uint64_t incarnation) const noexcept {
        const auto found = positions_.find(incarnation);
        return found == positions_.end() ? nullptr : &found->second;
    }

private:
    std::vector<native_order::RequestHandle> items_;
    // Derived lookup over items_ (never next-decision state).
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> positions_;
};

// Ordered member list of one source bracket family. Members are appended per
// accepted leg and only removed when cancelled, so the list retains every
// historical leg while removals target the few live ones near its tail. The
// per-incarnation member count bounds a removal to the suffix that holds the
// matching members.
class BracketRoster {
public:
    using const_iterator = std::vector<native_order::RequestHandle>::const_iterator;

    void push_back(const native_order::RequestHandle& handle) {
        auto& count = counts_[handle.incarnation];
        items_.push_back(handle);
        ++count;
    }
    const std::vector<native_order::RequestHandle>& members() const noexcept { return items_; }
    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    // How many members carry `incarnation`. Allocation-free.
    std::size_t count(std::uint64_t incarnation) const noexcept {
        const auto found = counts_.find(incarnation);
        return found == counts_.end() ? 0 : found->second;
    }

    // Erases every member equal to one of `doomed`, keeping the order of the
    // rest: the same result as erase(remove_if(find in doomed)).
    void remove_all(const std::vector<native_order::RequestHandle>& doomed) {
        std::vector<std::uint64_t> incarnations;
        std::size_t sharing = 0;
        for (const auto& handle : doomed) {
            if (std::find(incarnations.begin(), incarnations.end(), handle.incarnation)
                != incarnations.end()) continue;
            incarnations.push_back(handle.incarnation);
            const auto found = counts_.find(handle.incarnation);
            if (found != counts_.end()) sharing += found->second;
        }
        if (sharing == 0) return;
        // Lowest position holding a member that shares a doomed incarnation.
        std::size_t first = items_.size();
        while (sharing > 0 && first > 0) {
            --first;
            if (std::find(incarnations.begin(), incarnations.end(), items_[first].incarnation)
                != incarnations.end()) --sharing;
        }
        std::size_t out = first;
        for (std::size_t in = first; in < items_.size(); ++in) {
            if (std::find(doomed.begin(), doomed.end(), items_[in]) != doomed.end()) {
                const auto found = counts_.find(items_[in].incarnation);
                if (--found->second == 0) counts_.erase(found);
                continue;
            }
            if (out != in) items_[out] = items_[in];
            ++out;
        }
        items_.resize(out);
    }

private:
    std::vector<native_order::RequestHandle> items_;
    // Derived member multiplicity per incarnation (never next-decision state).
    std::unordered_map<std::uint64_t, std::size_t> counts_;
};

struct ShortSeedPlan {
    native_order::RequestHandle long_entry{};
    native_order::RequestHandle materialize_long{};
    native_order::RequestHandle final_short{};
    SourceId seed_id{};
    SourceId long_entry_id{};
    SourceId final_short_id{};
    SourceId materialize_label{};
    double seed_qty = std::numeric_limits<double>::quiet_NaN();
    std::int64_t seed_cycle = 0;
    bool active = false;
    // Generic matching must execute the artifact before the final source
    // short.  For variable-size source books their acceptance handles are
    // consequently opposite to their source report incarnations; retain the
    // pending source-report projection until the remnant is closed.
    bool report_swap_pending = false;
};

// The legacy three-object qualification is evaluated at the next broker open
// (`created_bar + 1 == bar_index`), so the complete source candidate waits
// here until that live fact is available.  It is not an executable order.
struct PendingShortSeedPlan {
    ShortSeedPlan plan{};
    std::uint64_t expected_open_epoch = 0;
    bool ready = false;
};

struct DroppedCloseReceipt {
    SourceId source_id{};
    std::string comment{};
    double qty = std::numeric_limits<double>::quiet_NaN();
    double qty_percent = std::numeric_limits<double>::quiet_NaN();
    bool immediately = false;
    std::uint64_t callsite_token = 0;
    std::uint64_t command_ordinal = 0;
};

struct OpenEntryFeeFact {
    native_order::RequestHandle opening{};
    SourceId source_id{};
    double units = 0.0;
    double nonpercent_fee = 0.0;
};

struct SourceDayLedger {
    std::int64_t current_day = std::numeric_limits<std::int64_t>::min();
    std::int64_t last_loss_day = std::numeric_limits<std::int64_t>::min();
    int consecutive_loss_days = 0;
    std::int64_t intraday_loss_day = std::numeric_limits<std::int64_t>::min();
    double intraday_start_equity = std::numeric_limits<double>::quiet_NaN();
    double intraday_realized = 0.0;
    std::uint64_t observed_applied_ordinal = 0;
};

struct PineRiskState {
    int direction = 0; // 0 both, >0 long, <0 short
    int max_cons_loss_days = 0;
    double max_drawdown = 0.0;
    bool max_drawdown_percent = false;
    double max_intraday_loss = 0.0;
    bool max_intraday_loss_percent = false;
    double max_position_size = 0.0;
    bool halted = false;
    // Runtime facts belong to the source policy, not to the generic
    // settlement kernel.  They are updated from public native projections at
    // the same broker coordinates at which the retired source route updated
    // its latches.
    double observed_peak_equity = std::numeric_limits<double>::quiet_NaN();
    double observed_max_drawdown = 0.0;
    std::int64_t intraday_block_day = std::numeric_limits<std::int64_t>::min();
    bool intraday_cancel_pending = false;
};

// ab9714be pine_risk.cpp:256-292 (update_per_trade_extremes): the source
// host's per-lot excursion sampler. Folds one completed source bar's H/L/C
// into every open lot, honoring the entry-bar masks. Shared by the bar-close
// walk, the stream tick walk and the margin-call submit preload.
void sample_open_trade_extremes(std::vector<PyramidEntry>& lots,
                                PositionSide side, int bar_index, const Bar& bar);
Bar margin_call_sample_bar(const Bar& bar, double fire_price, bool prefix_sample,
                           bool high_first, double mintick = 0.0, int slippage = 0);

class PineExecutionAdapter;

// Allocation-free view facade for Appendix C's later C projection. L2 does
// not wire the C ABI; this only exposes named truthful sources to native
// fixture tests and keeps observer reads side-effect-free.
class PendingIntentView {
public:
    int size() const noexcept;
    int probe_fill_qty(int index, double fill_price, double* qty,
                       int* close_only, int* partition) const noexcept;
    int level_resolved(int index) const noexcept;
    int effective_levels(int index, double* stop, double* limit,
                         double* trail_activation) const noexcept;
    int copy_v1(int index, pf_pending_order_v1_t* out) const noexcept;
    int short_seed_collision_role(int index) const noexcept;
    int last_bar_dual_entry_path() const noexcept;
    double trail_best_price() const noexcept;

private:
    friend class PineExecutionAdapter;
    const PineExecutionAdapter* owner_ = nullptr;
};

class PineExecutionAdapter {
public:
    struct FixturePendingSnapshot {
        std::uint64_t incarnation = 0;
        PlacementSnapshot snapshot{};
        bool staged = false;
    };
    struct FixtureCloseCallsite {
        std::uint64_t token = 0;
        bool active = false;
        double target = 0.0;
        int calls = 0;
        SourceId id{};
        std::string comment{};
        std::uint64_t queue_sequence = 0;
    };
    // Keep the legacy host's construction surface valid until L3a. A null
    // host means this compatibility carrier has no lowering authority.
    explicit PineExecutionAdapter(
            compat::pine::CapAttachment attachment = compat::pine::CapAttachment::None);
    PineExecutionAdapter(NativeStrategyHost& host,
                         compat::pine::CapAttachment attachment = compat::pine::CapAttachment::None);

    void bind(NativeStrategyHost& host) noexcept;
    void reset_for_run();
    void set_configuration(const PineStrategyConfig& config) noexcept;
    void set_staged_configuration(const StagedConfiguration& staged);
    void set_begin_mode(bool is_stream, bool bar_magnifier = false) noexcept;
    void set_path_order(NativePathOrder path_order) noexcept;

    NativeRunSpec project(const PineStrategyConfig&, const StagedConfiguration&,
                          const NativeBeginArgs&,
                          NativePathOrder path_order = NativePathOrder::Auto) const;

    void entry(const SourceId& id, bool is_long,
               double limit_price = std::numeric_limits<double>::quiet_NaN(),
               double stop_price = std::numeric_limits<double>::quiet_NaN(),
               double qty = std::numeric_limits<double>::quiet_NaN(),
               const std::string& comment = {}, const std::string& oca_name = {},
               int oca_type = 0, int qty_type = -1);
    void close(const SourceId& id, const std::string& comment = {},
               double qty = std::numeric_limits<double>::quiet_NaN(),
               double qty_percent = std::numeric_limits<double>::quiet_NaN(),
               bool immediately = false, std::uint64_t callsite_token = 0);
    void close_all();
    void exit(const SourceId& exit_id, const SourceId& from_entry,
              double limit_price, double stop_price,
              double trail_points = std::numeric_limits<double>::quiet_NaN(),
              double trail_offset = std::numeric_limits<double>::quiet_NaN(),
              double trail_price = std::numeric_limits<double>::quiet_NaN(),
              double qty_percent = 100.0, const std::string& comment = {},
              double qty = std::numeric_limits<double>::quiet_NaN(),
              const std::string& oca_name = {},
              double profit_ticks = std::numeric_limits<double>::quiet_NaN(),
              double loss_ticks = std::numeric_limits<double>::quiet_NaN());
    void exit_cancel_bracket(const SourceId& exit_id, const SourceId& from_entry,
                             const std::string& comment = {});
    void cancel(const SourceId& id);
    void cancel_all();
    void order(const SourceId& id, bool is_long, double qty,
               double limit_price = std::numeric_limits<double>::quiet_NaN(),
               double stop_price = std::numeric_limits<double>::quiet_NaN(),
               const std::string& oca_name = {}, int oca_type = 0);

    native_order::ExecutionTerms resolve_terms(const NativeExecutionTermsFacts&) const;
    // R5 lane R4d. anchor_relative_exits runs at the end of a source
    // evaluation: every queued relative strategy.exit whose parent entry is a
    // live kernel request is submitted as that parent's anchored bracket
    // child. resolve_anchored_level is the kernel's once-per-arm level hook:
    // the kernel computes fill + ticks on the tick ladder, and TradingView's
    // spelling of that ladder point and its trigger projection (the half-tick
    // threshold exit() also applies) are restated there.
    void anchor_relative_exits();
    std::optional<double> resolve_anchored_level(const NativeAnchoredLevelView&) const;
    // How many relative legs this run placed as anchored kernel children, how
    // many of those the fill-point re-run adopted as its own request, and how
    // many were withdrawn instead (parent ended, definition changed, or the
    // fill point asked for another request).
    struct AnchoredRelativeStats {
        std::uint64_t anchored = 0;
        std::uint64_t adopted = 0;
        std::uint64_t withdrawn = 0;
    };
    const AnchoredRelativeStats& anchored_relative_stats() const noexcept {
        return anchored_relative_stats_;
    }
    // R5 R2 classification, answered for the command a default-sized MARKET
    // entry on this side would write at the CURRENT execution point: true when
    // the core's native_order::Sized names the whole conversion, so the units
    // that settle are the core's and only the source lot floor and the source
    // admission gates run on top.  Pure in the run's declaration, the staged
    // instrument and that point; nothing is remembered.
    bool core_sizes_default_opening(bool is_long) const;
    NativePrecommitVerdict validate_precommit(const NativePrecommitView&) const;
    // R5: the three policy answers the kernel's margin model asks of its
    // host. `margin_check_allowed` is TradingView's scheduling -- the kernel
    // offers its own check points and the adapter admits only the ones
    // TradingView would also check at; `resolve_margin_requirement` is
    // TradingView's money (its fee-adjusted equity basis and its
    // ten-significant-digit requirement); `resolve_margin_call_units` is its
    // sizing (the lot-floored 4x restore and the family R whole-drop band).
    // TradingView's margin money at one mark: the requirement (on its
    // ten-significant-digit ladder where a lot is worth under one unit of
    // account) against its fee-adjusted live equity. One implementation, read
    // by the adapter's own checkpoints and by the kernel's requirement hook.
    struct SourceMarginMoney {
        bool valid = false;
        double mark = 0.0;
        double held = 0.0;
        double unit_margin = 0.0;
        double exact_required = 0.0;
        double required = 0.0;
        double equity = 0.0;
    };
    SourceMarginMoney source_margin_money(double mark_price,
                                          std::int64_t sub_bar_open_ms) const;
    double source_margin_units(const SourceMarginMoney&, bool opening_checkpoint) const;
    double source_margin_fill_price(double fire, bool close_is_buy) const;

    bool source_margin_rounded_tie_veto() const;
    bool margin_check_allowed(const NativeMarginCheckPoint&) const;
    std::optional<NativeMarginDecision> resolve_margin_requirement(
        const NativeMarginRequirementView&) const;
    std::optional<double> resolve_margin_call_units(const NativeMarginCallView&) const;
    // True when the request is a source exit leg carrying priced stop, limit
    // or trailing terms (L10j): its trade row folds the pre-fill path extremes.
    bool source_priced_exit(std::uint64_t incarnation) const noexcept;
    bool source_post_parent_calc_level_fill(std::uint64_t incarnation) const noexcept;
    std::optional<double> source_trail_offset_ticks(std::uint64_t incarnation) const noexcept;
    bool source_margin_exit(std::uint64_t incarnation) const noexcept;
    static bool source_kernel_liquidation(const native_order::DefinitionRef&) noexcept;
    bool has_pending_market_exit(int current_interval_index = -1) const noexcept;
    // The carried 1x long's opening money call precedes the bar's excursion
    // sample only ahead of one resting full-position priced exit that the
    // open does not reach (ab9714be pine_fills.cpp:164-218).
    bool carried_long_money_precedes_priced_exit(const NativePrecommitView&,
                                                 double held_units) const;
    void on_bar_open(const Bar&, const NativeDecisionContext&);
    void on_tick(const Bar&, const NativeTickContext&);
    // Called from the generic calculation callback after the source script
    // has returned while the current decision point remains executable.
    void on_bar_close(const Bar&, const NativeDecisionContext&);
    void on_applied(const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&);
    void source_batch_end();

    // Fixture/public projection of the chart-timezone day decomposition used
    // by the risk and intraday-cap ledgers.  It carries no generic-kernel
    // policy and lets native-route tests avoid reaching into retired host
    // internals.
    std::int64_t chart_day_key(std::int64_t timestamp_ms) const noexcept;

    int short_seed_collision_role_v1(native_order::RequestHandle) const noexcept;
    const PendingIntentView& pending_intent_view() const noexcept { return pending_view_; }
    // Read-only fixture facade for command-boundary rows that have not yet
    // become live native requests.  It never participates in matching.
    std::vector<FixturePendingSnapshot> fixture_pending_snapshots() const;
    // Fixture-only read of the source cohort's currently live quantity. It
    // projects the adapter's truthful opening facts; it does not recreate the
    // deleted executable id ledger.
    double source_unclosed_qty_for(const SourceId& id) const noexcept {
        return cohort_exposure_for(id);
    }
    int source_entry_slot_count() const noexcept;
    double fixture_close_logical_units(const SourceId&) const noexcept;
    double fixture_close_reserved_units(const SourceId&) const noexcept;
    double fixture_close_first_units(const SourceId&) const noexcept;
    double fixture_callsite_close_reserved_units(
        std::uint64_t, const SourceId&) const noexcept;
    double fixture_callsite_close_first_units(
        std::uint64_t, const SourceId&) const noexcept;
    std::size_t fixture_close_reservation_count() const noexcept;
    std::size_t fixture_close_first_count() const noexcept;
    std::size_t fixture_close_logical_count() const noexcept {
        return close_logical_units_.size();
    }
    std::size_t fixture_callsite_close_reservation_count() const noexcept;
    std::size_t fixture_callsite_close_first_count() const noexcept;
    double fixture_callsite_close_reserved_total() const noexcept;
    double fixture_close_pending_debt() const noexcept {
        return close_batch_pending_debt_;
    }
    double fixture_close_admitted_total() const noexcept {
        return close_batch_admitted_total_;
    }
    std::vector<FixtureCloseCallsite> fixture_close_callsites() const;

    void set_risk_direction(int direction) noexcept;
    void set_risk_max_cons_loss_days(int value) noexcept;
    void set_risk_max_drawdown(double value, bool percent) noexcept;
    void set_risk_max_intraday_loss(double value, bool percent) noexcept;
    void set_risk_max_position_size(double value) noexcept;
    bool allows_risk_direction(bool is_long) const noexcept {
        return risk_.direction == 0 || (is_long ? risk_.direction > 0 : risk_.direction < 0);
    }
    bool max_drawdown_is_percent() const noexcept { return risk_.max_drawdown_percent; }
    bool max_intraday_loss_is_percent() const noexcept {
        return risk_.max_intraday_loss_percent;
    }
    bool take_intraday_loss_relabel(std::uint64_t ordinal) noexcept;
    void set_margin_call_enabled(bool enabled) noexcept;
    // KI-62 (moved out of PyramidEntry by R5 lane L12, 2.ii j): the physical
    // lots opened by request incarnation `incarnation` came from a
    // same-direction MARKET pyramid add — not the base open, not a priced
    // entry.  When a from_entry priced bracket exit fills on such an add's OWN
    // entry bar, and after it in TV's open-tick fill sequence, the exit covers
    // (scratches) the add dur-0.  The reader also gates on
    // `entry_bar_index == bar`, so prior-bar slices are never covered.  This
    // is TradingView provenance, so it is adapter state keyed by the lot's
    // entry incarnation rather than a field on the generic lot.
    void mark_market_pyramid_add(std::uint64_t incarnation);
    bool market_pyramid_add(std::uint64_t incarnation) const noexcept {
        return market_pyramid_adds_.count(incarnation) != 0;
    }
    void enable_intraday_cap() noexcept;
    void attach_execution_adapter() noexcept;
    bool calc_on_order_fills() const noexcept { return config_.calc_on_order_fills; }
    bool process_orders_on_close() const noexcept { return config_.process_orders_on_close; }
    // Read-only L4c fixture observations.  They expose callback coordinates
    // already owned by the adapter; no test path can mutate the native book.
    bool fixture_coof_recalc_active() const noexcept { return coof_recalc_active_; }
    bool fixture_coof_cursor_is_bar_close() const noexcept {
        return coof_recalc_active_ && coof_context_.coordinate.path_phase == NativePathPhase::Close;
    }
    PineCancellationReceipt* fixture_mutable_cancellation(int index) noexcept;
    void fixture_remove_entry_without_named_cancel(const SourceId&);
    void begin_source_evaluation() noexcept {
        named_entry_cancel_tokens_.clear();
        pending_same_bar_close_qty_ = 0.0;
        source_batch_mutated_ = false;
    }
    bool fixture_named_entry_cancel_active(const SourceId& id) const noexcept {
        return named_entry_cancel_tokens_.find(id) != named_entry_cancel_tokens_.end();
    }
    std::vector<native_order::RequestHandle> take_first_open_newborns();
    // Pull terminal generic receipts before a source callback observes the
    // next command boundary.  This retires group-cancelled bracket siblings
    // and pending origins rejected by native admission.
    void observe_terminal_receipts();
    // Called by the fixture scheduler after one source script evaluation so
    // re-priced carried bracket legs retain their original roster order before
    // newly pending-entry legs are appended.
    void flush_pending_bracket_legs(
        native_order::RequestHandle just_applied = {},
        bool post_calculation = true,
        bool pre_script_drain = false);
    // Ordinary POOC same-direction adds are held until the source evaluation
    // closes, so a later close_all in that same evaluation settles first and
    // the add opens the next source position at the same close point.
    void flush_pending_entries();
    void flush_pending_closes();
    void release_delayed_orders(
        bool explicit_brackets_only = false,
        double current_open = std::numeric_limits<double>::quiet_NaN());
    void begin_coof_recalc(const native_order::ExecutionAppliedEvent&,
                           const NativeDecisionContext&, bool first_open,
                           std::uint64_t source_fill_sequence);
    void end_coof_recalc() noexcept;
    bool suppress_grouped_stop_recalc(
        const native_order::ExecutionAppliedEvent&,
        const NativeDecisionContext&) const noexcept;

    std::uint64_t command_sequence_for_exit(const SourceId& exit_id,
                                            const SourceId& from_entry = {}) const noexcept;
    bool is_open_phase_exit(std::size_t trade_index) const noexcept;
    // Reorders trade_exit_phase_[start..) to follow a same-bar exit-group sort:
    // slot start + i takes the phase of the trade previously at indices[i].
    void permute_exit_phases(std::size_t start, const std::vector<std::size_t>& indices);
    void hash_state(BrokerStateHashSink&) const;

    // Retained for the untouched legacy source host. New fixture state is
    // represented by the private maps below rather than this carrier alone.
    compat::pine::IntradayCap cap;
    compat::pine::OrderPriority priority;
    MarketAdmissionJournal admission_journal;

private:
    friend class PendingIntentView;
    friend class PineStrategyHost;
    friend class PineScheduler;
    struct CohortFacts {
        native_order::CohortHandle handle{};
        OriginRoster origins;
        std::vector<native_order::RequestHandle> opened;
        // Live source exposure by opening provenance.  This is source-layer
        // bookkeeping only: the generic core still resolves cohort authority
        // at every candidate.  It lets a command snapshot an existing id's
        // percentage basis before later requests in the same script pass
        // reduce that cohort.
        std::unordered_map<std::uint64_t, double> live_units_by_origin;
        std::int64_t cycle = 0;
    };

    struct PendingBracketLeg {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        std::uint64_t family_key = 0;
    };

    struct PendingEntry {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
    };

    struct DelayedMarketOrder {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        std::uint64_t release_open_epoch = 0;
        bool execute_at_open = false;
    };

    // A36/A28: a sell-side stop that is already marketable at the open, while
    // a buy-side open-marketable stop of the same flat pair is still live,
    // is held out of kernel matching (legacy opposing-stop pass-0 deferral)
    // and admitted after the bar path so later-path same-direction legs can
    // pyramid first. forced_execution_price keeps the open fill.
    struct DeferredOpenMarketableSell {
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        double fill_price = std::numeric_limits<double>::quiet_NaN();
        double path_position = 0.0;
        bool open_marketable = false;
    };

    // The legacy same-bar MARKET transaction is a source-side command batch:
    // all BUY members are admitted before SELL members at the next broker
    // open, while each member retains its placement-time physical quantity.
    // Keep the batch outside the generic core; it contains source ids and the
    // targeted-close artifact that the generic request model must not learn.
    struct PendingSameBarCommand {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        bool opening = false;
    };

    // A source command can remain observable through the enclosing source
    // evaluation after generic admission has already produced its terminal
    // receipt.  This is a source projection row, never a second executable
    // request; it expires at the following broker open.
    struct SourceShadowPending {
        PlacementSnapshot snapshot;
        std::string label;
    };

    struct PendingRelativeExit {
        SourceId exit_id;
        SourceId from_entry;
        double trail_points = std::numeric_limits<double>::quiet_NaN();
        double trail_offset = std::numeric_limits<double>::quiet_NaN();
        double trail_price = std::numeric_limits<double>::quiet_NaN();
        double qty_percent = 100.0;
        std::string comment;
        double qty = std::numeric_limits<double>::quiet_NaN();
        std::string oca_name;
        double profit_ticks = std::numeric_limits<double>::quiet_NaN();
        double loss_ticks = std::numeric_limits<double>::quiet_NaN();
    };

    // R5 lane R4d: one relative leg of a queued strategy.exit, spelled as the
    // kernel's anchored bracket child: the host-sized book close exit()
    // submits at the parent's fill, placed ahead of it (FromOwnerFill under
    // WaitForApplied{parent, PendingUntilArmed, AfterArmPrint, Book}). Until
    // its parent fills it is a kernel request only: no placement row, no live
    // handle, so every source book (reservations, admission, the pending
    // projection) reads exactly what it read before. `installed_level` is the
    // trigger level the arm hook restated; the fill-point exit() re-run
    // adopts the armed request when it is the request it would submit (same
    // trigger bits, same group, a host-sized book close), and withdraws it
    // otherwise.
    struct AnchoredRelativeLeg {
        SourceId exit_id;
        SourceId from_entry;
        PineOrderFamily family = PineOrderFamily::ExitLimit;
        native_order::RequestHandle handle{};
        native_order::RequestHandle parent{};
        bool parent_long = true;
        // The source tick operand this leg was anchored from (profit, loss, or
        // the ceiled trail_points) and, for a trailing leg, the source offset.
        double operand_ticks = 0.0;
        double trail_offset = std::numeric_limits<double>::quiet_NaN();
        native_order::Request request;
        bool armed = false;
        double installed_level = std::numeric_limits<double>::quiet_NaN();
    };

    struct PendingCoofRequest {
        native_order::Request request;
        PlacementSnapshot snapshot;
        SourceId replacement_key;
        bool opening = false;
        std::uint64_t family_key = 0;
        bool next_open = false;
    };

    struct PendingMarginRevival {
        PlacementSnapshot snapshot;
        std::int32_t decline_bar = -1;
    };

    struct NamedEntryCancelToken {
        std::uint64_t entry_incarnation = 0;
        std::uint64_t surviving_exit_incarnation = 0;
    };

    struct CloseCallsiteState {
        bool active = false;
        std::uint64_t token = 0;
        int calls = 0;
        SourceId first_id{};
        double first_target = 0.0;
        bool first_ledger_consumed = false;
        bool first_carry_valid = false;
        double first_carry_qty = 0.0;
        SourceId id{};
        std::string comment{};
        double target = 0.0;
        bool retire_ledger_whole = false;
        std::uint64_t queue_sequence = 0;
        std::vector<SourceId> deferred_cleanup_ids;
    };

    NativeStrategyHost& require_host() const;
    native_order::CohortHandle cohort_for(const SourceId& id);
    std::optional<native_order::RequestHandle> submit_or_replace(
        native_order::Request request, PlacementSnapshot snapshot, bool opening,
        const SourceId& replacement_key = {});
    void remember(const native_order::RequestHandle&, PlacementSnapshot&&);
    void retire(native_order::RequestHandle) noexcept;
    std::vector<native_order::RequestHandle> openings_for(const SourceId&) const;
    double cohort_exposure_for(const SourceId&) const noexcept;
    bool from_entry_filled_this_cycle(const SourceId&) const noexcept;
    double percent_commission_live_equity(double) const noexcept;
    double quantize_close_units(double basis, double percent) const noexcept;
    double quantize_percent_exit_units(double requested,
                                       double available) const noexcept;
    bool compute_exit_reservation(const SourceId& exit_id,
                                  const SourceId& from_entry,
                                  double requested_qty,
                                  double& qty_percent,
                                  double live_basis,
                                  double& reserved_qty) const;
    void reconcile_deferred_exit_reservations(const SourceId& from_entry,
                                               double live_basis);
    double active_staged_fx(std::int64_t) const noexcept;
    void apply_fx_open_margin_slice(const Bar&, const NativeDecisionContext&);
    void apply_fx_opening_margin_slice(const native_order::ExecutionAppliedEvent&,
                                       const NativeDecisionContext&);
    void schedule_preopen_margin_slice(const Bar&, const NativeDecisionContext&);
    bool submit_margin_call_slice(double mark_price, const NativeDecisionContext&,
                                  bool opening_checkpoint = false);
    bool submit_margin_call_units(double mark_price, const NativeDecisionContext&,
                                  double units,
                                  bool force_execution_price = true);
    bool submit_tv_money_long_margin_call(const Bar&, const NativeDecisionContext&);
    bool slipped_pooc_opening_money_scope(
        const Bar&, const NativeDecisionContext&) const;
    bool submit_slipped_pooc_opening_money_call(
        const Bar&, const NativeDecisionContext&);
    bool schedule_tv_money_long_margin_before_trail(
        const Bar&, const NativeDecisionContext&);
    bool market_orders_pending_at_close(const NativeDecisionContext& context,
                                        std::uint64_t except_incarnation = 0) const;
    bool defer_rounded_pooc_short_margin_until_close(const Bar&) const;
    bool declined_reversal_at_open(const Bar&) const;
    bool schedule_margin_call_path(const Bar&, const NativeDecisionContext&);
    void defer_declined_reversal_exits_at_adverse(const Bar&,
                                                  const NativeDecisionContext&,
                                                  bool margin_scheduled);
    bool intraday_loss_breached(double mark_price) const noexcept;
    bool submit_intraday_loss_close(double mark_price, const NativeDecisionContext&,
                                    bool execute_current);
    void schedule_intraday_loss_path(const Bar&, const NativeDecisionContext&);
    void update_risk_state(double mark_price);
    bool intraday_loss_orders_blocked() const noexcept;
    compat::pine::CapClock cap_clock(const NativeDecisionContext&) const;
    compat::pine::Calculation cap_calculation(const NativeDecisionContext&) const;
    compat::pine::MatchedAttempt cap_attempt(
        const PlacementSnapshot&, std::uint64_t incarnation,
        const native_order::ExecutionAppliedEvent* applied = nullptr) const;
    bool cap_placement_denied(const NativeDecisionContext&);
    void observe_intraday_cap(const native_order::ExecutionAppliedEvent&,
                              const PlacementSnapshot&, const NativeDecisionContext&);
    void observe_intraday_cap_noop(bool is_long, const NativeDecisionContext&);
    void execute_cap_close_now(const compat::pine::CloseNow&);
    void execute_due_cap_close(const NativeDecisionContext&);
    void maybe_activate_short_seed_plan();
    void activate_short_seed_plan_at_open(const NativeDecisionContext&);
    bool qualify_short_seed_plan(const ShortSeedPlan&) const;
    bool short_seed_context_is_live() const noexcept;
    void record_dropped_close(const SourceId&, const std::string&, double, double,
                              bool, std::uint64_t);
    void record_opening_fee(const PlacementSnapshot&,
                            const native_order::ExecutionAppliedEvent&);
    void consume_opening_fees(const native_order::ExecutionAppliedEvent&,
                              const SourceId*);
    void consume_cohort_units(const SourceId&, const native_order::ExecutionAppliedEvent&);
    void consume_closed_trade_rows(const native_order::ExecutionAppliedEvent&,
                                   const PlacementSnapshot*);
    bool origin_is_pending(const native_order::RequestHandle&) const noexcept;
    // Roster positions, latest first, of the cohort's origins that are live
    // handles. origin_is_pending() accepts only live handles, so walking these
    // positions meets exactly the pending origins a reverse walk of the whole
    // roster meets, in the same order.
    std::vector<std::size_t> live_origin_positions(const CohortFacts&) const;
    // R5 lane PERF-P7: lookups over the bookkeeping the adapter retains for
    // the whole run -- every origin a cohort ever accepted, every leg an exit
    // family ever placed -- answered from indexes the native consumer keeps
    // for this adapter (NativeHostCache; defined in pine_adapter.cpp), since
    // the adapter is a by-value member of PineStrategyHost and so holds none.
    // Each answers exactly what the walk it replaces answers, and each walks
    // when the consumer keeps no cache. lookup_index() is that cache for this
    // run (built on first use when `create`), or null.
    struct LookupIndex;
    LookupIndex* lookup_index(bool create = true) const noexcept;
    // Whether any origin the cohort ever accepted is an opening on that side.
    bool cohort_opened_on_side(const CohortFacts&, bool is_long) const noexcept;
    // Whether a leg of that kind the (exit id, from_entry) family placed for
    // that origin was consumed: a member of the family neither working nor
    // suspended.
    bool origin_leg_consumed(std::uint64_t family_key, PineOrderFamily,
                             const native_order::RequestHandle& origin) const noexcept;
    // Whether an immediate strategy.close was placed on that bar.
    bool immediate_close_placed_on(std::int32_t bar) const noexcept;
    void cancel_bracket_origin(native_order::RequestHandle);
    void cancel_bracket_siblings(native_order::RequestHandle);
    void cancel_exit_orders_for_full_close(const SourceId& from_entry);
    void retire_in_position_exits_at_flat(bool preserve_pending_parents,
                                          bool dormant_rows_only,
                                          const PlacementSnapshot* paired_close);
    void materialize_relative_exits(PlacementSnapshot,
                                   const native_order::ExecutionAppliedEvent&);
    void withdraw_anchored_relative_legs(const SourceId* exit_id,
                                         const SourceId* from_entry);
    bool anchorable_relative_exit(const PendingRelativeExit&,
                                  native_order::RequestHandle& parent,
                                  bool& parent_long) const;
    struct RelativeLegShape {
        PineOrderFamily family = PineOrderFamily::ExitLimit;
        native_order::Trigger trigger = native_order::Limit{};
        double signed_ticks = 0.0;
        double operand_ticks = 0.0;
    };
    std::vector<RelativeLegShape> relative_leg_shapes(const PendingRelativeExit&,
                                                      bool parent_long) const;
    bool armed_relative_legs_adoptable(const PlacementSnapshot& opening,
                                       const native_order::ExecutionAppliedEvent&,
                                       const std::vector<PendingRelativeExit>&) const;
    // TradingView's trigger projection of an exit level, one spelling for
    // exit() and for the kernel's arm hook: the limit leg's tick-quantized
    // threshold (raw on-grid under calc_on_order_fills) and the one-shot
    // trail activation touch.
    double exit_limit_trigger(double limit_price, double tick, bool exit_is_buy) const noexcept;
    double one_shot_trail_trigger(double activation, double source_trail_offset, double tick,
                                  bool exit_is_buy, bool quantized_activation) const noexcept;
    void materialize_pending_bracket_legs(
        const native_order::ExecutionAppliedEvent&);
    void stage_flat_children_before_parent(const SourceId&, std::int32_t,
                                           std::int64_t);
    bool defer_coof_tail() const noexcept;
    bool source_path_uses_high_first(const Bar&) const noexcept;
    bool coof_fill_on_path_point() const noexcept;
    bool coof_fill_at_path_point(double waypoint) const noexcept;
    bool coof_current_fill_was_forced_waypoint() const noexcept;
    double coof_next_waypoint(int* path_index = nullptr) const noexcept;
    bool coof_remaining_recrosses(double level, bool long_position) const noexcept;
    void flush_coof_tail(bool openings_only = false,
                         bool include_next_open = false);
    native_order::Owner owner_for_close(const SourceId&, bool dynamic) const;
    bool same_bar_market_tx_scope() const;
    void flush_pending_same_bar_commands();
    // The placement-time default quantity: the core's own conversion, read as
    // a query (NativeStrategyHost::native_sized_units) and floored by the
    // source.  The source's money band and affordability gates consume the
    // number before any request exists, which is why it is computed here at
    // all; the arithmetic itself exists once, in the core (R5 N11).
    double default_sizing_units(const PineSizingSnapshot&) const;
    // The three pieces of the default quantity the source keeps as its own
    // policy: the money the percentage is taken of (with its equity mark and
    // its ten-significant-digit rounding), whether a percentage fee is
    // reserved out of it, and the lot floor applied to the core's quotient.
    double default_sizing_cash(const PineSizingSnapshot&) const noexcept;
    bool default_sizing_reserves_percent_fee() const noexcept;
    double default_sizing_lot_floor(double units) const noexcept;
    // The core intent a default-quantity declaration spells -- the source
    // money as a CashValue basis, the core's fee reserve, the raw quotient --
    // and the sided intent a re-lowerable opening carries.  std::nullopt keeps
    // the host-resolved shape for every path the core cannot name.
    std::optional<native_order::Sized> default_sizing_shape(
        const PineSizingSnapshot&) const noexcept;
    std::optional<native_order::Sized> default_sizing_intent(
        const PineSizingSnapshot&, bool is_long) const noexcept;
    // True when the price the core will freeze for a Sized opening accepted
    // NOW is bit-for-bit the price this snapshot already froze.  A command
    // deferred to a later execution point, or one whose sizing price the
    // source took from somewhere other than the signal rule, fails it and
    // keeps its host-resolved intent.
    bool core_sizing_price_matches(const PineSizingSnapshot&, bool is_long) const;
    double default_market_sizing_price(double mark, bool is_long) const noexcept;
    native_order::Trigger trigger_for(double limit_price, double stop_price) const;
    native_order::Group group_for(const std::string&, int, std::int64_t = 0) const;
    PineSizingSnapshot sizing_snapshot() const;
    std::uint64_t key_for(const SourceId&, const SourceId& = {}) const noexcept;
    void refresh_pending_view() noexcept;
    OrderBirth capture_order_birth() const;
    void initialize_l4c_policy(PlacementSnapshot&, native_order::RequestHandle);
    void update_l4c_priority();
    void update_l4c_lifecycle(const native_order::ExecutionAppliedEvent&,
                              const NativeDecisionContext&);
    using ReceiptHighWaterReader = std::uint64_t (*)(const NativeStrategyHost&) noexcept;

    // The consumer's terminal-receipt high water as observe_terminal_receipts
    // last read it, advanced only on a run without an intrabar path (it stays
    // 0 under the magnifier). It no longer gates any read: it only feeds the
    // broker-state hash (pine_state_hash.cpp), so removing it would move
    // hash values.
    std::uint64_t terminal_receipt_cursor_ = 0;
    // Retired (R5 lane PERF-L4): the receipt-reader binding PineStrategyHost
    // installed at every begin, which since lane PERF-P4 only marked "this
    // host is a PineStrategyHost". The readers ask the host's
    // PineStrategyHost view instead, so nothing reads or writes these two
    // slots; they keep this class's layout, which is part of
    // PineStrategyHost's and so of the generated script's ABI.
    ReceiptHighWaterReader event_high_water_reader_ = nullptr;
    ReceiptHighWaterReader terminal_receipt_high_water_reader_ = nullptr;
    bool is_declined_market_reversal(
        const native_order::MatchRejectedEvent&) const noexcept;
    bool follows_same_bar_declined_reversal(
        const PlacementSnapshot&, const NativePrecommitView&) const;
    bool bracket_belongs_to_reversal(
        const PlacementSnapshot&, const PlacementSnapshot&) const noexcept;
    void suspend_brackets_for_reversal(
        const PlacementSnapshot&, const exit_legs::Frame&, double open_price);
    void suspend_declined_reversal_brackets(
        const native_order::MatchRejectedEvent&);
    void suspend_coof_declined_reversal_at_open(
        const Bar&, const NativeDecisionContext&);
    void hold_reversal_pair_brackets(const SourceId&);
    void purge_brackets_after_applied_reversal(const PlacementSnapshot&);
    void revive_brackets_after_margin(
        const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&);
    int projected_pending_size() const noexcept;
    bool projected_pending_at(int index, const PlacementSnapshot*& snapshot,
                              native_order::RequestHandle& handle) const noexcept;
    int projected_raw_pending_size() const noexcept;
    bool projected_raw_pending_at(int index, const PlacementSnapshot*& snapshot,
                                  native_order::RequestHandle& handle) const noexcept;
    static bool same_projected_order(const PlacementSnapshot& left,
                                     const PlacementSnapshot& right) noexcept;
    void apply_open_market_admission(const NativeDecisionContext&);
    void defer_open_marketable_sells(const Bar& bar);
    void admit_deferred_open_marketable_sells();
    void rearm_throttled_reopens();
    void flush_pooc_marketable_limit_entry_fills(const Bar&, const NativeDecisionContext&);
    void flush_pooc_marketable_exit_fills(const Bar&, const NativeDecisionContext&);
    void record_market_review(admission::Checkpoint, int,
                              const std::vector<native_order::RequestHandle>&);
    void refresh_pending_sizing_after_margin(
        const native_order::ExecutionAppliedEvent&, const NativeDecisionContext&);
    void apply_reversal_gap_bracket_policy(
        const Bar&, const NativeDecisionContext&, bool defer_trails = false);
    void reaccept_gapped_bracket_behind_same_id_add(
        const Bar&, const NativeDecisionContext&);
    void apply_terminal_explicit_market_policy(const NativeDecisionContext&);
    bool enqueue_pooc_fifo_close(const SourceId&, const std::string&,
                                 std::uint64_t, std::uint64_t);
    double close_reserved_other_units(const SourceId&,
                                      std::uint64_t) const noexcept;
    void observe_close_policy(const native_order::ExecutionAppliedEvent&,
                              const PlacementSnapshot&);

    // @source-state begin
    NativeStrategyHost* host_ = nullptr;
    PineStrategyConfig config_{};
    StagedConfiguration staged_{};
    NativePathOrder path_order_ = NativePathOrder::Auto;
    mutable std::uint64_t run_counter_ = 0;
    std::uint64_t source_sequence_ = 0;
    std::uint64_t command_ordinal_ = 0;
    std::uint64_t broker_open_epoch_ = 0;
    std::int64_t last_broker_open_ms_ = std::numeric_limits<std::int64_t>::min();
    std::uint64_t source_command_sequence_ = 0;
    std::int32_t entry_attempt_bar_ = -1;
    std::uint32_t entry_attempts_on_bar_ = 0;
    std::unordered_map<SourceId, CohortFacts> cohorts_by_id_;
    std::vector<SourceId> cohort_order_;
    PlacementTable placement_;
    std::unordered_map<std::uint64_t, native_order::RequestHandle> live_by_source_key_;
    std::unordered_map<std::uint64_t, BracketRoster> bracket_families_;
    std::vector<PendingBracketLeg> pending_bracket_legs_;
    std::vector<PendingEntry> pending_entries_;
    std::vector<DelayedMarketOrder> delayed_market_orders_;
    std::vector<DeferredOpenMarketableSell> deferred_open_marketable_sells_;
    // Flat stop entries the per-bar priced-entry throttle refused; re-armed
    // as their original stop at the bar close (validate_precommit is const).
    mutable std::vector<PlacementSnapshot> throttled_reopen_rearm_;
    int entry_openings_interval_index_ = -1;
    int entry_openings_this_interval_ = 0;
    std::vector<PendingSameBarCommand> pending_same_bar_commands_;
    std::vector<SourceShadowPending> source_shadow_pending_;
    double pending_same_bar_close_qty_ = 0.0;
    std::vector<PendingRelativeExit> pending_relative_exits_;
    // Mutable: the kernel's arm hook is const and records the level it
    // installed on the leg it restated.
    mutable std::vector<AnchoredRelativeLeg> anchored_relative_legs_;
    AnchoredRelativeStats anchored_relative_stats_{};
    std::int64_t anchored_cohort_sequence_ = 0;
    std::vector<PendingCoofRequest> pending_coof_requests_;
    std::vector<PendingMarginRevival> pending_margin_revivals_;
    std::vector<native_order::RequestHandle> live_handles_;
    std::vector<native_order::RequestHandle> first_open_newborns_;
    std::vector<DroppedCloseReceipt> dropped_close_receipts_;
    std::vector<OpenEntryFeeFact> open_entry_fees_;
    // Current executions settle synchronously, while their generic Applied
    // notification is delivered after the enclosing callback.  Record the
    // source-cohort debit so a second immediate command sees the new basis,
    // then suppress just that duplicate debit at notification delivery.
    std::unordered_set<std::uint64_t> current_debited_applied_ordinals_;
    std::unordered_set<std::uint64_t> intraday_loss_relabel_ordinals_;
    std::unordered_map<SourceId, std::int64_t> consumed_partial_exit_cycles_;
    std::unordered_set<std::uint64_t> bracket_shadowed_openings_;
    std::unordered_map<SourceId, NamedEntryCancelToken> named_entry_cancel_tokens_;
    std::map<SourceId, double> close_logical_units_;
    std::map<SourceId, double> close_reserved_units_;
    std::map<SourceId, double> close_first_units_;
    std::map<std::uint64_t, std::map<SourceId, double>>
        close_callsite_reserved_units_;
    std::map<std::uint64_t, std::map<SourceId, double>>
        close_callsite_first_units_;
    std::map<std::uint64_t, CloseCallsiteState> close_batch_callsites_;
    std::int32_t close_batch_bar_ = -1;
    std::uint64_t close_batch_queue_sequence_ = 0;
    double close_batch_pending_debt_ = 0.0;
    double close_batch_admitted_total_ = 0.0;
    std::uint64_t receipt_cursor_ = 0;
    std::uint64_t last_applied_ordinal_ = 0;
    bool materializing_relative_ = false;
    // The parent whose fill the current materialize_relative_exits re-run
    // serves; only that parent's armed anchored legs are adoptable.
    native_order::RequestHandle materializing_parent_{};
    std::int64_t current_position_cycle_ = 0;
    int current_position_sign_ = 0;
    std::uint64_t next_sequential_group_ = 0;
    bool source_batch_mutated_ = false;
    bool coof_recalc_active_ = false;
    bool coof_first_open_ = false;
    std::uint64_t coof_market_entry_recalc_incarnation_ = 0;
    std::uint64_t coof_market_entry_recalc_fill_seq_ = 0;
    std::uint64_t coof_current_fill_seq_ = 0;
    double coof_fill_cursor_t_ = std::numeric_limits<double>::quiet_NaN();
    NativeDecisionContext coof_context_{};
    Bar coof_script_bar_{};
    bool coof_script_bar_valid_ = false;
    std::unordered_map<std::int64_t, double> pooc_close_basis_by_script_bar_;
    double pooc_open_basis_ = 0.0;
    std::int64_t pooc_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t close_all_pending_script_bar_ = std::numeric_limits<std::int64_t>::min();
    double last_fx_rate_ = std::numeric_limits<double>::quiet_NaN();
    std::int64_t position_open_script_bar_ = std::numeric_limits<std::int64_t>::min();
    std::uint64_t position_open_epoch_ = 0;
    std::int32_t position_open_bar_index_ = -1;
    NativePathPhase position_open_phase_ = NativePathPhase::None;
    bool position_open_priced_ = false;
    std::int64_t last_margin_call_script_bar_ = std::numeric_limits<std::int64_t>::min();
    // R5: the driver point ordinal whose kernel margin check the adapter's
    // own scheduling admitted. The kernel offers its check points at every
    // bar; exactly the one TradingView also checks at is let through, and
    // only for the driver point it was armed on.
    //
    // Deliberately NOT folded into the broker state hash. Driver point
    // ordinals are strictly monotone within a run and this value is only ever
    // compared for equality with the current point's, so from the first point
    // after the one it names it can never match again: it carries no
    // information across any observation boundary the hash identifies. Folding
    // it would move every adapter state hash (and R1's pinned hash-neutrality
    // evidence in tests/test_adapter_report_relower.cpp) for a value that is
    // dead by the time the next one is taken.
    std::uint64_t kernel_margin_path_point_ = std::numeric_limits<std::uint64_t>::max();
    // The driver point ordinal at which the adapter re-sizes a RESTING kernel
    // liquidation from the post-exit book (ab9714be pine_scheduler.cpp:267-282,
    // the legacy cancel-and-reschedule after a priced bracket leg of this
    // script bar fills). A point named here is admitted only while a slice
    // actually rests, because the legacy broker did nothing at all when none
    // did. Same hash argument as kernel_margin_path_point_ above: a strictly
    // monotone ordinal compared only for equality with the current point's.
    std::uint64_t kernel_margin_resize_point_ = std::numeric_limits<std::uint64_t>::max();
    // Close-time carried-POOC-short checkpoint deferred behind this bar's
    // market fills (ab9714be pine_scheduler.cpp:260 before :278).
    std::int64_t pooc_close_checkpoint_deferred_ms_ = std::numeric_limits<std::int64_t>::min();
    std::int32_t signal_close_mc_event_bar_ = -1;
    std::int64_t signal_close_mc_position_cycle_ = 0;
    std::uint64_t signal_close_mc_entry_incarnation_ = 0;
    std::uint64_t signal_close_mc_fill_seq_ = 0;
    double signal_close_mc_before_qty_ = std::numeric_limits<double>::quiet_NaN();
    double signal_close_mc_remaining_qty_ = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t last_margin_call_event_ordinal_ = 0;
    std::uint64_t last_margin_call_entry_incarnation_ = 0;
    std::int64_t last_margin_call_position_cycle_ = 0;
    bool last_margin_call_at_script_close_ = false;
    double last_margin_call_closed_units_ = 0.0;
    double last_margin_call_remaining_units_ = 0.0;
    std::int64_t risk_coof_direct_script_bar_ = std::numeric_limits<std::int64_t>::min();
    std::uint64_t cap_latest_fill_ = 0;
    bool source_margin_call_enabled_ = true;
    Bar policy_script_bar_{};
    bool policy_script_bar_valid_ = false;
    std::unordered_set<std::uint64_t> market_pyramid_adds_;
    std::unordered_map<std::uint64_t, NativeTrailState> trail_state_at_open_;
    bool stream_mode_ = false;
    SourceDayLedger day_ledger_{};
    PineRiskState risk_{};
    ShortSeedPlan short_seed_{};
    PendingShortSeedPlan pending_short_seed_{};
    native_order::RequestHandle short_seed_long_candidate_{};
    int last_bar_dual_entry_path_ = 0;
    std::int64_t last_bar_dual_entry_script_open_ms_ =
        std::numeric_limits<std::int64_t>::min();
    PendingIntentView pending_view_{};
    std::vector<std::uint8_t> trade_exit_phase_;
    // @source-state end
    // Install-time magnifier fact from NativeBeginArgs. Scheduler already
    // folds retained_.bar_magnifier; this copy is the host-kind-free query
    // for qualify_short_seed_plan.
    bool bar_magnifier_ = false;
};

} // namespace pineforge::source
