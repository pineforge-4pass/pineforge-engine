/*
 * engine_orders.cpp — the physical lot ledger, the close-row builder and the
 * position-state helpers every settlement path shares
 *
 * Which kernel a matched entry or exit runs through is the execution
 * consumer's choice, settled through this file's lot ledger and close-row
 * builder; the report rows a position still open when the feed ends produces
 * are the consumer's too (record_open_position_report_rows, under the spec's
 * own NativeRunSpec::report_open_position_at_end). This file keeps no entry,
 * exit or range-end helper of its own (ADR-0001 rule 5).
 */
#include "engine_internal.hpp"
#include <pineforge/order_action.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include "retired_trade_history_read.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pineforge {
using namespace internal;

namespace {
// Source predicates are resolved here, never retained by native settlement.
// Every fragment of an opening must agree with the selected source predicate.
template<class Predicate>
std::vector<uint64_t> source_opening_membership(
        const std::vector<PyramidEntry>& lots, Predicate selected) {
    std::unordered_map<uint64_t, bool> membership;
    std::vector<uint64_t> incarnations;
    for (const auto& lot : lots) {
        const bool matches = selected(lot);
        if (lot.entry_incarnation == 0) {
            if (matches)
                throw std::runtime_error("invalid resolved bound-close settlement: unowned opening");
            continue;
        }
        const auto [it, inserted] = membership.emplace(lot.entry_incarnation, matches);
        if (!inserted && it->second != matches)
            throw std::runtime_error("invalid resolved bound-close settlement: heterogeneous opening");
        if (inserted && matches) incarnations.push_back(lot.entry_incarnation);
    }
    return incarnations;
}
} // namespace

void BacktestEngine::append_quoted_lot(PyramidEntry lot, double total_qty,
                                      double average_price) {
    if (position_entry_count_ == std::numeric_limits<int>::max())
        throw std::overflow_error("position entry counter exhausted");
    position_entry_price_ = average_price;
    position_qty_ = total_qty;
    ++position_entry_count_;
    trail_best_price_ = lot.price;
    pyramid_entries_.push_back(std::move(lot));
    if (stream_observe_actions_) stream_observe_entry(pyramid_entries_.back());
}

// ────────────────────────────────────────────────────────────────────
// Shared close-side / position-state helpers
// ────────────────────────────────────────────────────────────────────
// Build a close row using the current source context and resolved price.
// This does not book cash, emit a live trade or update source-day observations;
// range-end reporting also uses this non-mutating row builder.
Trade BacktestEngine::build_close_trade_with_costs(const PyramidEntry& pe, double close_qty,
        double fill_price, bool was_long, double entry_commission,
        double exit_commission,
        const execution::PhysicalExecutionContext& context) const {
    return build_close_trade_with_costs(pe, close_qty, fill_price, was_long,
        entry_commission, exit_commission, context, nullptr, true);
}

Trade BacktestEngine::build_close_trade_with_costs(const PyramidEntry& pe, double close_qty,
        double fill_price, bool was_long, double entry_commission,
        double exit_commission,
        const execution::PhysicalExecutionContext& context,
        const ClosedLotExcursion* retained_excursion, bool consult_excursion_hook) const {
    // Realized PnL scales by the instrument point value ($ per point per
    // contract). Crypto/equity (pointvalue=1) is unchanged; futures (e.g. ES=50)
    // multiply the price-difference PnL. The price-difference component is in
    // the symbol's QUOTE currency; multiply by account_currency_fx_ (default
    // 1.0, no-op for the corpus) to report in the strategy's ACCOUNT currency
    // — same conversion the margin gate (engine_strategy_commands.cpp) already
    // applies. Commission is already account-currency-native (calc_commission
    // applies the same fx factor internally for its PERCENT case; cash-per-*
    // is account-currency by construction), so it is NOT scaled again here.
    const double pv = syminfo_.pointvalue;
    // The row converts at the rate its execution names, or at the presented
    // clock's when it names none (PhysicalExecutionContext::account_fx).
    const double fx = context.account_fx ? *context.account_fx
                                         : active_account_currency_fx();
    double pnl = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                 * close_qty * pv * fx;
    pnl -= entry_commission + exit_commission;
    // Percent P&L is the NET pnl (after both commissions, so computed AFTER
    // the subtraction above -- order matters) over the entry cost,
    // entry_price * qty * pointvalue at the same account-currency rate, so
    // the ratio is currency-invariant and a short's percent is its own
    // return on cost ((entry/exit - 1) was wrong on large moves; a long
    // without commission reduces to the (exit/entry - 1) form). ADR-0001
    // Section B rules this settlement convention; its calibration record is
    // a TradingView export (2026-06-12, trade #258 short: 102.44 USD on a
    // 2276.66 entry is 4.50%).
    const double entry_cost = pe.price * close_qty * pv
                              * fx;
    double pnl_pct = (entry_cost > 0.0) ? (pnl / entry_cost) * 100.0 : 0.0;

    Trade trade;
    trade.entry_time = pe.time;
    trade.exit_time = context.effective_time_ms;
    trade.entry_price = pe.price;
    trade.exit_price = fill_price;
    trade.qty = close_qty;
    trade.pnl = pnl;
    trade.pnl_pct = pnl_pct;
    trade.is_long = was_long;
    trade.entry_bar_index = pe.entry_bar_index;
    trade.exit_bar_index = context.interval_index;
    trade.entry_id = pe.entry_id;
    trade.entry_incarnation = pe.entry_incarnation;
    trade.entry_comment = pe.entry_comment;
    trade.commission = entry_commission + exit_commission;
    // Excursions: a closing row's excursion includes its own exit fill -- a
    // stop-out's adverse excursion is at least the loss at the stop fill and
    // a take-profit's favorable excursion includes the move to the limit
    // fill. The per-lot sampler (NativeExecutionConsumer::apply_excursion)
    // cannot see it: the exit fills inside request matching and the lot
    // leaves the book before the next sample, so a same-bar entry and exit
    // would otherwise report 0/0. Fold the fill price in here. The carried
    // per-lot extreme is scaled to the closed slice (close_qty/pe.qty), so a
    // partial close reports its own slice. Both fields stay >= 0
    // (magnitudes); a signed export is its writer's business.
    const double slice = (pe.qty > 0.0) ? (close_qty / pe.qty) : 1.0;
    const double fill_fav = (was_long ? (fill_price - pe.price) : (pe.price - fill_price))
                      * close_qty;
    if (retained_excursion != nullptr) {
        trade.max_runup = retained_excursion->favorable;
        trade.max_drawdown = retained_excursion->adverse;
        return trade;
    }
    if (consult_excursion_hook && lot_excursion_hook_) {
        // The host owns this lot's excursion (RULING A48): it sampled the
        // lot's path itself and answers the two magnitudes of the closing
        // row, which the row records exactly as answered -- facts in,
        // magnitudes out. The kernel contributes the booking facts, the entry
        // fee share among them, and applies no reporting basis of its own.
        ClosedLotExcursionFacts facts;
        facts.entry_incarnation = pe.entry_incarnation;
        facts.entry_time_ms = pe.time;
        facts.entry_price = pe.price;
        facts.lot_qty = pe.qty;
        facts.closed_qty = close_qty;
        facts.fill_price = fill_price;
        facts.carried_favorable = pe.max_runup;
        facts.carried_adverse = pe.max_drawdown;
        facts.is_long = was_long;
        facts.entry_bar_index = pe.entry_bar_index;
        facts.exit_bar_index = context.interval_index;
        facts.entry_bar_high_masked = pe.skip_entry_bar_high;
        facts.entry_bar_low_masked = pe.skip_entry_bar_low;
        facts.entry_commission = entry_commission;
        facts.account_fx = fx;
        const ClosedLotExcursion owned = lot_excursion_hook_(facts);
        trade.max_runup = owned.favorable;
        trade.max_drawdown = owned.adverse;
        return trade;
    }
    double runup = std::max(pe.max_runup * slice, fill_fav);
    double drawdown = std::max(pe.max_drawdown * slice, -fill_fav);
    // A priced (stop / limit / trail) exit fills mid-bar, before that bar's
    // own per-trade extreme sampling, so a bar-path extreme the modeled path
    // reaches ahead of the fill is seen by nothing else. The one such extreme
    // the settling path carries is the trail's: the peak that armed the
    // trailing stop (fill +/- offset) is a pre-fill favorable excursion no
    // bar-boundary sample ever sees.
    if (context.preceding_exit_trail_peak) {
        const double peak = *context.preceding_exit_trail_peak;
        double peak_fav = (was_long ? (peak - pe.price) : (pe.price - peak)) * close_qty;
        runup = std::max(runup, peak_fav);
    }
    // The kernel's own model only -- an excursion owner's answer is recorded
    // above as given, on whatever basis the owner reports.
    // Excursions are reported on the NET open-profit basis: open profit at
    // the entry tick is already -commission, so the entry-leg commission
    // shrinks the favorable extreme, floored at 0, and grows the adverse
    // one. ADR-0001 Section B rules this settlement convention; its
    // calibration record is TradingView's export of
    // pyramid-cash-fractional-commission-01 (every trade differs from the
    // gross excursion by exactly qty * cash_per_contract, both columns) and
    // the corpus's 757k rows (none exports a negative favorable excursion).
    // Both fields remain >= 0 here.
    // runup/drawdown are quote-currency (price-diff × qty); convert to
    // account currency via account_currency_fx_ (default 1.0, no-op) before
    // combining with entry_commission, which is already account-currency
    // (see calc_commission) — same convention as pnl above.
    trade.max_runup = std::max(
        0.0, runup * pv * fx - entry_commission);
    // Excursion columns are nonnegative magnitudes even when an observed
    // entry rebate is negative. Keep positive-fee behavior unchanged while
    // preventing a rebate from producing an impossible negative drawdown.
    trade.max_drawdown = std::max(
        0.0, drawdown * pv * fx
                 + entry_commission);
    return trade;
}

// Settlement SAFETY GUARD (an invariant), not Pine parity: a closed or marked
// row whose money, price, quantity or excursion figure is not finite must never
// be committed or presented. Generic over the kernel's own Trade row; every
// settlement or close entry point calls it (the census is in the source notes).
bool internal::closed_row_all_finite(const Trade& trade) noexcept {
    return std::isfinite(trade.entry_price)
        && std::isfinite(trade.exit_price)
        && std::isfinite(trade.qty)
        && std::isfinite(trade.pnl)
        && std::isfinite(trade.pnl_pct)
        && std::isfinite(trade.max_runup)
        && std::isfinite(trade.max_drawdown)
        && std::isfinite(trade.commission);
}




void BacktestEngine::record_close_trade(Trade trade) {
    validate_close_trade_counters(&trade, 1);
    const double trade_pnl = trade.pnl;
    trades_.push_back(std::move(trade));
    if (stream_observe_actions_) stream_observe_exit(trades_.size() - 1);
    const double previous_net_profit = net_profit_sum_;
    net_profit_sum_ += trade_pnl;
    // Knuth TwoSum recovers this addition's exact binary64 residual without
    // changing the authoritative cached sum. Accumulate absolute residuals
    // upward so vanished intermediate profits cannot disappear from the
    // uncertainty used by a later rounded-money margin comparison.
    if (previous_net_profit == net_profit_roundoff_value_
        && std::isfinite(previous_net_profit) && std::isfinite(trade_pnl)
        && std::isfinite(net_profit_sum_)
        && std::isfinite(net_profit_roundoff_bound_)) {
        const double virtual_pnl = net_profit_sum_ - previous_net_profit;
        const double residual =
            (previous_net_profit - (net_profit_sum_ - virtual_pnl))
            + (trade_pnl - virtual_pnl);
        if (std::isfinite(residual)) {
            if (residual != 0.0) {
                net_profit_roundoff_bound_ = std::nextafter(
                    net_profit_roundoff_bound_ + std::abs(residual),
                    std::numeric_limits<double>::infinity());
            }
        } else {
            net_profit_roundoff_bound_ = std::numeric_limits<double>::infinity();
        }
    } else {
        net_profit_roundoff_bound_ = std::numeric_limits<double>::infinity();
    }
    net_profit_roundoff_value_ = net_profit_sum_;
    if (trade_pnl > 0) { gross_profit_sum_ += trade_pnl; win_trades_count_++; }
    else if (trade_pnl < 0) { gross_loss_sum_ += trade_pnl; loss_trades_count_++; }
    else { ++eventrades_count_; }  // strategy.eventrades: P&L exactly zero (== 0, no epsilon)
}

void BacktestEngine::validate_close_trade_counters(const Trade* rows, size_t count) const {
    const int maximum = std::numeric_limits<int>::max();
    // In an ordinary run each counter can advance at most once per row.
    // Only an exhausted/near-exhausted counter needs the exact financial walk.
    if (count <= static_cast<size_t>(maximum)) {
        const int room = maximum - static_cast<int>(count);
        if (win_trades_count_ <= room && loss_trades_count_ <= room
            && eventrades_count_ <= room)
            return;
    }
    int64_t wins = win_trades_count_, losses = loss_trades_count_;
    int64_t evens = eventrades_count_;
    for (size_t i = 0; i < count; ++i) {
        const double pnl = rows[i].pnl;
        if (pnl > 0.0) {
            ++wins;
        } else if (pnl < 0.0) {
            ++losses;
        } else {
            ++evens;
        }
        if (wins > maximum || losses > maximum || evens > maximum)
            throw std::overflow_error("closed trade counter exhausted");
    }
}


// Reset all per-position state after the position is fully closed: the native
// settlement commit calls it when a close leaves no surviving lot
// (commit_prepared_native_settlement_stage, engine_execution.cpp), and
// reset_run_state starts every run from it.
void BacktestEngine::reset_position_state_to_flat() {
    position_side_ = PositionSide::FLAT;
    position_cycle_seq_ = 0;
    position_entry_price_ = 0.0;
    opening_obligations_.invalidate();
    position_entry_time_ = 0;
    position_qty_ = 0.0;
    position_entry_count_ = 0;
    position_open_bar_ = -1;
    trail_best_price_ = std::numeric_limits<double>::quiet_NaN();
    pyramid_entries_.clear();
}

// Establish a fresh position at fill_price/qty after a transition from FLAT
// or a same-bar close. Resets all per-position state and seeds the first
// pyramid entry. Used by every entry path that opens a brand-new position
// (FLAT entry, close-only-opposite remainder, opposite flip).
void BacktestEngine::open_quoted_position(PositionSide requested, PyramidEntry lot) {
    if (next_position_cycle_seq_ <= 0
        || next_position_cycle_seq_ == std::numeric_limits<int64_t>::max())
        throw std::overflow_error("position cycle sequence exhausted");
    position_side_ = requested;
    position_cycle_seq_ = next_position_cycle_seq_++;
    position_entry_price_ = lot.price;
    // The shared post-dispatch lifecycle hook queues the new fill's event.
    // Clear prior-cycle provenance now so reversals cannot expose it even
    // transiently.
    opening_obligations_.invalidate();
    position_entry_time_ = lot.time;
    position_qty_ = lot.qty;
    position_entry_count_ = 1;
    position_open_bar_ = lot.entry_bar_index;
    trail_best_price_ = lot.price;
    pyramid_entries_.clear();
    pyramid_entries_.push_back(std::move(lot));
    if (stream_observe_actions_) stream_observe_entry(pyramid_entries_.back());
}

// The execute_market_entry case helpers are gone: growth on a deferred flip
// survives only as a frozen pending-row field name, the opening gate is
// NativeMarginModel::initial_long / initial_short, and the order of two fills
// at one point is the consumer's acceptance and incarnation ordinals
// (ADR-0001, detached comment residue).

inline namespace engine_script_run_v19 {

struct TradeLedger::State {
    // The directory is itself external: fixed-width row descriptors are
    // addressed by global index; variable strings live in the payload file.
    // Updating an old mutable row replaces its descriptor after its new
    // payload has been written. No in-memory vector of offsets is needed.
    struct StringExtent { std::uint64_t offset = 0, length = 0; };
    struct Record {
        std::int64_t entry_time = 0, exit_time = 0;
        double entry_price = 0, exit_price = 0, qty = 0, pnl = 0, pnl_pct = 0;
        std::int32_t entry_bar_index = -1, exit_bar_index = -1;
        StringExtent entry_id, entry_comment, exit_comment, exit_id;
        double max_runup = 0, max_drawdown = 0, commission = 0;
        std::uint64_t entry_incarnation = 0;
        execution::CloseCause close_cause = execution::CloseCause::Unspecified;
        bool is_long = false, exit_from_bracket = false, open_at_end = false;
    };
    static_assert(std::is_trivially_copyable_v<Record>);
    static_assert(TradeLedger::kRecordBytes == sizeof(Record));
    static_assert(sizeof(int) == sizeof(std::int32_t));
    // The row window holds fixed descriptors; even the newest row's four
    // payloads already live in scratch. Cache only a fixed byte tail below.
    static constexpr size_type kPayloadTailBytes = kResidentRows * sizeof(Record);
    std::array<Record, kResidentRows> rows{};
    size_type first = 0, end = 0, resident_first = 0, count = 0, head = 0;
    detail::ScratchFile index, payload;
    std::uint64_t payload_end = 0;
    // Physical origins only. Record keys, text extents, size/base, all money
    // folds and every hash remain in the original global coordinate space.
    std::uint64_t index_origin = 0, payload_origin = 0, payload_retired = 0;
    bool retiring = false;
    std::array<char, kPayloadTailBytes> payload_tail{};
    std::string directory;
    mutable std::map<size_type, Trade> materialized;
    // Packed owning string bytes, rather than std::strings whose capacities
    // could exceed the advertised bound. Record retains exact numeric bits
    // and four string lengths; offsets remain scratch-only metadata.
    static constexpr size_type kReferenced = 2;
    struct CacheEntry {
        Record record{};
        size_type global = 0, generation = 0;
        std::array<char, kCacheStringBytes> strings{};
        // Generation bases are multiples of four. Bit 0 marks packed strings
        // ready; bit 1 is the reference bit. Neither adds entry storage.
        bool strings_valid() const { return (generation & size_type{1}) != 0; }
    };
    static_assert(sizeof(CacheEntry) == kLedgerEntryBytes);
    size_type cache_rows = kDefaultCacheRows;
    mutable std::unique_ptr<CacheEntry[]> cache;
    mutable size_type cache_count = 0, cache_capacity = 0;
    mutable size_type cache_generation = 4;
    // A block can contain more rows than a deliberately small configured
    // cache. Keep the requested row available to the current accessor after
    // later rows replace its direct-mapped slot.
    mutable CacheEntry block_result{};
    static constexpr size_type kBlockPayloadBytes = kResidentRows * kCacheStringBytes;

    // Numeric scans retain descriptors independently of cache-slot admission.
    // This fixed window adds 64,528 bytes on 64-bit targets, including its
    // two keys; it neither allocates nor retains payload bytes.
    struct NumericWindow {
        std::array<Record, kNumericWindowRows> records{};
        size_type first = 0, count = 0;
    };
    static_assert(sizeof(NumericWindow)
                  == kNumericWindowRows * sizeof(Record) + 2 * sizeof(size_type));
    mutable NumericWindow numeric_window{};
    mutable detail::MappedIndexView index_view;
    // One bit per descriptor actually copied from the mapped source.
    // Unadmitted neighbours stay in the view until requested.
    using NumericCopies = std::bitset<kNumericWindowRows>;
    static_assert(sizeof(NumericCopies) == 48);
    mutable NumericCopies numeric_copied{};
    // Only misses update direction, leaving cache hits at one reference write.
    // Key and flags occupy 16 bytes on 64-bit targets, eight more than the key
    // alone. No additional record or per-entry state is needed.
    struct NumericDirection {
        size_type last_numeric_miss = 0;
        bool descending_step = false, window_backward = false;
    };
    static_assert(sizeof(NumericDirection) == 2 * sizeof(size_type));
    mutable NumericDirection numeric_direction{};

    struct BlockExtent {
        std::uint64_t offset = 0;
        size_type length = 0, row = 0, packed_offset = 0;
    };

    State() {
        index.bound_cache();
        payload.bound_cache();
    }
    ~State() { index.unmap(index_view); }
    static void validate_cache_rows(size_type rows) {
        if (rows == 0 || rows > kMaxCacheRows)
            throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                               {{"input", "resident.ledger_rows"},
                                                {"reason", rows == 0 ? "value_below_minimum"
                                                                     : "value_above_maximum"}},
                                               "trade ledger: cache rows outside [1,1048576]");
    }
    size_type cache_slot(size_type global) const {
        // Defaults/growth are powers of two. The public integer knob also
        // permits other final caps, which need the ordinary modulo fallback.
        const auto mask = cache_capacity - 1;
        return (cache_capacity & mask) == 0 ? global & mask : global % cache_capacity;
    }
    bool current(const CacheEntry& entry) const {
        return (entry.generation & ~size_type{3}) == cache_generation;
    }
    CacheEntry* cached(size_type global) const {
        if (!cache) return nullptr;
        auto& entry = cache[cache_slot(global)];
        if (!current(entry) || entry.global != global) return nullptr;
        entry.generation |= kReferenced;
        return &entry;
    }
    void invalidate(size_type global) {
        index.unmap(index_view);
        if (global >= numeric_window.first
            && global - numeric_window.first < numeric_window.count)
            numeric_window.count = 0;
        if (cached(global)) {
            cache[cache_slot(global)].generation = 0;
            --cache_count;
        }
    }
    void invalidate_cache() const {
        index.unmap(index_view);
        cache_count = 0;
        numeric_window.count = 0;
        numeric_direction = {};
        // Generations make publication/rewind/move invalidation O(1).
        // Even the wraparound fallback touches at most the configured budget.
        if (cache_generation == std::numeric_limits<size_type>::max() - 3) {
            if (cache)
                for (size_type i = 0; i < cache_capacity; ++i) cache[i].generation = 0;
            cache_generation = 4;
        } else {
            cache_generation += 4;
        }
    }
    void ensure_cache(size_type population) const {
        // A retiring ledger refuses reads below `first`, so only the retained
        // rows can ever be cached: size by them, not by every trade ever
        // closed. Slots are global & mask, so any capacity covering the
        // retained window holds it without collisions, wherever it starts.
        if (retiring && population > first) population -= first;
        if (cache && (population <= cache_capacity || cache_capacity == cache_rows)) return;
        size_type capacity = kResidentRows;
        while (capacity < population && capacity < cache_rows) capacity *= 2;
        capacity = std::min(capacity, cache_rows);
        invalidate_cache();
        // Discard the old allocation first: even transient storage stays
        // within the configured row budget during growth.
        cache.reset();
        cache_capacity = 0;
        cache = std::make_unique<CacheEntry[]>(capacity);
        cache_capacity = capacity;
    }
    void pack_strings(CacheEntry& entry) const {
        if (entry.strings_valid()) return;
        const auto& record = entry.record;
        const std::array<StringExtent, 4> texts{{
            record.entry_id, record.entry_comment, record.exit_comment, record.exit_id}};
        size_type bytes = 0;
        for (const auto text : texts) {
            if (text.length > kCacheStringBytes - bytes) return;
            bytes += static_cast<size_type>(text.length);
        }
        size_type offset = 0;
        for (const auto text : texts) {
            read_string(text, entry.strings.data() + offset);
            offset += static_cast<size_type>(text.length);
        }
        entry.generation |= size_type{1};
    }
    const CacheEntry& remember(size_type global, const Record& record) const {
        ensure_cache(end);
        auto& entry = cache[cache_slot(global)];
        if (current(entry)) --cache_count;
        entry.generation = 0;
        entry.record = record;
        entry.global = global;
        pack_strings(entry);
        entry.generation |= cache_generation | kReferenced;
        ++cache_count;
        return entry;
    }
    const CacheEntry& load(size_type global) const {
        if (auto* entry = cached(global)) {
            pack_strings(*entry);
            return *entry;
        }
        if (global < resident_first) return load_block(global);
        return remember(global, descriptor(global));
    }
    void admit_numeric(size_type global, const Record& record) const {
        auto& entry = cache[cache_slot(global)];
        // An admission counts as the first reference. A colliding miss gives
        // a referenced resident one second chance; stale residents can then
        // age out, while repeated scans retain their frequently hit rows.
        if (current(entry)) {
            if (entry.global == global) return;
            if (entry.generation & kReferenced) {
                entry.generation &= ~kReferenced;
                return;
            }
            --cache_count;
        }
        entry.generation = 0;
        entry.record = record;
        entry.global = global;
        entry.generation = cache_generation | kReferenced; // Strings stay lazy.
        ++cache_count;
    }
    NumericCopies numeric_copies(size_type first, size_type count,
                                 size_type requested, size_type retained) const {
        struct Key { size_type global, generation; };
        // Preview collisions in admission order without publishing changes
        // before every potentially failing mapped copy has succeeded.
        // At most 383 slots need shadowing: larger caches cannot collide
        // within a single numeric window. The fixed workspace is 6,144 bytes.
        std::array<Key, kNumericWindowRows> shadow;
        const bool collisions = cache_capacity < count;
        if (collisions)
            for (size_type i = 0; i < cache_capacity; ++i)
                shadow[i] = {cache[i].global, cache[i].generation};
        NumericCopies copies;
        copies[requested - first] = true;
        if (retained) copies[0] = true;
        for (size_type row = retained; row < count; ++row) {
            const auto global = first + row;
            const auto at = cache_slot(global);
            Key key = collisions ? shadow[at]
                                 : Key{cache[at].global, cache[at].generation};
            if ((key.generation & ~size_type{3}) == cache_generation) {
                if (key.global == global) continue;
                if (key.generation & kReferenced) {
                    key.generation &= ~kReferenced;
                    if (collisions) shadow[at] = key;
                    continue;
                }
            }
            copies[row] = true;
            if (collisions) shadow[at] = {global, cache_generation | kReferenced};
        }
        return copies;
    }
    void copy_numeric(size_type row, size_type first, size_type count,
                      bool descending) const {
        index.read_mapped(index_view, record_offset(first),
                          &numeric_window.records[row], sizeof(Record),
                          count * sizeof(Record), row * sizeof(Record), descending, false);
    }
    const Record& numeric_record(size_type global) const {
        if (const auto* entry = cached(global)) return entry->record;
        ensure_cache(end);
        // Starting a reverse scan requires two consecutive descending miss
        // steps. An ascending (i,i-1) pair supplies only one. Once a backward
        // window is established, its lower edge also continues the scan past
        // neighbours that were admitted and therefore became cache hits.
        const bool descending_step = numeric_direction.last_numeric_miss == global + 1;
        const bool descending = (descending_step && numeric_direction.descending_step)
            || (numeric_direction.window_backward && numeric_window.count != 0
                && numeric_window.first == global + 1);
        // In an ascending pair stream, the preceding low miss is two below
        // this high request. Keep the old window's last descriptor so the
        // paired low request cannot immediately discard the new window.
        // Ordinary ascending scans still read full, non-overlapping windows.
        const bool retain_predecessor = !descending
            && numeric_direction.descending_step
            && global > numeric_direction.last_numeric_miss
            && global - numeric_direction.last_numeric_miss == 2
            && !numeric_direction.window_backward && numeric_window.count != 0
            && global >= numeric_window.first
            && global - numeric_window.first == numeric_window.count;
        numeric_direction.last_numeric_miss = global;
        numeric_direction.descending_step = descending_step;
        if (global >= resident_first) {
            const auto& record = rows[slot(global)];
            admit_numeric(global, record);
            return record;
        }
        if (global >= numeric_window.first
            && global - numeric_window.first < numeric_window.count) {
            const auto row = global - numeric_window.first;
            if (!numeric_copied[row]) {
                try {
                    copy_numeric(row, numeric_window.first, numeric_window.count, descending);
                    numeric_copied[row] = true;
                } catch (...) {
                    numeric_window.count = 0;
                    throw;
                }
            }
            return numeric_window.records[row];
        }

        const size_type retained = retain_predecessor ? 1 : 0;
        const auto count = std::min(kNumericWindowRows - retained,
                                    descending ? global - first + 1 : resident_first - global);
        const auto window_first = descending ? global - (count - 1) : global - retained;
        if (retain_predecessor) {
            const auto row = numeric_window.count - 1;
            // A cache hit may have left the predecessor uncopied. Preserve
            // its actual descriptor before reusing the old window's storage.
            try {
                if (!numeric_copied[row])
                    copy_numeric(row, numeric_window.first, numeric_window.count, false);
                numeric_window.records[0] = numeric_window.records[row];
            } catch (...) {
                numeric_window.count = 0;
                throw;
            }
        }
        // Clear validity before I/O so a failed transfer cannot expose any
        // mixture of the preceding window and the new descriptors.
        numeric_window.count = 0;
        numeric_copied = numeric_copies(window_first, count + retained, global, retained);
        // Retain both sides of a crossing window to avoid revisiting the
        // preceding mapping for a lazily copied row during a monotone scan.
        if (index.mapped_read_crosses_window(record_offset(window_first),
                                             (count + retained) * sizeof(Record)))
            numeric_copied.set();
        try {
            for (size_type step = 0; step < count; ++step) {
                const auto row = descending ? count - 1 - step : retained + step;
                if (numeric_copied[row])
                    copy_numeric(row, window_first, count + retained, descending);
            }
            // Preserve the logical descriptor-read DONTNEED cadence.
            // The retained predecessor and lazy copies are not charged again.
            index.charge_mapped_read(count * sizeof(Record));
        } catch (...) {
            index.unmap(index_view);
            throw;
        }
        numeric_window.first = window_first;
        numeric_window.count = count + retained;
        numeric_direction.window_backward = descending;
        // The retained predecessor was already offered for admission.
        // Replay admission only after all selected copies have succeeded.
        for (size_type row = retained; row < numeric_window.count; ++row)
            admit_numeric(window_first + row, numeric_window.records[row]);
        return numeric_window.records[global - window_first];
    }
    const CacheEntry& load_block(size_type global) const {
        // These five fixed arrays total 35,392 bytes on the 64-bit build:
        // 64 * 168 descriptors + 64 * 128 payload + 64 * 128 packed strings
        // + (64 * 4) * 32 extent metadata + 64 * 1 validity
        // = 10,752 + 8,192 + 8,192 + 8,192 + 64. This stack workspace
        // does not grow with the ledger population.
        std::array<Record, kResidentRows> records{};
        std::array<char, kBlockPayloadBytes> payload_bytes{};
        std::array<char, kResidentRows * kCacheStringBytes> strings{};
        std::array<BlockExtent, kResidentRows * 4> extents{};
        std::array<bool, kResidentRows> strings_valid{};

        const auto count = std::min(kResidentRows,
                                    std::min(end - global, resident_first - global));
        index.read(record_offset(global), records.data(), count * sizeof(Record), true);

        size_type extent_count = 0;
        for (size_type row = 0; row < count; ++row) {
            const std::array<StringExtent, 4> texts{{
                records[row].entry_id, records[row].entry_comment,
                records[row].exit_comment, records[row].exit_id}};
            size_type bytes = 0;
            bool valid = true;
            for (const auto text : texts) {
                if (text.length > kCacheStringBytes - bytes) {
                    valid = false;
                    break;
                }
                bytes += static_cast<size_type>(text.length);
            }
            strings_valid[row] = valid;
            if (!valid) continue;

            size_type packed_offset = 0;
            for (const auto text : texts) {
                // A corrupt extent in any packable neighbour can make a
                // healthy requested row throw; scratch corruption fails the
                // run anyway, even when the requested row is intact.
                validate_string(text);
                const auto length = static_cast<size_type>(text.length);
                if (length != 0) {
                    auto* destination = strings.data()
                        + row * kCacheStringBytes + packed_offset;
                    if (payload_end - text.offset <= kPayloadTailBytes) {
                        const auto span = tail_span(text.offset, length);
                        std::copy_n(payload_tail.data() + span.slot, span.first, destination);
                        std::copy_n(payload_tail.data(), length - span.first,
                                    destination + span.first);
                    } else {
                        extents[extent_count++] = {text.offset, length, row, packed_offset};
                    }
                }
                packed_offset += length;
            }
        }

        std::sort(extents.begin(), extents.begin() + extent_count,
                  [](const BlockExtent& left, const BlockExtent& right) {
                      return left.offset < right.offset;
                  });
        size_type begin = 0;
        while (begin < extent_count) {
            size_type finish = begin + 1;
            std::uint64_t range_end = extents[begin].offset + extents[begin].length;
            while (finish < extent_count
                   && extents[finish].offset <= range_end) {
                range_end = std::max(range_end,
                                     extents[finish].offset + extents[finish].length);
                ++finish;
            }
            const auto range_start = extents[begin].offset;
            const auto range_length = range_end - range_start;
            // Only touching/overlapping extents merge, so a range is their
            // union: at most 64 packable rows * 128 bytes = 8,192 bytes.
            if (range_length > payload_bytes.size())
                throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                               "trade ledger: block payload exceeds workspace");
            payload.read(range_start, payload_bytes.data(),
                         static_cast<size_type>(range_length));
            for (size_type i = begin; i < finish; ++i) {
                const auto& extent = extents[i];
                const auto relative = static_cast<size_type>(extent.offset - range_start);
                std::copy_n(payload_bytes.data() + relative, extent.length,
                            strings.data() + extent.row * kCacheStringBytes
                                + extent.packed_offset);
            }
            begin = finish;
        }

        block_result.record = records[0];
        block_result.global = global;
        if (strings_valid[0])
            std::copy_n(strings.data(), kCacheStringBytes, block_result.strings.data());
        block_result.generation = cache_generation | kReferenced
            | static_cast<size_type>(strings_valid[0]);

        ensure_cache(end);
        for (size_type row = 0; row < count; ++row) {
            auto& entry = cache[cache_slot(global + row)];
            if (current(entry)) --cache_count;
            entry.generation = 0;
            entry.record = records[row];
            if (strings_valid[row]) {
                std::copy_n(strings.data() + row * kCacheStringBytes,
                            kCacheStringBytes, entry.strings.data());
            }
            entry.global = global + row;
            entry.generation = cache_generation | kReferenced
                | static_cast<size_type>(strings_valid[row]);
            ++cache_count;
        }
        if (const auto* entry = cached(global)) return *entry;
        return block_result;
    }
    template<auto Member>
    auto numeric_field(size_type global) const {
        check(global, true);
        return numeric_record(global).*Member;
    }
    template<StringExtent Record::*Member>
    std::string string_field(size_type global) const {
        check(global, true);
        const auto& entry = load(global);
        const auto text = entry.record.*Member;
        if (!entry.strings_valid()) return get_string(text);
        size_type offset = 0;
        if constexpr (Member != &Record::entry_id)
            offset += static_cast<size_type>(entry.record.entry_id.length);
        if constexpr (Member == &Record::exit_comment || Member == &Record::exit_id)
            offset += static_cast<size_type>(entry.record.entry_comment.length);
        if constexpr (Member == &Record::exit_id)
            offset += static_cast<size_type>(entry.record.exit_comment.length);
        return std::string(entry.strings.data() + offset, static_cast<size_type>(text.length));
    }
    template<StringExtent Record::*Member>
    bool string_equals(size_type global, std::string_view rhs) const {
        check(global, true);
        const auto& entry = load(global);
        const auto text = entry.record.*Member;
        if (!entry.strings_valid() || text.length > kCacheStringBytes)
            return string_equals_chunks(text, rhs);
        size_type offset = 0;
        if constexpr (Member != &Record::entry_id)
            offset += static_cast<size_type>(entry.record.entry_id.length);
        if constexpr (Member == &Record::exit_comment || Member == &Record::exit_id)
            offset += static_cast<size_type>(entry.record.entry_comment.length);
        if constexpr (Member == &Record::exit_id)
            offset += static_cast<size_type>(entry.record.exit_comment.length);
        return std::string_view(entry.strings.data() + offset,
                                static_cast<size_type>(text.length)) == rhs;
    }
    static std::uint64_t absolute_record_offset(size_type global) {
        if (global > std::numeric_limits<std::uint64_t>::max() / sizeof(Record))
            throw detail::ScratchIoError(EFBIG, "trade ledger offset");
        return static_cast<std::uint64_t>(global) * sizeof(Record);
    }
    std::uint64_t record_offset(size_type global) const {
        return absolute_record_offset(global) - index_origin;
    }
    void check(size_type global, bool mutable_row = false) const {
        if (retiring && global < first) throw detail::RetiredTradeHistoryRead();
        if (global >= end || (mutable_row && global < first))
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                           "trade ledger: row outside resident window");
    }
    size_type slot(size_type global) const {
        return (head + global - resident_first) % kResidentRows;
    }
    struct TailSpan { size_type slot, first; };
    static TailSpan tail_span(std::uint64_t offset, size_type length) noexcept {
        const auto slot = static_cast<size_type>(offset % kPayloadTailBytes);
        return {slot, std::min(length, kPayloadTailBytes - slot)};
    }
    void remember_payload(std::uint64_t offset, const std::string& text) noexcept {
        const auto retained = std::min(text.size(), kPayloadTailBytes);
        const auto skip = text.size() - retained;
        const auto span = tail_span(offset + skip, retained);
        std::copy_n(text.data() + skip, span.first, payload_tail.data() + span.slot);
        std::copy_n(text.data() + skip + span.first, retained - span.first, payload_tail.data());
    }
    StringExtent put_string(const std::string& text) {
        detail::ScratchFile::check_extent(payload_end, text.size());
        const StringExtent result{payload_end, static_cast<std::uint64_t>(text.size())};
        if (!text.empty()) {
            payload.write(payload_end - payload_origin, text.data(), text.size());
            remember_payload(payload_end, text);
        }
        payload_end += text.size();
        return result;
    }
    void validate_string(StringExtent text) const {
        if (text.length > std::numeric_limits<size_type>::max()
            || text.offset > payload_end || text.length > payload_end - text.offset
            || (text.length != 0 && text.offset < payload_retired))
            throw detail::ScratchIoError(EIO, "trade ledger string extent");
    }
    void read_string(StringExtent text, char* result) const {
        validate_string(text);
        const auto length = static_cast<size_type>(text.length);
        if (length != 0) {
            // Validation above proves the entire extent ends at/before
            // payload_end. Its start must also be in the retained byte tail;
            // in particular, a string wider than the tail always spills.
            if (payload_end - text.offset <= kPayloadTailBytes) {
                const auto span = tail_span(text.offset, length);
                std::copy_n(payload_tail.data() + span.slot, span.first, result);
                std::copy_n(payload_tail.data(), length - span.first, result + span.first);
            } else {
                payload.read(text.offset - payload_origin, result, length);
            }
        }
    }
    [[gnu::noinline, gnu::cold]]
    bool string_equals_chunks(StringExtent text, std::string_view rhs) const {
        validate_string(text);
        const auto length = static_cast<size_type>(text.length);
        // Match the scratch transfer budget; keep this fixed buffer off the
        // cache-hit stack and independent of the requested field's length.
        std::array<char, detail::ScratchFile::kCacheBytes> chunk;
        bool equal = length == rhs.size();
        for (size_type offset = 0; offset < length;) {
            const auto count = std::min(chunk.size(), length - offset);
            if (payload_end - text.offset <= kPayloadTailBytes)
                read_string({text.offset + offset, count}, chunk.data());
            else
                payload.read(text.offset + offset - payload_origin, chunk.data(), count);
            // Never skip a read after a length/content mismatch: the owning
            // read would still surface an I/O failure later in the extent.
            if (equal && std::string_view(chunk.data(), count)
                             != std::string_view(rhs.data() + offset, count))
                equal = false;
            offset += count;
        }
        return equal;
    }
    std::string get_string(StringExtent text) const {
        validate_string(text);
        std::string result(static_cast<size_type>(text.length), '\0');
        read_string(text, result.data());
        return result;
    }
    StringExtent reuse_string(const std::string& text, StringExtent old) {
        if (text.size() == old.length && text == get_string(old)) return old;
        return put_string(text);
    }
    Record store(size_type global, const Trade& row, const Record* previous = nullptr) {
        index.open("pineforge-trade-index");
        payload.open("pineforge-trade-payload");
        Record record{};
        record.entry_time = row.entry_time; record.exit_time = row.exit_time;
        record.entry_price = row.entry_price; record.exit_price = row.exit_price;
        record.qty = row.qty; record.pnl = row.pnl; record.pnl_pct = row.pnl_pct;
        record.entry_bar_index = row.entry_bar_index; record.exit_bar_index = row.exit_bar_index;
        // Replacements retain equal extents, including embedded NULs. Payload
        // growth follows actual string changes, not numeric-update/bar count.
        record.entry_id = previous ? reuse_string(row.entry_id, previous->entry_id)
                                   : put_string(row.entry_id);
        record.entry_comment = previous ? reuse_string(row.entry_comment, previous->entry_comment)
                                        : put_string(row.entry_comment);
        record.exit_comment = previous ? reuse_string(row.exit_comment, previous->exit_comment)
                                       : put_string(row.exit_comment);
        record.exit_id = previous ? reuse_string(row.exit_id, previous->exit_id)
                                  : put_string(row.exit_id);
        record.max_runup = row.max_runup; record.max_drawdown = row.max_drawdown;
        record.commission = row.commission; record.entry_incarnation = row.entry_incarnation;
        record.close_cause = row.close_cause; record.is_long = row.is_long;
        record.exit_from_bracket = row.exit_from_bracket; record.open_at_end = row.open_at_end;
        // Growth changes the view's memoized EOF even outside the window.
        index.unmap(index_view);
        index.write(record_offset(global), &record, sizeof(record), true);
        return record;
    }
    static void erase_bytes(detail::ScratchFile& file, std::uint64_t first,
                            std::uint64_t stop) {
        const std::array<unsigned char, 4096> zeros{};
        while (first < stop) {
            const auto count = static_cast<size_type>(
                std::min<std::uint64_t>(zeros.size(), stop - first));
            file.write(first, zeros.data(), count);
            first += count;
        }
    }
    void erase_payload(StringExtent text) {
        if (text.length == 0) return;
        validate_string(text);
        erase_bytes(payload, text.offset - payload_origin,
                    text.offset - payload_origin + text.length);
        // Also remove the erased bytes from the fixed resident payload tail.
        const auto first = std::max(text.offset, payload_end > kPayloadTailBytes
            ? payload_end - kPayloadTailBytes : 0);
        const auto stop = text.offset + text.length;
        if (first < stop) {
            const auto count = static_cast<size_type>(stop - first);
            const auto span = tail_span(first, count);
            std::fill_n(payload_tail.data() + span.slot, span.first, '\0');
            std::fill_n(payload_tail.data(), count - span.first, '\0');
        }
    }
    static void release_head(detail::ScratchFile& file, std::uint64_t& origin,
                             std::uint64_t old_end, std::uint64_t new_end,
                             bool empty) {
        if (new_end == origin && !empty) return;
        if (empty) {
            // No suffix, including an all-empty-string payload: release the
            // final partial chunk too. Keep the configured backing open.
            if (file.opened()) file.truncate(0);
            origin = new_end;
            return;
        }
        const auto bytes = (new_end - origin) / detail::ScratchArena::kChunkBytes
            * detail::ScratchArena::kChunkBytes;
        file.release_prefix(bytes);
        origin += bytes;
        // At most one zero-filled head chunk remains. It contains no closed
        // row bytes; complete chunks have returned to the allocator/file.
        erase_bytes(file, std::max(old_end, origin) - origin, new_end - origin);
    }
    void retire(size_type global_end) {
        if (!retiring || global_end == first) return;
        // Mutable rows may have been replaced out of order. The first live
        // row's text is not necessarily the first live text extent: examine
        // every surviving descriptor before selecting a payload watermark.
        std::uint64_t text_first = payload_end;
        for (size_type i = global_end; i < end; ++i) {
            const auto row = descriptor(i);
            for (const auto text : {row.entry_id, row.entry_comment,
                                    row.exit_comment, row.exit_id})
                if (text.length != 0) text_first = std::min(text_first, text.offset);
        }
        // No strings are shared between rows. Erase retired text even when a
        // replacement put it AFTER a surviving row's earliest payload.
        for (size_type i = first; i < global_end; ++i) {
            const auto row = descriptor(i);
            for (const auto text : {row.entry_id, row.entry_comment,
                                    row.exit_comment, row.exit_id})
                erase_payload(text);
        }
        release_head(index, index_origin, absolute_record_offset(first),
                     absolute_record_offset(global_end), global_end == end);
        release_head(payload, payload_origin, payload_retired, text_first,
                     text_first == payload_end);
        payload_retired = text_first;
        invalidate_cache();
        materialized.erase(materialized.begin(), materialized.lower_bound(global_end));
    }
    static Trade decode_numeric(const Record& record) {
        Trade row{};
        row.entry_time = record.entry_time; row.exit_time = record.exit_time;
        row.entry_price = record.entry_price; row.exit_price = record.exit_price;
        row.qty = record.qty; row.pnl = record.pnl; row.pnl_pct = record.pnl_pct;
        row.entry_bar_index = record.entry_bar_index; row.exit_bar_index = record.exit_bar_index;
        row.max_runup = record.max_runup; row.max_drawdown = record.max_drawdown;
        row.commission = record.commission; row.entry_incarnation = record.entry_incarnation;
        row.close_cause = record.close_cause; row.is_long = record.is_long;
        row.exit_from_bracket = record.exit_from_bracket; row.open_at_end = record.open_at_end;
        return row;
    }
    Trade decode(const Record& record) const {
        Trade row = decode_numeric(record);
        row.entry_id = get_string(record.entry_id);
        row.entry_comment = get_string(record.entry_comment);
        row.exit_comment = get_string(record.exit_comment);
        row.exit_id = get_string(record.exit_id);
        return row;
    }
    static Trade decode_cached(const CacheEntry& entry) {
        Trade row = decode_numeric(entry.record);
        const std::array<std::string*, 4> texts{{
            &row.entry_id, &row.entry_comment, &row.exit_comment, &row.exit_id}};
        const std::array<std::uint64_t, 4> lengths{{
            entry.record.entry_id.length, entry.record.entry_comment.length,
            entry.record.exit_comment.length, entry.record.exit_id.length}};
        size_type offset = 0;
        for (size_type i = 0; i < texts.size(); ++i) {
            const auto length = static_cast<size_type>(lengths[i]);
            texts[i]->assign(entry.strings.data() + offset, length);
            offset += length;
        }
        return row;
    }
    Record descriptor(size_type global, bool mapped = false) const {
        if (const auto* entry = cached(global)) return entry->record;
        if (global >= resident_first) return rows[slot(global)];
        Record record{};
        if (mapped)
            index.read_mapped(index_view, record_offset(global), &record, sizeof(record));
        else
            index.read(record_offset(global), &record, sizeof(record), true);
        return record;
    }
    void evict() {
        rows[head] = Record{};
        head = (head + 1) % kResidentRows;
        ++resident_first;
        --count;
    }
};

TradeLedger::TradeLedger() : state_(std::make_unique<State>()) {
    assert(logical_end_ == state_->end);
}
TradeLedger::~TradeLedger() = default;

TradeLedger::TradeLedger(const TradeLedger& other)
    : TradeLedger() {
    assert(other.logical_end_ == other.state_->end);
    set_cache_rows(other.cache_rows());
    set_scratch_directory(other.state_->directory);
    // Do not populate/copy the read cache just to clone scratch. Existing
    // source hits may serve the copy; misses keep the original O(T) I/O path.
    if (other.state_->retiring) {
        // A copy owns the same logical window, without trying to resurrect an
        // intentionally retired prefix. Rebuild only the retained payload.
        state_->retiring = true;
        state_->first = state_->end = state_->resident_first = other.base();
        logical_end_ = state_->end;
        assert(logical_end_ == state_->end);
        state_->index_origin = State::absolute_record_offset(other.base());
    }
    for (size_type i = state_->end; i < other.size(); ++i) {
        const auto* entry = other.state_->cached(i);
        push_back(entry && entry->strings_valid()
                      ? State::decode_cached(*entry)
                      : other.state_->decode(other.state_->descriptor(i)));
    }
    seal(other.base());
    assert(logical_end_ == state_->end);
}

TradeLedger& TradeLedger::operator=(const TradeLedger& other) {
    assert(logical_end_ == state_->end);
    assert(other.logical_end_ == other.state_->end);
    if (this != &other) {
        TradeLedger next(other);
        state_.swap(next.state_);
        std::swap(logical_end_, next.logical_end_);
        assert(next.logical_end_ == next.state_->end);
    }
    assert(logical_end_ == state_->end);
    return *this;
}

TradeLedger::TradeLedger(TradeLedger&& other) : TradeLedger() {
    assert(other.logical_end_ == other.state_->end);
    state_.swap(other.state_);
    std::swap(logical_end_, other.logical_end_);
    state_->invalidate_cache();
    assert(logical_end_ == state_->end);
    assert(other.logical_end_ == other.state_->end);
}

TradeLedger& TradeLedger::operator=(TradeLedger&& other) {
    assert(logical_end_ == state_->end);
    assert(other.logical_end_ == other.state_->end);
    if (this != &other) {
        TradeLedger next(std::move(other));
        state_.swap(next.state_);
        std::swap(logical_end_, next.logical_end_);
        assert(next.logical_end_ == next.state_->end);
    }
    assert(logical_end_ == state_->end);
    assert(other.logical_end_ == other.state_->end);
    return *this;
}

void TradeLedger::clear() {
    assert(logical_end_ == state_->end);
    auto next = std::make_unique<State>();
    next->cache_rows = state_->cache_rows;
    next->directory = state_->directory;
    next->index.set_directory(next->directory);
    next->payload.set_directory(next->directory);
    state_.swap(next);
    logical_end_ = 0;
    assert(logical_end_ == state_->end);
}

void TradeLedger::set_scratch_directory(const std::string& directory) {
    state_->index.set_directory(directory);
    state_->payload.set_directory(directory);
    state_->directory = directory;
}
const std::string& TradeLedger::scratch_directory() const { return state_->directory; }

void TradeLedger::set_history_retirement(bool enabled) {
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: retirement policy changed after append");
    state_->retiring = enabled;
}

void TradeLedger::check_history_read(size_type global) const { state_->check(global); }

std::array<std::uint64_t, 2> TradeLedger::scratch_storage_bytes() const {
    return {state_->index.storage_bytes(), state_->payload.storage_bytes()};
}

void TradeLedger::set_cache_rows(size_type rows) {
    State::validate_cache_rows(rows);
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened())
        throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                           {{"input", "resident.ledger_rows"}, {"reason", "frozen"}},
                                           "trade ledger: cache rows changed after spill");
    state_->cache.reset();
    state_->cache_count = 0;
    state_->cache_capacity = 0;
    state_->cache_rows = rows;
}
TradeLedger::size_type TradeLedger::cache_rows() const { return state_->cache_rows; }
TradeLedger::size_type TradeLedger::resident_cache_bytes(size_type rows) {
    State::validate_cache_rows(rows);
    return sizeof(TradeLedger) + sizeof(State) + rows * kLedgerEntryBytes;
}
TradeLedger::size_type TradeLedger::cache_resident_bytes() const {
    return sizeof(TradeLedger) + sizeof(State)
        + state_->cache_capacity * kLedgerEntryBytes;
}
TradeLedger::size_type TradeLedger::cache_size() const { return state_->cache_count; }

void TradeLedger::reserve(size_type) {}

TradeLedger::size_type TradeLedger::base() const { return state_->first; }
TradeLedger::size_type TradeLedger::resident_size() const {
    return state_->count + state_->materialized.size();
}
TradeLedger::size_type TradeLedger::resident_base() const { return state_->resident_first; }
bool TradeLedger::empty() const { return size() == 0; }

Trade TradeLedger::read(size_type global) const {
    state_->check(global);
    const auto& entry = state_->load(global);
    return entry.strings_valid() ? State::decode_cached(entry) : state_->decode(entry.record);
}

Trade TradeLedger::operator[](size_type global) const { return at(global); }
Trade TradeLedger::at(size_type global) const {
    state_->check(global, true);
    return read(global);
}

Trade TradeLedger::back() const {
    if (size() == base())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: no resident last row");
    return read(size() - 1);
}

Trade TradeLedger::numeric_at(size_type global) const {
    state_->check(global, true);
    // Preserve this whole-numeric-row accessor's existing admission behavior.
    return State::decode_numeric(state_->descriptor(global, true));
}

std::string TradeLedger::entry_id_at(size_type global) const {
    return state_->string_field<&State::Record::entry_id>(global);
}
std::string TradeLedger::entry_comment_at(size_type global) const {
    return state_->string_field<&State::Record::entry_comment>(global);
}
std::string TradeLedger::exit_comment_at(size_type global) const {
    return state_->string_field<&State::Record::exit_comment>(global);
}
std::string TradeLedger::exit_id_at(size_type global) const {
    return state_->string_field<&State::Record::exit_id>(global);
}
bool TradeLedger::entry_id_equals(size_type global, std::string_view rhs) const {
    return state_->string_equals<&State::Record::entry_id>(global, rhs);
}
bool TradeLedger::exit_id_equals(size_type global, std::string_view rhs) const {
    return state_->string_equals<&State::Record::exit_id>(global, rhs);
}
bool TradeLedger::entry_comment_equals(size_type global, std::string_view rhs) const {
    return state_->string_equals<&State::Record::entry_comment>(global, rhs);
}
bool TradeLedger::exit_comment_equals(size_type global, std::string_view rhs) const {
    return state_->string_equals<&State::Record::exit_comment>(global, rhs);
}
double TradeLedger::pnl_at(size_type global) const {
    return state_->numeric_field<&State::Record::pnl>(global);
}
double TradeLedger::pnl_pct_at(size_type global) const {
    return state_->numeric_field<&State::Record::pnl_pct>(global);
}
double TradeLedger::commission_at(size_type global) const {
    return state_->numeric_field<&State::Record::commission>(global);
}
std::int32_t TradeLedger::entry_bar_index_at(size_type global) const {
    return state_->numeric_field<&State::Record::entry_bar_index>(global);
}
std::int32_t TradeLedger::exit_bar_index_at(size_type global) const {
    return state_->numeric_field<&State::Record::exit_bar_index>(global);
}
std::uint64_t TradeLedger::entry_incarnation_at(size_type global) const {
    return state_->numeric_field<&State::Record::entry_incarnation>(global);
}
double TradeLedger::entry_price_at(size_type global) const {
    return state_->numeric_field<&State::Record::entry_price>(global);
}
double TradeLedger::exit_price_at(size_type global) const {
    return state_->numeric_field<&State::Record::exit_price>(global);
}
std::int64_t TradeLedger::entry_time_at(size_type global) const {
    return state_->numeric_field<&State::Record::entry_time>(global);
}
std::int64_t TradeLedger::exit_time_at(size_type global) const {
    return state_->numeric_field<&State::Record::exit_time>(global);
}
double TradeLedger::qty_at(size_type global) const {
    return state_->numeric_field<&State::Record::qty>(global);
}
double TradeLedger::max_runup_at(size_type global) const {
    return state_->numeric_field<&State::Record::max_runup>(global);
}
double TradeLedger::max_drawdown_at(size_type global) const {
    return state_->numeric_field<&State::Record::max_drawdown>(global);
}
bool TradeLedger::is_long_at(size_type global) const {
    return state_->numeric_field<&State::Record::is_long>(global);
}

void TradeLedger::replace(size_type global, const Trade& row) {
    state_->check(global, true);
    const auto previous = state_->descriptor(global);
    state_->invalidate(global);
    const auto record = state_->store(global, row, &previous);
    if (state_->retiring) {
        const State::StringExtent old_texts[]{previous.entry_id, previous.entry_comment,
                                             previous.exit_comment, previous.exit_id};
        const State::StringExtent new_texts[]{record.entry_id, record.entry_comment,
                                             record.exit_comment, record.exit_id};
        for (size_type i = 0; i < 4; ++i)
            if (old_texts[i].offset != new_texts[i].offset
                || old_texts[i].length != new_texts[i].length)
                state_->erase_payload(old_texts[i]);
    }
    if (global >= state_->resident_first) state_->rows[state_->slot(global)] = record;
    const auto legacy = state_->materialized.find(global);
    if (legacy != state_->materialized.end()) legacy->second = row;
}

void TradeLedger::modify(size_type global, const std::function<void(Trade&)>& edit) {
    Trade row = at(global);
    edit(row);
    replace(global, row);
}

void TradeLedger::push_back(const Trade& trade) {
    assert(logical_end_ == state_->end);
    if (size() == std::numeric_limits<size_type>::max())
        throw std::length_error("trade ledger: logical end exhausted");
    // Copies and write-only runs keep a cold cache unallocated. Once a reader
    // allocates it, population growth expands it only up to the configured cap.
    if (state_->cache) state_->ensure_cache(size() + 1);
    const auto record = state_->store(size(), trade);
    if (state_->count == kResidentRows) state_->evict();
    state_->rows[(state_->head + state_->count) % kResidentRows] = record;
    ++state_->count;
    ++state_->end;
    logical_end_ = state_->end;
    assert(logical_end_ == state_->end);
}

void TradeLedger::push_back(Trade&& trade) { push_back(static_cast<const Trade&>(trade)); }

void TradeLedger::resize(size_type global_end) {
    assert(logical_end_ == state_->end);
    if (global_end < state_->first)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: rewind below resident window");
    state_->invalidate_cache();
    if (global_end < size()) {
        // Re-cuts may rewind far below the descriptor window. All row bytes
        // are already external; discard the bounded descriptor cache.
        while (state_->count != 0) state_->evict();
        state_->end = state_->resident_first = global_end;
        logical_end_ = state_->end;
        state_->materialized.erase(state_->materialized.lower_bound(global_end),
                                    state_->materialized.end());
    } else {
        while (size() < global_end) push_back(Trade{});
    }
    assert(logical_end_ == state_->end);
}

Trade TradeLedger::const_iterator::operator*() const { return ledger_->read(index_); }
TradeLedger::const_iterator TradeLedger::begin() const { return {this, base()}; }
TradeLedger::const_iterator TradeLedger::end() const { return {this, size()}; }
void TradeLedger::visit(size_type first, size_type end, const Visitor& visitor) const {
    if (first > end || end > size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: invalid visit range");
    for (size_type i = first; i < end; ++i) {
        const Trade row = read(i);
        if (!visitor(i, row)) break;
    }
}

bool TradeLedger::previous(size_type& cursor, Trade& value) const {
    if (cursor > size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: invalid backward cursor");
    if (cursor == 0) return false;
    value = read(cursor - 1);
    --cursor;
    return true;
}

const Trade& TradeLedger::materialize(size_type global) const {
    state_->check(global, true);
    const auto found = state_->materialized.find(global);
    if (found != state_->materialized.end()) return found->second;
    return state_->materialized.emplace(global, read(global)).first->second;
}

void TradeLedger::seal(size_type global_end) {
    assert(logical_end_ == state_->end);
    if (global_end < state_->first || global_end > size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: seal outside resident window");
    // Drop the read-only index window before retirement can erase or release
    // the bytes it maps.
    state_->index.unmap(state_->index_view);
    state_->numeric_window.count = 0;
    // The consumer has already emitted T and folded all logical identities.
    // Only an explicitly admitted sink run drops its physical history.
    state_->retire(global_end);
    // Publication changes no row content, and the checked accessors refuse
    // sealed rows before they reach the cache: cached entries stay valid, so a
    // closedtrades loop is not made cold again by every published trade.
    while (state_->count != 0 && state_->resident_first < global_end) state_->evict();
    state_->first = global_end;
    assert(logical_end_ == state_->end);
}

} // inline namespace engine_script_run_v19

}  // namespace pineforge
