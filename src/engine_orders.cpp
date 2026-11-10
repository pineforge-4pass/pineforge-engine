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
#include <pineforge/detail/cache_capacity.hpp>
#include <pineforge/cache_policy.hpp>
#include <pineforge/report_pages.hpp>
#include "retired_trade_history_read.hpp"
#include "run_cache_diagnostics.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pineforge {
using namespace internal;

namespace {
#if defined(__GNUC__) || defined(__clang__)
// Sixteen visits at about 7 ns each give about 112 ns of lead time,
// roughly two cache-miss latencies.
constexpr std::size_t kPrefetchAhead = 16;
#endif

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
    if (report_replay_state()) record_report_replay_close(trades_.size());
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

inline namespace engine_script_run_v20 {

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
    // A supplied run-lifetime capability, separate from the optional decoded
    // cache. Descriptors grow from its start; byte-reversed payload grows from
    // its end. Thus no population-sized heap directory or fixed row partition
    // is needed. Physical byte E-1-n contains logical payload byte n.
    struct HistoryBacking {
        int fd = -1;
        std::uint64_t extent = 0, index_end = 0, text_end = 0, high_water = 0;
        dev_t device{};
        ino_t inode{};
        HistoryFactory factory;
        ~HistoryBacking() { if (fd >= 0) ::close(fd); }
        bool bound() const noexcept { return fd >= 0; }
        void bind(HistoryFactory supplied, std::uint64_t bytes,
                  const HistoryBacking* previous = nullptr) {
            if (!supplied || bytes == 0)
                throw detail::ScratchIoError(EINVAL, "trade history capability extent");
            detail::ScratchFile::check_extent(bytes, 0);
            const int candidate = supplied(bytes);
            if (candidate < 0)
                throw detail::ScratchIoError(errno ? errno : EBADF, "trade history capability");
            // A broken supplier cannot transfer the source's very descriptor
            // to a copy/reset and then have its rejection close that source.
            if (previous && candidate == previous->fd)
                throw detail::ScratchIoError(EINVAL, "trade history capability aliases source");
            // Take ownership immediately, including every validation failure.
            struct Owner {
                int fd;
                ~Owner() { if (fd >= 0) ::close(fd); }
            } owner{candidate};
            struct stat info{};
            if (::fstat(candidate, &info) != 0)
                throw detail::ScratchIoError(errno, "trade history capability stat");
            if (previous && info.st_dev == previous->device && info.st_ino == previous->inode)
                throw detail::ScratchIoError(EINVAL, "trade history capability aliases source");
            const int flags = ::fcntl(candidate, F_GETFL);
            if (flags < 0)
                throw detail::ScratchIoError(errno, "trade history capability access");
            const int descriptor_flags = ::fcntl(candidate, F_GETFD);
            if (descriptor_flags < 0)
                throw detail::ScratchIoError(errno, "trade history capability flags");
            if (!S_ISREG(info.st_mode) || info.st_size < 0
                || static_cast<std::uint64_t>(info.st_size) != bytes
                || (flags & O_ACCMODE) != O_RDWR || (flags & O_APPEND) != 0
                || (descriptor_flags & FD_CLOEXEC) == 0)
                throw detail::ScratchIoError(EINVAL, "trade history capability geometry");
            factory = std::move(supplied);
            extent = bytes;
            device = info.st_dev;
            inode = info.st_ino;
            fd = std::exchange(owner.fd, -1);
        }
        void reserve(std::uint64_t descriptors, std::uint64_t text) {
            if (descriptors > extent || text > extent - descriptors)
                throw detail::ScratchIoError(ENOSPC, "trade history capability exhausted");
            index_end = descriptors;
            text_end = text;
            high_water = std::max(high_water, descriptors + text);
        }
        void transfer(bool write, std::uint64_t offset, void* data, size_type length) const {
            if (offset > extent || length > extent - offset)
                throw detail::ScratchIoError(EIO, "trade history capability range");
            auto* cursor = static_cast<unsigned char*>(data);
            for (size_type done = 0; done < length;) {
                const auto count = std::min(length - done,
                    static_cast<size_type>(std::numeric_limits<ssize_t>::max()));
                const auto result = write
                    ? ::pwrite(fd, cursor + done, count, static_cast<off_t>(offset + done))
                    : ::pread(fd, cursor + done, count, static_cast<off_t>(offset + done));
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0)
                    throw detail::ScratchIoError(result < 0 ? errno : EIO,
                        write ? "trade history write" : "trade history read");
                done += static_cast<size_type>(result);
            }
        }
        void read(bool text, std::uint64_t offset, void* data, size_type length) const {
            const auto end = text ? text_end : index_end;
            if (offset > end || length > end - offset)
                throw detail::ScratchIoError(EIO, "trade history read extent");
            if (!text) { transfer(false, offset, data, length); return; }
            std::array<unsigned char, 4096> window{};
            auto* output = static_cast<unsigned char*>(data);
            for (size_type done = 0; done < length;) {
                const auto count = std::min(window.size(), length - done);
                transfer(false, extent - offset - done - count, window.data(), count);
                std::reverse_copy(window.data(), window.data() + count, output + done);
                done += count;
            }
        }
        void write(bool text, std::uint64_t offset, const void* data, size_type length) {
            const auto stop = offset + length; // caller checked the positional extent
            reserve(text ? index_end : std::max(index_end, stop),
                    text ? std::max(text_end, stop) : text_end);
            if (!text) {
                transfer(true, offset, const_cast<void*>(data), length);
                return;
            }
            std::array<unsigned char, 4096> window{};
            const auto* input = static_cast<const unsigned char*>(data);
            for (size_type done = 0; done < length;) {
                const auto count = std::min(window.size(), length - done);
                std::reverse_copy(input + done, input + done + count, window.data());
                transfer(true, extent - offset - done - count, window.data(), count);
                done += count;
            }
        }
    } history;
    // Preserve legacy ScratchFile behavior when no capability was supplied.
    // The supplied path never opens a file, attaches an arena or resizes E.
    struct File {
        HistoryBacking& history;
        bool text;
        detail::ScratchFile legacy;
        File(HistoryBacking& owner, bool payload) : history(owner), text(payload) {}
        void bound_cache() { legacy.bound_cache(); }
        void set_directory(const std::string& value) { legacy.set_directory(value); }
        bool opened() const {
            return history.bound() ? history.index_end != 0 || history.text_end != 0
                                   : legacy.opened();
        }
        void open(const char* prefix) { if (!history.bound()) legacy.open(prefix); }
        void read(std::uint64_t offset, void* data, size_type length) const {
            if (history.bound()) history.read(text, offset, data, length);
            else legacy.read(offset, data, length);
        }
        void write(std::uint64_t offset, const void* data, size_type length) {
            detail::ScratchFile::check_extent(offset, length);
            if (history.bound()) history.write(text, offset, data, length);
            else legacy.write(offset, data, length);
        }
        std::uint64_t storage_bytes() const {
            return history.bound() ? (text ? history.text_end : history.index_end)
                                   : legacy.storage_bytes();
        }
        void truncate(std::uint64_t bytes) {
            if (history.bound()) {
                if (text) history.text_end = bytes;
                else history.index_end = bytes;
            } else legacy.truncate(bytes);
        }
        void release_prefix(std::uint64_t bytes) {
            if (history.bound())
                throw detail::ScratchIoError(EINVAL, "retained trade history prefix release");
            legacy.release_prefix(bytes);
        }
    };
    // The row window holds fixed descriptors; even the newest row's four
    // payloads already live in scratch. Cache only a fixed byte tail below.
    static constexpr size_type kPayloadTailBytes = kResidentRows * sizeof(Record);
    std::array<Record, kResidentRows> rows{};
    size_type first = 0, end = 0, resident_first = 0, count = 0, head = 0;
    File index{history, false}, payload{history, true};
    std::uint64_t payload_end = 0;
    // Physical origins only. Record keys, text extents, size/base, all money
    // folds and every hash remain in the original global coordinate space.
    std::uint64_t index_origin = 0, payload_origin = 0, payload_retired = 0;
    // File-relative zeroed prefixes. These watermarks move with release_prefix
    // and never claim a block until its write has completed.
    std::uint64_t index_zeroed_end = 0, payload_zeroed_end = 0;
    bool retiring = false;
    std::exception_ptr rollback_failure;
    std::array<char, kPayloadTailBytes> payload_tail{};
    std::string directory;
    mutable std::map<size_type, Trade> materialized;
    // Packed owning string bytes, rather than std::strings whose capacities
    // could exceed the advertised bound. Record retains exact numeric bits
    // and four string lengths; offsets remain scratch-only metadata.
    struct CacheEntry {
        Record record{};
        size_type global = 0, generation = 0;
        std::array<char, kCacheStringBytes> strings{};
        // Bits 0..7: strings_valid and min(entry_id.length,127) << 1.
        // Bits 8..9: bounded replacement rank. Generations step by 1024.
        [[gnu::always_inline]] bool strings_valid() const { return (generation & size_type{1}) != 0; }
    };
    static_assert(sizeof(CacheEntry) == kLedgerEntryBytes);
    static_assert(std::is_trivially_copyable_v<CacheEntry>);
    static_assert(std::is_trivially_destructible_v<CacheEntry>);
    static_assert(alignof(CacheEntry) <= alignof(std::max_align_t));
    // Scalar raw allocations plus placement construction have no array
    // cookie. Directory ownership and every page release live in this State.
    using CachePage = CacheEntry*;
    static_assert(sizeof(CachePage) == sizeof(void*));
    static_assert(kCachePageSlots % kCacheWays == 0);
    bool cache_auto = true;
    size_type cache_max_bytes = kDefaultLedgerCacheMaxBytes;
    size_type cache_rows = TradeLedger::cache_rows_for_capacity(
        CacheCapacity{kDefaultLedgerCacheMaxBytes});
    mutable CachePage* cache = nullptr;
    mutable size_type cache_count = 0, cache_allocated_slots = 0;
    static constexpr size_type kCacheGenerationStep = 1024;
    mutable size_type cache_generation = kCacheGenerationStep;
    mutable bool cache_allocation_failed = false;
    mutable size_type cache_failed_population = 0;
    mutable CacheStats cache_stats;
    // true while the ledger holds at most cache_rows rows: one slot per row (see cache_slot)
    bool cache_direct = true;
    // Zero-row policy reads use fixed owner storage without allocating pages.
    mutable CacheEntry uncached;
    detail::RunCacheSource cache_diagnostics{this, [](const void* owner) noexcept {
        return static_cast<std::uint64_t>(
            static_cast<const State*>(owner)->cache_resident_bytes());
    }};

    State() {
        index.bound_cache();
        payload.bound_cache();
    }
    ~State() { reset_cache(); }
    static void add_counter(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        value += std::min(amount, maximum - value);
    }
    size_type cache_resident_bytes() const noexcept {
        return sizeof(TradeLedger) + sizeof(State)
            + (cache ? cache_table_size() * sizeof(CachePage) : 0)
            + cache_allocated_slots * kLedgerEntryBytes;
    }
    static void validate_cache_rows(size_type rows) {
        if (rows > kMaxCacheRows)
            throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                               {{"input", "resident.ledger_rows"},
                                                {"reason", "value_above_maximum"}},
                                               "trade ledger: cache rows outside [0,1048576]");
    }
    [[gnu::always_inline]] size_type cache_slot(size_type global) const {
        // A global ordinal chooses a bounded set, never a history directory.
        // Direct mode: row number == slot; an id past the end (a prefetch) maps to slot 0, which is always valid.
        if (cache_direct) return global < cache_rows ? global : size_type{0};
        const auto sets = cache_rows / kCacheWays + (cache_rows % kCacheWays != 0);
        return (global % sets) * kCacheWays;
    }
    // Ways of the set at a slot: one in direct mode, up to four otherwise.
    [[gnu::always_inline]] size_type cache_ways(size_type slot) const {
        return cache_direct ? size_type{1} : std::min(kCacheWays, cache_rows - slot);
    }
    // Called after the number of rows changed: the mapping from rows to slots depends on whether
    // the ledger still fits the cache, so every flip drops the cached rows (a generation bump).
    void sync_cache_mode() {
        const bool direct = end <= cache_rows;
        if (direct != cache_direct) {
            cache_direct = direct;
            invalidate_cache();
        }
    }
    size_type cache_table_size() const {
        return cache_rows / kCachePageSlots + (cache_rows % kCachePageSlots != 0);
    }
    size_type cache_page_slots(size_type page_index) const {
        return std::min(kCachePageSlots, cache_rows - page_index * kCachePageSlots);
    }
    [[gnu::always_inline]] CacheEntry* cache_entry(size_type global) const {
        if (!cache || cache_rows == 0) return nullptr;
        const auto slot = cache_slot(global);
        const auto& page = cache[slot >> 8];
        return page ? &page[slot & (kCachePageSlots - 1)] : nullptr;
    }
    [[gnu::always_inline]] bool current(const CacheEntry& entry) const {
        return (entry.generation & ~(kCacheGenerationStep - 1)) == cache_generation;
    }
    void touch_cache_entry(CacheEntry* entries, size_type ways, CacheEntry* selected) const noexcept {
        constexpr size_type rank_mask = size_type{3} << 8;
        for (size_type i = 0; i < ways; ++i) {
            auto& entry = entries[i];
            if (!current(entry)) continue;
            const auto rank = &entry == selected ? size_type{0}
                : std::min(size_type{3}, ((entry.generation >> 8) & size_type{3}) + 1);
            entry.generation = (entry.generation & ~rank_mask) | (rank << 8);
        }
    }
    // A hit reorders ranks like a least-recently-used list: an entry that is
    // already the most recent changes nothing; otherwise only the entries more
    // recent than it age by one and it becomes the most recent.
    [[gnu::always_inline]]
    void touch_hit(CacheEntry* entries, size_type ways, CacheEntry* selected) const noexcept {
        constexpr size_type rank_shift = 8;
        constexpr size_type rank_mask = size_type{3} << rank_shift;
        const auto old_rank = selected->generation & rank_mask;
        if (old_rank == 0) return;
        for (size_type i = 0; i < ways; ++i) {
            auto& entry = entries[i];
            if (&entry == selected || !current(entry)) continue;
            if ((entry.generation & rank_mask) < old_rank)
                entry.generation += size_type{1} << rank_shift;
        }
        selected->generation &= ~rank_mask;
    }
    [[gnu::always_inline]] CacheEntry* cached(size_type global) const {
        auto* entries = cache_entry(global);
        if (!entries) return nullptr;
        const auto ways = cache_ways(cache_slot(global));
        for (size_type i = 0; i < ways; ++i) {
            add_counter(cache_stats.lookup_probe_count);
            auto* entry = entries + i;
            if (current(*entry) && entry->global == global) {
                touch_hit(entries, ways, entry);
                return entry;
            }
        }
        return nullptr;
    }
    void invalidate(size_type global) {
        if (auto* entry = cached(global)) {
            entry->generation = 0;
            --cache_count;
        }
    }
    void invalidate_cache() const {
        cache_count = 0;
        // Generations make publication/rewind invalidation O(1).
        // Even the wraparound fallback touches at most the configured budget.
        if (cache_generation > std::numeric_limits<size_type>::max() - kCacheGenerationStep) {
            if (cache)
                for (size_type page = 0; page < cache_table_size(); ++page)
                    if (cache[page])
                        for (size_type i = 0; i < cache_page_slots(page); ++i)
                            cache[page][i].generation = 0;
            cache_generation = kCacheGenerationStep;
        } else {
            cache_generation += kCacheGenerationStep;
        }
    }
    size_type retained_population() const noexcept {
        return end - (retiring ? first : 0);
    }
    bool allocation_blocked() const noexcept {
        return cache_allocation_failed
            && (!cache_auto || !TradeLedger::cache_retry_population_reached(
                cache_failed_population, retained_population()));
    }
    void fail_cache_allocation() const noexcept {
        cache_failed_population = retained_population();
        cache_allocation_failed = true;
        add_counter(cache_stats.allocation_bypass_count);
        cache_diagnostics.allocation_bypassed();
    }
    static bool cache_geometry_representable(size_type rows) noexcept {
        const auto pages = rows / kCachePageSlots + (rows % kCachePageSlots != 0);
        // This is not an allocation attempt. No failure is fabricated here.
        // Scalar allocations have no array cookie; each page is <= 256 slots.
        const auto maximum = static_cast<size_type>(std::numeric_limits<std::ptrdiff_t>::max());
        return pages <= maximum / sizeof(CachePage)
            && std::min(rows, kCachePageSlots) <= maximum / sizeof(CacheEntry);
    }
    void* allocate_cache_storage(size_type bytes) const noexcept {
        add_counter(cache_stats.allocation_attempt_count);
        void* result = ::operator new(bytes, std::nothrow);
        if (!result) fail_cache_allocation();
        return result;
    }
    bool ensure_cache() const noexcept {
        // Fixed geometry, including its entire directory, is derived once
        // from the allowance. A miss never rebuilds/reindexes retained rows.
        if (cache_rows == 0 || allocation_blocked()
            || !cache_geometry_representable(cache_rows)) return false;
        if (!cache) {
            auto* storage = static_cast<CachePage*>(
                allocate_cache_storage(cache_table_size() * sizeof(CachePage)));
            if (!storage) return false;
            for (size_type i = 0; i < cache_table_size(); ++i)
                ::new (static_cast<void*>(storage + i)) CachePage(nullptr);
            cache = storage;
        }
        return true;
    }
    CacheEntry* ensure_cache_entry(size_type global) const noexcept {
        if (!ensure_cache()) return nullptr;
        const auto slot = cache_slot(global);
        auto& page = cache[slot >> 8];
        if (!page) {
            const auto slots = cache_page_slots(slot >> 8);
            auto* storage = static_cast<CacheEntry*>(
                allocate_cache_storage(slots * sizeof(CacheEntry)));
            if (!storage) return nullptr;
            for (size_type i = 0; i < slots; ++i)
                ::new (static_cast<void*>(storage + i)) CacheEntry{};
            page = storage;
            cache_allocated_slots += slots;
        }
        auto* entries = page + (slot & (kCachePageSlots - 1));
        const auto ways = cache_ways(slot);
        CacheEntry* victim = entries;
        for (size_type i = 0; i < ways; ++i) {
            auto* entry = entries + i;
            if (!current(*entry) || entry->global == global) return entry;
            if (((entry->generation >> 8) & size_type{3})
                > ((victim->generation >> 8) & size_type{3})) victim = entry;
        }
        return victim;
    }
    // Only after invalidation. Associative placement has no contiguous ring
    // correspondence to retired globals; release all optional pages instead.
    // This bounded directory walk is never part of a cold lookup.
    void release_cache_pages(size_type, size_type, size_type, size_type) const noexcept {
        if (!cache) return;
        for (size_type page = 0; page < cache_table_size(); ++page) {
            ::operator delete(static_cast<void*>(cache[page]));
            cache[page] = nullptr;
        }
        cache_allocated_slots = 0;
    }
    void reset_cache() const noexcept {
        release_cache_pages(0, 0, 0, 0);
        ::operator delete(static_cast<void*>(cache));
        cache = nullptr;
        cache_count = 0;
    }
    const CacheEntry& remember(size_type global, const Record& record,
                               const Trade* row = nullptr) const {
        auto* destination = ensure_cache_entry(global);
        if (!destination) {
            uncached.record = record;
            uncached.global = global;
            uncached.generation = 0;
            return uncached;
        }
        auto& entry = *destination;
        if (current(entry)) --cache_count;
        entry.generation = 0;
        entry.record = record;
        entry.global = global;
        const std::array<StringExtent, 4> texts{{
            record.entry_id, record.entry_comment, record.exit_comment, record.exit_id}};
        size_type bytes = 0;
        bool strings_valid = true;
        for (const auto text : texts) {
            if (text.length > kCacheStringBytes - bytes) {
                strings_valid = false;
                break;
            }
            bytes += static_cast<size_type>(text.length);
        }
        if (strings_valid) {
            size_type offset = 0;
            if (row) {
                std::memcpy(entry.strings.data() + offset, row->entry_id.data(), row->entry_id.size());
                offset += row->entry_id.size();
                std::memcpy(entry.strings.data() + offset, row->entry_comment.data(), row->entry_comment.size());
                offset += row->entry_comment.size();
                std::memcpy(entry.strings.data() + offset, row->exit_comment.data(), row->exit_comment.size());
                offset += row->exit_comment.size();
                std::memcpy(entry.strings.data() + offset, row->exit_id.data(), row->exit_id.size());
            } else {
                for (const auto text : texts) {
                    read_string(text, entry.strings.data() + offset);
                    offset += static_cast<size_type>(text.length);
                }
            }
        }
        entry.generation = cache_generation | static_cast<size_type>(strings_valid)
            | (static_cast<size_type>(std::min<std::uint64_t>(record.entry_id.length, 127)) << 1);
        touch_cache_entry(cache_entry(global), cache_ways(cache_slot(global)), &entry);
        ++cache_count;
        return entry;
    }
    const CacheEntry& load(size_type global) const {
        if (const auto* entry = cached(global)) {
            add_counter(cache_stats.hot_hit_count);
            return *entry;
        }
        return load_miss(global);
    }
    // The part of load() after a failed lookup. A caller that has probed already
    // calls it directly, so that a miss is neither probed nor counted twice.
    const CacheEntry& load_miss(size_type global) const {
        add_counter(cache_stats.cold_miss_count);
        if (cache_rows == 0 || allocation_blocked()) {
            uncached.record = uncached_descriptor(global);
            uncached.global = global;
            uncached.generation = 0;
            return uncached;
        }
        return remember(global, uncached_descriptor(global));
    }
    template<auto Member>
    [[gnu::always_inline]]
    auto numeric_field(size_type global) const {
        check(global);
        if (const auto* entry = cached(global)) {
            add_counter(cache_stats.hot_hit_count);
            return entry->record.*Member;
        }
        // Read the private entry directly; no Trade or owning string is
        // constructed for a numeric field.
        return load_miss(global).record.*Member;
    }
    template<StringExtent Record::*Member>
    // Owning read; the compiler decides inlining of its string construction.
    std::string string_field(size_type global) const {
        check(global);
        const auto* hit = cached(global);
        if (hit) add_counter(cache_stats.hot_hit_count);
        const auto& entry = hit ? *hit : load_miss(global);
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
    [[gnu::always_inline]]
    static bool packed_equals(const char* lhs, size_type n, std::string_view rhs) {
        if (n <= sizeof(std::uint64_t)) {
            if (rhs.size() != n) return false;
            std::uint64_t left = 0, right = 0;
            // Fixed-size copies read exactly n bytes, with no library call.
            if (n == sizeof(std::uint64_t)) {
                std::memcpy(&left, lhs, sizeof(std::uint64_t));
                std::memcpy(&right, rhs.data(), sizeof(std::uint64_t));
            } else {
                auto* left_bytes = reinterpret_cast<char*>(&left);
                auto* right_bytes = reinterpret_cast<char*>(&right);
                if (n & 4) {
                    std::memcpy(left_bytes, lhs, 4);
                    std::memcpy(right_bytes, rhs.data(), 4);
                }
                if (n & 2) {
                    std::memcpy(left_bytes + (n & 4), lhs + (n & 4), 2);
                    std::memcpy(right_bytes + (n & 4), rhs.data() + (n & 4), 2);
                }
                if (n & 1) {
                    std::memcpy(left_bytes + (n & 6), lhs + (n & 6), 1);
                    std::memcpy(right_bytes + (n & 6), rhs.data() + (n & 6), 1);
                }
            }
            return left == right;
        }
        return std::string_view(lhs, n) == rhs;
    }
    template<StringExtent Record::*Member>
    [[gnu::always_inline]]
    bool string_equals(size_type global, std::string_view rhs) const {
        check(global);
#if defined(__GNUC__) || defined(__clang__)
        if (const auto* ahead = cache_entry(global + kPrefetchAhead)) {
            __builtin_prefetch(&ahead->generation, 0, 3);
            __builtin_prefetch(&ahead->record.entry_id.length, 0, 3);
            __builtin_prefetch(ahead->strings.data(), 0, 3);
        }
#endif
        const auto* cached_entry = cached(global);
        if constexpr (Member == &Record::entry_id) {
            if (cached_entry && cached_entry->strings_valid()) {
                const auto n = (cached_entry->generation >> 1) & size_type{127};
                if (n != 127) return packed_equals(cached_entry->strings.data(), n, rhs);
            }
        }
        const auto& entry = cached_entry ? *cached_entry : load(global);
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
        return packed_equals(entry.strings.data() + offset,
                             static_cast<size_type>(text.length), rhs);
    }
    static std::uint64_t absolute_record_offset(size_type global) {
        if (global > std::numeric_limits<std::uint64_t>::max() / sizeof(Record))
            throw detail::ScratchIoError(EFBIG, "trade ledger offset");
        return static_cast<std::uint64_t>(global) * sizeof(Record);
    }
    std::uint64_t record_offset(size_type global) const {
        return absolute_record_offset(global) - index_origin;
    }
    [[noreturn, gnu::cold, gnu::noinline]]
    static void throw_check_failure(bool retired) {
        if (retired) throw detail::RetiredTradeHistoryRead();
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: row outside resident window");
    }
    [[gnu::always_inline]]
    void check(size_type global, bool mutable_row = false) const {
        if (rollback_failure) std::rethrow_exception(rollback_failure);
        if (retiring && global < first) throw_check_failure(true);
        if (global >= end || (mutable_row && global < first)) throw_check_failure(false);
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
    void read_payload(std::uint64_t offset, void* result, size_type length) const {
        add_counter(cache_stats.string_read_count);
        payload.read(offset, result, length);
        add_counter(cache_stats.string_read_bytes, length);
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
                read_payload(text.offset - payload_origin, result, length);
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
            read_payload(text.offset + offset - payload_origin, chunk.data(), count);
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
        if (text.size() == old.length && string_equals_chunks(old, text)) return old;
        return put_string(text);
    }
    bool put_append_strings(Record& record, const Trade& row) {
        const auto& entry_id = row.entry_id;
        const auto& entry_comment = row.entry_comment;
        const auto& exit_comment = row.exit_comment;
        const auto& exit_id = row.exit_id;
        size_type bytes = 0, nonempty = 0;
        // Subtraction keeps the sum within the fixed payload-tail capacity.
        if (entry_id.size() > kPayloadTailBytes - bytes) return false;
        bytes += entry_id.size();
        nonempty += !entry_id.empty();
        if (entry_comment.size() > kPayloadTailBytes - bytes) return false;
        bytes += entry_comment.size();
        nonempty += !entry_comment.empty();
        if (exit_comment.size() > kPayloadTailBytes - bytes) return false;
        bytes += exit_comment.size();
        nonempty += !exit_comment.empty();
        if (exit_id.size() > kPayloadTailBytes - bytes) return false;
        bytes += exit_id.size();
        nonempty += !exit_id.empty();
        if (nonempty <= 1) return false;
        auto offset = payload_end;
        detail::ScratchFile::check_extent(offset, entry_id.size());
        record.entry_id = {offset, static_cast<std::uint64_t>(entry_id.size())};
        offset += entry_id.size();
        detail::ScratchFile::check_extent(offset, entry_comment.size());
        record.entry_comment = {offset, static_cast<std::uint64_t>(entry_comment.size())};
        offset += entry_comment.size();
        detail::ScratchFile::check_extent(offset, exit_comment.size());
        record.exit_comment = {offset, static_cast<std::uint64_t>(exit_comment.size())};
        offset += exit_comment.size();
        detail::ScratchFile::check_extent(offset, exit_id.size());
        record.exit_id = {offset, static_cast<std::uint64_t>(exit_id.size())};
        offset += exit_id.size();
        // Capacity = kResidentRows * sizeof(Record), matching the guard above.
        char buffer[kPayloadTailBytes];
        size_type copied = 0;
        if (!entry_id.empty()) std::memcpy(buffer + copied, entry_id.data(), entry_id.size());
        copied += entry_id.size();
        if (!entry_comment.empty()) std::memcpy(buffer + copied, entry_comment.data(), entry_comment.size());
        copied += entry_comment.size();
        if (!exit_comment.empty()) std::memcpy(buffer + copied, exit_comment.data(), exit_comment.size());
        copied += exit_comment.size();
        if (!exit_id.empty()) std::memcpy(buffer + copied, exit_id.data(), exit_id.size());
        // A failed write leaves end/tail unchanged. The row's descriptor is
        // published only after all payload bytes succeed, as with put_string.
        payload.write(payload_end - payload_origin, buffer, bytes);
        payload_end = offset;
        return true;
    }
    Record store(size_type global, const Trade& row, const Record* previous = nullptr) {
        if (rollback_failure) std::rethrow_exception(rollback_failure);
        index.open("pineforge-trade-index");
        payload.open("pineforge-trade-payload");
        const auto saved_payload_end = payload_end;
        const auto saved_index_bytes = index.storage_bytes();
        const auto saved_payload_bytes = payload.storage_bytes();
        const auto offset = record_offset(global);
        detail::ScratchFile::check_extent(offset, sizeof(Record));
        if (history.bound()) {
            // Reserve the complete transaction before the first write,
            // including changed text while its old extents remain live.
            auto pending = payload_end;
            const std::array<const std::string*, 4> values{{
                &row.entry_id, &row.entry_comment, &row.exit_comment, &row.exit_id}};
            const std::array<StringExtent, 4> old = previous
                ? std::array<StringExtent, 4>{{previous->entry_id, previous->entry_comment,
                                               previous->exit_comment, previous->exit_id}}
                : std::array<StringExtent, 4>{};
            for (size_type i = 0; i < values.size(); ++i) {
                if (previous && values[i]->size() == old[i].length
                    && string_equals_chunks(old[i], *values[i])) continue;
                detail::ScratchFile::check_extent(pending, values[i]->size());
                pending += values[i]->size();
            }
            history.reserve(std::max(saved_index_bytes, offset + sizeof(Record)), pending);
        }
        bool descriptor_attempted = false;
        Record record{};
        try {
            record.entry_time = row.entry_time; record.exit_time = row.exit_time;
            record.entry_price = row.entry_price; record.exit_price = row.exit_price;
            record.qty = row.qty; record.pnl = row.pnl; record.pnl_pct = row.pnl_pct;
            record.entry_bar_index = row.entry_bar_index; record.exit_bar_index = row.exit_bar_index;
            // Replacements retain equal extents, including embedded NULs.
            // Payload growth follows actual string changes, not numeric updates.
            if (previous || !put_append_strings(record, row)) {
                record.entry_id = previous ? reuse_string(row.entry_id, previous->entry_id)
                                           : put_string(row.entry_id);
                record.entry_comment = previous ? reuse_string(row.entry_comment, previous->entry_comment)
                                                : put_string(row.entry_comment);
                record.exit_comment = previous ? reuse_string(row.exit_comment, previous->exit_comment)
                                               : put_string(row.exit_comment);
                record.exit_id = previous ? reuse_string(row.exit_id, previous->exit_id)
                                          : put_string(row.exit_id);
            }
            record.max_runup = row.max_runup; record.max_drawdown = row.max_drawdown;
            record.commission = row.commission; record.entry_incarnation = row.entry_incarnation;
            record.close_cause = row.close_cause; record.is_long = row.is_long;
            record.exit_from_bracket = row.exit_from_bracket; record.open_at_end = row.open_at_end;
            // Skip only a descriptor whose whole representation already matches.
            if (!previous || std::memcmp(previous, &record, sizeof(record)) != 0) {
                descriptor_attempted = true;
                index.write(offset, &record, sizeof(record));
            }
        } catch (...) {
            const auto first_failure = std::current_exception();
            try {
                // A short pwrite may already have damaged a live descriptor.
                // Restore it before exposing the old logical generation.
                if (previous && descriptor_attempted)
                    index.write(offset, previous, sizeof(*previous));
                index.truncate(saved_index_bytes);
                payload.truncate(saved_payload_bytes);
            } catch (...) {
                // An unrepairable owner cannot serve cached/resident bytes as
                // if the transaction had succeeded. Keep the first typed cause.
                rollback_failure = first_failure;
                invalidate_cache();
            }
            payload_end = saved_payload_end;
            std::rethrow_exception(first_failure);
        }
        // Publish the fixed payload tail only after the descriptor commits.
        // Reused extents must not overwrite slots belonging to newer bytes.
        if (record.entry_id.offset >= saved_payload_end)
            remember_payload(record.entry_id.offset, row.entry_id);
        if (record.entry_comment.offset >= saved_payload_end)
            remember_payload(record.entry_comment.offset, row.entry_comment);
        if (record.exit_comment.offset >= saved_payload_end)
            remember_payload(record.exit_comment.offset, row.exit_comment);
        if (record.exit_id.offset >= saved_payload_end)
            remember_payload(record.exit_id.offset, row.exit_id);
        return record;
    }
    static constexpr std::uint64_t kZeroBlock = 4096;
    static_assert(detail::ScratchArena::kChunkBytes % kZeroBlock == 0);
    static void erase_bytes(File& file, std::uint64_t first,
                            std::uint64_t stop) {
        const std::array<unsigned char, kZeroBlock> zeros{};
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
        erase_payload_tail(text);
    }
    void erase_payload_tail(StringExtent text) {
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
    static void release_head(File& file, std::uint64_t& origin,
                             std::uint64_t& zeroed_end, std::uint64_t new_end,
                             bool empty) {
        if (new_end == origin && !empty) return;
        if (empty) {
            // No suffix, including an all-empty-string payload: release the
            // final partial chunk too. Keep the configured backing open.
            if (file.opened()) file.truncate(0);
            origin = new_end;
            zeroed_end = 0;
            return;
        }
        const auto bytes = (new_end - origin) / detail::ScratchArena::kChunkBytes
            * detail::ScratchArena::kChunkBytes;
        if (bytes != 0) {
            // Rebase before zeroing: release_prefix compacts the surviving
            // suffix, so zeroing before it would clear bytes that are copied
            // or discarded. A pending range wholly below the release drops
            // with the prefix.
            file.release_prefix(bytes);
            origin += bytes;
            zeroed_end = zeroed_end > bytes ? zeroed_end - bytes : 0;
        }
        const auto dead_end = new_end - origin;
        // Everything below dead_end is dead. Start at zeroed_end, NOT at the
        // previous dead end: the bytes in between are the pending tail of the
        // earlier retirements and must be zeroed with the block they complete.
        const auto first = zeroed_end;
        const auto aligned_end = dead_end - dead_end % kZeroBlock;
        const std::array<unsigned char, kZeroBlock> zeros{};
        for (auto offset = first; offset < aligned_end;) {
            const auto count = static_cast<size_type>(
                std::min<std::uint64_t>(zeros.size(), aligned_end - offset));
            file.write(offset, zeros.data(), count);
            offset += count;
            zeroed_end = offset;
        }
        // At most one head chunk remains: completed blocks are zero-filled,
        // while the newest dead bytes below one zero-fill block remain pending.
    }
    void retire(size_type global_end) {
        if (!retiring || history.bound() || global_end == first) return;
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
        const auto index_stop = absolute_record_offset(global_end);
        const bool index_empty = global_end == end;
        const bool payload_empty = text_first == payload_end;
        // The payload plan covers everything below text_first: complete
        // chunks are released, and release_head zeroes completed blocks while
        // leaving only its newest sub-block tail pending. An empty payload is
        // truncated in full.
        // No strings are shared between rows. Erase retired text even when a
        // replacement put it AFTER a surviving row's earliest payload.
        for (size_type i = first; i < global_end; ++i) {
            const auto row = descriptor(i);
            for (const auto text : {row.entry_id, row.entry_comment,
                                    row.exit_comment, row.exit_id}) {
                if (text.length == 0) continue;
                validate_string(text);
                const auto erase_first = std::max(text.offset, text_first);
                const auto erase_stop = text.offset + text.length;
                if (erase_first < erase_stop)
                    erase_bytes(payload, erase_first - payload_origin,
                                erase_stop - payload_origin);
                erase_payload_tail(text);
            }
        }
        release_head(index, index_origin, index_zeroed_end, index_stop, index_empty);
        release_head(payload, payload_origin, payload_zeroed_end, text_first, payload_empty);
        payload_retired = text_first;
        invalidate_cache();
        release_cache_pages(first, global_end, global_end, end);
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
    Record uncached_descriptor(size_type global) const {
        if (global >= resident_first) return rows[slot(global)];
        Record record{};
        add_counter(cache_stats.descriptor_read_count);
        index.read(record_offset(global), &record, sizeof(record));
        add_counter(cache_stats.descriptor_read_bytes, sizeof(record));
        return record;
    }
    Record descriptor(size_type global) const {
        if (const auto* entry = cached(global)) return entry->record;
        return uncached_descriptor(global);
    }
    void evict() {
        rows[head] = Record{};
        head = (head + 1) % kResidentRows;
        ++resident_first;
        --count;
    }
};

TradeLedger::TradeLedger() : state_(std::make_unique<State>()) {}
TradeLedger::TradeLedger(size_type cache_rows) : TradeLedger() {
    set_cache_rows(cache_rows);
}
TradeLedger::~TradeLedger() = default;

TradeLedger::TradeLedger(const TradeLedger& other)
    : TradeLedger() {
    if (other.state_->rollback_failure)
        std::rethrow_exception(other.state_->rollback_failure);
    // The byte-policy path may exceed the legacy row setter's ceiling.
    // This fresh State copies an already validated physical allowance.
    state_->cache_rows = other.state_->cache_rows;
    state_->cache_max_bytes = other.state_->cache_max_bytes;
    state_->cache_auto = other.state_->cache_auto;
    set_scratch_directory(other.state_->directory);
    if (other.state_->history.bound()) {
        state_->history.bind(other.state_->history.factory, other.state_->history.extent,
                             &other.state_->history);
    }
    // A cold source remains unallocated. Reuse already cached source rows;
    // neither the source nor its copy allocates pages just for copying.
    if (other.state_->retiring) {
        // A copy owns the same logical window, without trying to resurrect an
        // intentionally retired prefix. Rebuild only the retained payload.
        state_->retiring = true;
        state_->first = state_->end = state_->resident_first = other.base();
        state_->sync_cache_mode();
        state_->index_origin = State::absolute_record_offset(other.base());
    }
    for (size_type i = state_->end; i < other.size(); ++i) {
        const auto* entry = other.state_->cached(i);
        push_back(entry && entry->strings_valid()
                      ? State::decode_cached(*entry)
                      : other.state_->decode(entry ? entry->record
                                                   : other.state_->descriptor(i)));
    }
    seal(other.base());
    // Fresh owner: a source's optional allocation failure is not a failure
    // of this copy. Its data/backing failures above still propagate exactly.
}

TradeLedger& TradeLedger::operator=(const TradeLedger& other) {
    if (this != &other) {
        TradeLedger next(other);
        state_.swap(next.state_);
    }
    return *this;
}

TradeLedger::TradeLedger(TradeLedger&& other) : TradeLedger() {
    // Leave an empty, usable owner with the same declared physical policy.
    state_->cache_auto = other.state_->cache_auto;
    state_->cache_rows = other.state_->cache_rows;
    state_->cache_max_bytes = other.state_->cache_max_bytes;
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
    next->cache_auto = state_->cache_auto;
    next->cache_rows = state_->cache_rows;
    next->cache_max_bytes = state_->cache_max_bytes;
    next->directory = state_->directory;
    next->index.set_directory(next->directory);
    next->payload.set_directory(next->directory);
    if (state_->history.bound()) {
        next->history.bind(state_->history.factory, state_->history.extent, &state_->history);
    }
    state_.swap(next);
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
    state_->retiring = enabled && !state_->history.bound();
}

void TradeLedger::check_history_read(size_type global) const { state_->check(global); }

std::array<std::uint64_t, 2> TradeLedger::scratch_storage_bytes() const {
    if (state_->history.bound()) {
        struct stat info{};
        if (::fstat(state_->history.fd, &info) != 0)
            throw detail::ScratchIoError(errno, "trade history capability stat");
        if (info.st_size < 0)
            throw detail::ScratchIoError(EIO, "trade history capability extent");
        // One actual file extent, counted once. Not its populated frontiers,
        // resident pages, allocated blocks or memory-domain charge.
        return {static_cast<std::uint64_t>(info.st_size), 0};
    }
    return {state_->index.storage_bytes(), state_->payload.storage_bytes()};
}

void TradeLedger::bind_retained_history(HistoryFactory factory, std::uint64_t extent) {
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened()
        || state_->history.bound())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade history capability changed after binding or append");
    state_->history.bind(std::move(factory), extent);
    // A full retained resource never acquires no-history erasure semantics.
    state_->retiring = false;
}

std::array<std::uint64_t, 4> TradeLedger::retained_history_bytes() const {
    const auto& history = state_->history;
    return {history.extent, history.index_end, history.text_end, history.high_water};
}

TradeLedger::TradeLedger(CacheCapacity capacity) : TradeLedger(size_type{0}) {
    set_cache_capacity(capacity);
}

TradeLedger::TradeLedger(const LedgerCachePolicy& policy) : TradeLedger() {
    if (!policy.automatic())
        set_cache_capacity(CacheCapacity{static_cast<size_type>(policy.capacity_bytes)});
}

void TradeLedger::set_cache_capacity(CacheCapacity capacity) {
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened())
        throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                           {{"input", "cache_policy"}, {"reason", "frozen"}},
                                           "trade ledger: cache capacity changed after append");
    state_->reset_cache();
    state_->cache_max_bytes = capacity.capacity_bytes;
    state_->cache_rows = cache_rows_for_capacity(capacity);
    state_->cache_auto = false;
}

TradeLedger::size_type TradeLedger::cache_rows_for_capacity(CacheCapacity capacity) noexcept {
    // Complete pages fund 256 entries plus one pointer. The remainder must
    // pay for another pointer before it can fund any clipped-page entries.
    constexpr auto page_bytes = kCachePageSlots * kLedgerEntryBytes
        + sizeof(State::CachePage);
    const auto complete_pages = capacity.capacity_bytes / page_bytes;
    const auto remainder = capacity.capacity_bytes % page_bytes;
    const auto partial_rows = remainder > sizeof(State::CachePage)
        ? (remainder - sizeof(State::CachePage)) / kLedgerEntryBytes : 0;
    return complete_pages * kCachePageSlots + partial_rows;
}

void TradeLedger::set_cache_auto() {
    set_ledger_cache_max_bytes(kDefaultLedgerCacheMaxBytes);
}

void TradeLedger::set_ledger_cache_max_bytes(size_type bytes) {
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened())
        throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                           {{"input", "cache_policy"}, {"reason", "frozen"}},
                                           "trade ledger: cache policy changed after append");
    state_->reset_cache();
    state_->cache_max_bytes = bytes;
    state_->cache_rows = cache_rows_for_capacity(CacheCapacity{bytes});
    state_->cache_auto = true;
}

TradeLedger::size_type TradeLedger::ledger_cache_max_bytes() const {
    return state_->cache_max_bytes;
}

void TradeLedger::set_cache_rows(size_type rows) {
    State::validate_cache_rows(rows);
    if (state_->end != 0 || state_->index.opened() || state_->payload.opened())
        throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                                           {{"input", "resident.ledger_rows"}, {"reason", "frozen"}},
                                           "trade ledger: cache rows changed after spill");
    state_->reset_cache();
    state_->cache_max_bytes = cache_capacity_bytes(rows);
    state_->cache_rows = rows;
    state_->cache_auto = false;
}
TradeLedger::size_type TradeLedger::cache_rows() const { return state_->cache_rows; }
TradeLedger::size_type TradeLedger::resident_cache_bytes(size_type rows) {
    State::validate_cache_rows(rows);
    return sizeof(TradeLedger) + sizeof(State) + cache_capacity_bytes(rows);
}
TradeLedger::size_type TradeLedger::cache_resident_bytes() const {
    return state_->cache_resident_bytes();
}
TradeLedger::size_type TradeLedger::cache_size() const { return state_->cache_count; }
bool TradeLedger::cache_allocation_failed() const noexcept {
    return state_->cache_allocation_failed;
}
TradeLedger::CacheStats TradeLedger::read_cache_stats() const noexcept {
    auto stats = state_->cache_stats;
    stats.bypass_reason = state_->cache_allocation_failed
        ? CacheBypassReason::allocation_failed : CacheBypassReason::none;
    stats.failed_population = state_->cache_failed_population;
    stats.allocation_retry_blocked = state_->allocation_blocked();
    return stats;
}

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
    const auto& entry = state_->load(global);
    return entry.strings_valid() ? State::decode_cached(entry) : state_->decode(entry.record);
}

Trade TradeLedger::operator[](size_type global) const { return at(global); }
Trade TradeLedger::at(size_type global) const {
    state_->check(global);
    return read(global);
}

Trade TradeLedger::back() const {
    if (empty())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: no resident last row");
    return read(size() - 1);
}

Trade TradeLedger::numeric_at(size_type global) const {
    state_->check(global);
    return State::decode_numeric(state_->descriptor(global));
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
bool TradeLedger::entry_id_matches(int index, std::string_view rhs) const {
    if (index < 0 || static_cast<size_type>(index) >= state_->end) return rhs.empty();
    return state_->string_equals<&State::Record::entry_id>(static_cast<size_type>(index), rhs);
}
bool TradeLedger::exit_id_matches(int index, std::string_view rhs) const {
    if (index < 0 || static_cast<size_type>(index) >= state_->end) return rhs.empty();
    return state_->string_equals<&State::Record::exit_id>(static_cast<size_type>(index), rhs);
}
bool TradeLedger::entry_comment_matches(int index, std::string_view rhs) const {
    if (index < 0 || static_cast<size_type>(index) >= state_->end) return rhs.empty();
    return state_->string_equals<&State::Record::entry_comment>(static_cast<size_type>(index), rhs);
}
bool TradeLedger::exit_comment_matches(int index, std::string_view rhs) const {
    if (index < 0 || static_cast<size_type>(index) >= state_->end) return rhs.empty();
    return state_->string_equals<&State::Record::exit_comment>(static_cast<size_type>(index), rhs);
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
    const auto legacy = state_->materialized.find(global);
    Trade legacy_value{};
    if (legacy != state_->materialized.end()) legacy_value = row;
    const auto record = state_->store(global, row, &previous);
    state_->invalidate(global);
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
    if (legacy != state_->materialized.end()) legacy->second = std::move(legacy_value);
    if (state_->cache) state_->remember(global, record, &row);
}

void TradeLedger::modify(size_type global, const std::function<void(Trade&)>& edit) {
    state_->check(global, true);
    Trade row = at(global);
    edit(row);
    replace(global, row);
}

void TradeLedger::push_back(const Trade& trade) {
    if (size() == std::numeric_limits<size_type>::max())
        throw std::length_error("trade ledger: logical end exhausted");
    // Copies and write-only runs keep a cold cache unallocated. Optional
    // cache fills follow backing publication and cannot refuse the append.
    const auto record = state_->store(size(), trade);
    if (state_->count == kResidentRows) state_->evict();
    state_->rows[(state_->head + state_->count) % kResidentRows] = record;
    ++state_->count;
    ++state_->end;
    state_->sync_cache_mode();
    if (state_->cache) state_->remember(state_->end - 1, record, &trade);
}

void TradeLedger::push_back(Trade&& trade) { push_back(static_cast<const Trade&>(trade)); }

void TradeLedger::resize(size_type global_end) {
    if (state_->rollback_failure) std::rethrow_exception(state_->rollback_failure);
    if (global_end < state_->first)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: rewind below resident window");
    state_->invalidate_cache();
    if (global_end < size()) {
        if (state_->retiring)
            state_->release_cache_pages(global_end, size(), state_->first, global_end);
        // Re-cuts may rewind far below the descriptor window. All row bytes
        // are already external; discard the bounded descriptor cache.
        while (state_->count != 0) state_->evict();
        state_->end = state_->resident_first = global_end;
        state_->sync_cache_mode();
        state_->materialized.erase(state_->materialized.lower_bound(global_end),
                                    state_->materialized.end());
    } else {
        while (size() < global_end) push_back(Trade{});
        state_->invalidate_cache();
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
    state_->check(global);
    const auto found = state_->materialized.find(global);
    if (found != state_->materialized.end()) return found->second;
    return state_->materialized.emplace(global, read(global)).first->second;
}

void TradeLedger::seal(size_type global_end) {
    if (state_->rollback_failure) std::rethrow_exception(state_->rollback_failure);
    if (global_end < state_->first || global_end > size())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                       "trade ledger: seal outside resident window");
    // The consumer has already emitted T and folded all logical identities.
    // Only an explicitly admitted sink run drops its physical history.
    state_->retire(global_end);
    // Publication changes no retained row content: cached entries stay valid,
    // so sealed reads can reuse them without making a closedtrades loop cold.
    // Physical retirement invalidates its cache and refuses erased rows.
    while (state_->count != 0 && state_->resident_first < global_end) state_->evict();
    state_->first = global_end;
}

// This adapter is deliberately private until P2/R compose the engine's frozen
// report generation. The existing report contract supplies all wire types.
class TradeLedger::Source final : public report::TradeSourceV1 {
    const TradeLedger& ledger_;
    using Index = report::LogicalIndexV1;
    using Row = report::TradeRowV1;
    using Text = report::TradeTextV1;
    void range(Index first, Index count) const {
        const auto total = size();
        if (first > total || count > total - first)
            throw std::out_of_range("trade source range");
    }
    State::StringExtent text(Index index, Text field) const {
        // Validate selectors even on an otherwise invalid/empty request.
        if (field != Text::EntryId && field != Text::EntryComment
            && field != Text::ExitId && field != Text::ExitComment)
            throw std::invalid_argument("trade source text selector");
        if (index >= size()) throw std::out_of_range("trade source index");
        ledger_.state_->check(static_cast<size_type>(index));
        const auto row = ledger_.state_->descriptor(static_cast<size_type>(index));
        switch (field) {
        case Text::EntryId: return row.entry_id;
        case Text::EntryComment: return row.entry_comment;
        case Text::ExitId: return row.exit_id;
        case Text::ExitComment: return row.exit_comment;
        }
        throw std::invalid_argument("trade source text selector");
    }
    report::VisitResultV1 visit_rows(Index from, Index count, void* context,
                                     report::TradeVisitorV1 visitor, bool backward) const {
        if (!visitor) throw std::invalid_argument("trade source null visitor");
        range(from, count);
        report::VisitResultV1 result{};
        for (Index step = 0; step < count; ++step) {
            const auto index = backward ? from + count - 1 - step : from + step;
            const Row value = at(index);
            ++result.visited;
            if (!visitor(context, index, value)) return result;
        }
        result.complete = true;
        return result;
    }
public:
    explicit Source(const TradeLedger& ledger) : ledger_(ledger) {}
    Index size() const override { return ledger_.size(); }
    Row at(Index index) const override {
        if (index >= size()) throw std::out_of_range("trade source index");
        const Trade value = ledger_.numeric_at(static_cast<size_type>(index));
        Row row{};
        auto& out = row.value;
        out.entry_time = value.entry_time; out.exit_time = value.exit_time;
        out.entry_price = value.entry_price; out.exit_price = value.exit_price;
        out.pnl = value.pnl; out.pnl_pct = value.pnl_pct;
        out.is_long = value.is_long ? 1 : 0;
        out.max_runup = value.max_runup; out.max_drawdown = value.max_drawdown;
        out.qty = value.qty; out.commission = value.commission;
        out.entry_bar_index = value.entry_bar_index; out.exit_bar_index = value.exit_bar_index;
        out.open_at_end = value.open_at_end ? 1 : 0;
        row.entry_incarnation = value.entry_incarnation;
        // Same precedence as BacktestEngine::classify_report_close_cause.
        const auto cause = value.open_at_end ? execution::CloseCause::RangeEnd
            : value.close_cause != execution::CloseCause::Unspecified ? value.close_cause
            : value.exit_from_bracket ? execution::CloseCause::Bracket
                                     : execution::CloseCause::Script;
        row.close_cause = static_cast<std::int32_t>(cause);
        row.exit_from_bracket = value.exit_from_bracket ? 1u : 0u;
        return row;
    }
    size_type read_page(Index from, Row* out, size_type capacity) const override {
        if (!out && capacity != 0) throw std::invalid_argument("trade source null buffer");
        range(from, 0);
        const auto count = static_cast<size_type>(std::min<Index>(capacity, size() - from));
        for (size_type i = 0; i < count; ++i) out[i] = at(from + i);
        return count;
    }
    report::VisitResultV1 visit(Index from, Index count, void* context,
                                 report::TradeVisitorV1 visitor) const override {
        return visit_rows(from, count, context, visitor, false);
    }
    report::VisitResultV1 visit_backward(Index from, Index count, void* context,
                                          report::TradeVisitorV1 visitor) const override {
        return visit_rows(from, count, context, visitor, true);
    }
    std::uint64_t text_size(Index index, Text field) const override {
        const auto extent = text(index, field);
        ledger_.state_->validate_string(extent);
        return extent.length;
    }
    size_type read_text(Index index, Text field, std::uint64_t offset,
                        char* out, size_type capacity) const override {
        if (!out && capacity != 0) throw std::invalid_argument("trade source null text buffer");
        const auto extent = text(index, field);
        ledger_.state_->validate_string(extent);
        if (offset > extent.length) throw std::out_of_range("trade source text offset");
        const auto count = static_cast<size_type>(
            std::min<std::uint64_t>(capacity, extent.length - offset));
        if (count != 0)
            ledger_.state_->read_string({extent.offset + offset, count}, out);
        return count;
    }
};

std::unique_ptr<report::TradeSourceV1> TradeLedger::retained_trade_source() const {
    if (state_->rollback_failure) std::rethrow_exception(state_->rollback_failure);
    return std::make_unique<Source>(*this);
}

} // inline namespace engine_script_run_v20

}  // namespace pineforge
