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

#include <algorithm>
#include <array>
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
// settlement or close entry point calls it (the census is in the EN-132 notes).
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
    // Strings are never retained by the window. It holds fixed descriptors;
    // even the newest row's four payloads already live in scratch.
    std::array<Record, kResidentRows> rows{};
    size_type first = 0, end = 0, resident_first = 0, count = 0, head = 0;
    detail::ScratchFile index, payload;
    std::uint64_t payload_end = 0;
    std::string directory;
    mutable std::map<size_type, Trade> materialized;

    State() {
        index.bound_cache();
        payload.bound_cache();
    }
    static std::uint64_t record_offset(size_type global) {
        if (global > std::numeric_limits<std::uint64_t>::max() / sizeof(Record))
            throw detail::ScratchIoError(EFBIG, "trade ledger offset");
        return static_cast<std::uint64_t>(global) * sizeof(Record);
    }
    void check(size_type global, bool mutable_row = false) const {
        if (global >= end || (mutable_row && global < first))
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                           "trade ledger: row outside resident window");
    }
    size_type slot(size_type global) const {
        return (head + global - resident_first) % kResidentRows;
    }
    StringExtent put_string(const std::string& text) {
        detail::ScratchFile::check_extent(payload_end, text.size());
        const StringExtent result{payload_end, static_cast<std::uint64_t>(text.size())};
        if (!text.empty()) payload.write(payload_end, text.data(), text.size());
        payload_end += text.size();
        return result;
    }
    std::string get_string(StringExtent text) const {
        if (text.length > std::numeric_limits<size_type>::max()
            || text.offset > payload_end || text.length > payload_end - text.offset)
            throw detail::ScratchIoError(EIO, "trade ledger string extent");
        std::string result(static_cast<size_type>(text.length), '\0');
        if (!result.empty()) payload.read(text.offset, result.data(), result.size());
        return result;
    }
    Record store(size_type global, const Trade& row) {
        index.open("pineforge-trade-index");
        payload.open("pineforge-trade-payload");
        Record record{};
        record.entry_time = row.entry_time; record.exit_time = row.exit_time;
        record.entry_price = row.entry_price; record.exit_price = row.exit_price;
        record.qty = row.qty; record.pnl = row.pnl; record.pnl_pct = row.pnl_pct;
        record.entry_bar_index = row.entry_bar_index; record.exit_bar_index = row.exit_bar_index;
        record.entry_id = put_string(row.entry_id);
        record.entry_comment = put_string(row.entry_comment);
        record.exit_comment = put_string(row.exit_comment);
        record.exit_id = put_string(row.exit_id);
        record.max_runup = row.max_runup; record.max_drawdown = row.max_drawdown;
        record.commission = row.commission; record.entry_incarnation = row.entry_incarnation;
        record.close_cause = row.close_cause; record.is_long = row.is_long;
        record.exit_from_bracket = row.exit_from_bracket; record.open_at_end = row.open_at_end;
        index.write(record_offset(global), &record, sizeof(record));
        return record;
    }
    Trade decode(const Record& record) const {
        Trade row{};
        row.entry_time = record.entry_time; row.exit_time = record.exit_time;
        row.entry_price = record.entry_price; row.exit_price = record.exit_price;
        row.qty = record.qty; row.pnl = record.pnl; row.pnl_pct = record.pnl_pct;
        row.entry_bar_index = record.entry_bar_index; row.exit_bar_index = record.exit_bar_index;
        row.entry_id = get_string(record.entry_id);
        row.entry_comment = get_string(record.entry_comment);
        row.exit_comment = get_string(record.exit_comment);
        row.exit_id = get_string(record.exit_id);
        row.max_runup = record.max_runup; row.max_drawdown = record.max_drawdown;
        row.commission = record.commission; row.entry_incarnation = record.entry_incarnation;
        row.close_cause = record.close_cause; row.is_long = record.is_long;
        row.exit_from_bracket = record.exit_from_bracket; row.open_at_end = record.open_at_end;
        return row;
    }
    Trade load(size_type global) const {
        Record record{};
        index.read(record_offset(global), &record, sizeof(record));
        return decode(record);
    }
    void evict() {
        rows[head] = Record{};
        head = (head + 1) % kResidentRows;
        ++resident_first;
        --count;
    }
};

TradeLedger::TradeLedger() : state_(std::make_unique<State>()) {}
TradeLedger::~TradeLedger() = default;

TradeLedger::TradeLedger(const TradeLedger& other)
    : TradeLedger() {
    set_scratch_directory(other.state_->directory);
    for (size_type i = 0; i < other.size(); ++i) push_back(other.read(i));
    seal(other.base());
}

TradeLedger& TradeLedger::operator=(const TradeLedger& other) {
    if (this != &other) {
        TradeLedger next(other);
        state_.swap(next.state_);
    }
    return *this;
}

TradeLedger::TradeLedger(TradeLedger&& other) : TradeLedger() {
    state_.swap(other.state_);
}

TradeLedger& TradeLedger::operator=(TradeLedger&& other) {
    if (this != &other) {
        TradeLedger next(std::move(other));
        state_.swap(next.state_);
    }
    return *this;
}

void TradeLedger::clear() {
    auto next = std::make_unique<State>();
    next->directory = state_->directory;
    next->index.set_directory(next->directory);
    next->payload.set_directory(next->directory);
    state_.swap(next);
}

void TradeLedger::set_scratch_directory(const std::string& directory) {
    state_->index.set_directory(directory);
    state_->payload.set_directory(directory);
    state_->directory = directory;
}
const std::string& TradeLedger::scratch_directory() const { return state_->directory; }

void TradeLedger::reserve(size_type) {}

TradeLedger::size_type TradeLedger::size() const { return state_->end; }
TradeLedger::size_type TradeLedger::base() const { return state_->first; }
TradeLedger::size_type TradeLedger::resident_size() const {
    return state_->count + state_->materialized.size();
}
TradeLedger::size_type TradeLedger::resident_base() const { return state_->resident_first; }
bool TradeLedger::empty() const { return size() == 0; }

Trade TradeLedger::read(size_type global) const {
    state_->check(global);
    return global >= state_->resident_first ? state_->decode(state_->rows[state_->slot(global)])
                                           : state_->load(global);
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

void TradeLedger::replace(size_type global, const Trade& row) {
    state_->check(global, true);
    const auto record = state_->store(global, row);
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
    if (size() == std::numeric_limits<size_type>::max())
        throw std::length_error("trade ledger: logical end exhausted");
    const auto record = state_->store(size(), trade);
    if (state_->count == kResidentRows) state_->evict();
    state_->rows[(state_->head + state_->count) % kResidentRows] = record;
    ++state_->count;
    ++state_->end;
}

void TradeLedger::push_back(Trade&& trade) { push_back(static_cast<const Trade&>(trade)); }

void TradeLedger::resize(size_type global_end) {
    if (global_end < state_->first)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: rewind below resident window");
    if (global_end < size()) {
        // Re-cuts may rewind far below the descriptor window. All row bytes
        // are already external; discard the bounded descriptor cache.
        while (state_->count != 0) state_->evict();
        state_->end = state_->resident_first = global_end;
        state_->materialized.erase(state_->materialized.lower_bound(global_end),
                                    state_->materialized.end());
    } else {
        while (size() < global_end) push_back(Trade{});
    }
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
    if (global_end < state_->first || global_end > size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: seal outside resident window");
    while (state_->count != 0 && state_->resident_first < global_end) state_->evict();
    state_->first = global_end;
}

} // inline namespace engine_script_run_v19

}  // namespace pineforge
