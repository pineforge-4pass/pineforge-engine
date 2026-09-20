#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "../engine_internal.hpp"
#include "../timezone.hpp"
#include "../native_execution_consumer.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pineforge {
using namespace source;

namespace {

bool priced_opening_trigger(const native_order::Trigger& trigger) {
    return std::holds_alternative<native_order::Stop>(trigger)
        || std::holds_alternative<native_order::Limit>(trigger)
        || std::holds_alternative<native_order::StopLimit>(trigger);
}

// ab9714be pine_fills.cpp:35-44: a priced entry masks the extreme traversed
// before the fill position on the assumed OHLC path.
void set_entry_fill_excursion_masks(PyramidEntry& pe, const Bar& bar, double fill_price) {
    double fill_pos = 0.0;
    if (!internal::first_touch_position(bar, fill_price, &fill_pos)) return;
    const bool high_first = internal::bar_path_uses_high_first(bar);
    const double high_pos = high_first ? 1.0 : 2.0;
    const double low_pos  = high_first ? 2.0 : 1.0;
    pe.skip_entry_bar_high = (high_pos < fill_pos);
    pe.skip_entry_bar_low  = (low_pos < fill_pos);
}

// The kernel still samples the delivered path at every driver point, which
// can book an entry-bar extreme the owner masks out (a gap-through stop is
// filled 1 ulp beyond the open). On the entry bar of a masked lot the host's
// own masked H/L/C walk is authoritative, so it replaces whatever the path
// sampling carried.

void replace_masked_entry_bar_extremes(std::vector<PyramidEntry>& lots, PositionSide side,
                                       int bar_index, const Bar& bar) {
    if (side == PositionSide::FLAT || lots.empty()) return;
    if (!std::isfinite(bar.high) || !std::isfinite(bar.low) || !std::isfinite(bar.close))
        return;
    const bool is_long = (side == PositionSide::LONG);
    for (auto& pe : lots) {
        if (pe.entry_bar_index != bar_index) continue;
        if (!pe.skip_entry_bar_high && !pe.skip_entry_bar_low) continue;
        const double pe_hi = pe.skip_entry_bar_high ? pe.price : bar.high;
        const double pe_lo = pe.skip_entry_bar_low ? pe.price : bar.low;
        const double fav_px = is_long ? pe_hi : pe_lo;
        const double adv_px = is_long ? pe_lo : pe_hi;
        const double favorable = is_long ? (fav_px - pe.price) * pe.qty
                                         : (pe.price - fav_px) * pe.qty;
        const double adverse = is_long ? (pe.price - adv_px) * pe.qty
                                       : (adv_px - pe.price) * pe.qty;
        const double closing = is_long ? (bar.close - pe.price) * pe.qty
                                       : (pe.price - bar.close) * pe.qty;
        pe.max_runup = std::max(0.0, std::max(favorable, closing));
        pe.max_drawdown = std::max(0.0, std::max(adverse, -closing));
    }
}

[[noreturn]] void reject_begin_bar(int index, const char* field, const char* detail) {
    throw std::invalid_argument(
        "bar[" + std::to_string(index) + "]." + field + (detail ? detail : ""));
}

// Validate the borrowed public begin array before the source provider stages
// syminfo, inputs, adapter state, or a native run spec.  This mirrors the
// legacy chart/stream shape checks and deliberately does not impose native
// calendar or slot-label policy; those remain the generic preflight's job.
void validate_source_begin_bars(const NativeBeginArgs& args) {
    if (args.n < 0) throw std::invalid_argument("bar count must be non-negative");
    if (args.n > 0 && args.bars == nullptr)
        throw std::invalid_argument("bars must be non-null for a nonempty array");
    for (int i = 0; i < args.n; ++i) {
        const Bar& bar = args.bars[i];
        if (!std::isfinite(bar.open)) reject_begin_bar(i, "open", " must be finite");
        if (!std::isfinite(bar.high)) reject_begin_bar(i, "high", " must be finite");
        if (!std::isfinite(bar.low)) reject_begin_bar(i, "low", " must be finite");
        if (!std::isfinite(bar.close)) reject_begin_bar(i, "close", " must be finite");
        if (args.is_stream) {
            if (bar.timestamp < 0)
                reject_begin_bar(i, "timestamp", " must be non-negative");
            if (bar.open < 0.0) reject_begin_bar(i, "open", " must be non-negative");
            if (bar.high < 0.0) reject_begin_bar(i, "high", " must be non-negative");
            if (bar.low < 0.0) reject_begin_bar(i, "low", " must be non-negative");
            if (bar.close < 0.0) reject_begin_bar(i, "close", " must be non-negative");
            if (!std::isfinite(bar.volume) || bar.volume < 0.0)
                reject_begin_bar(i, "volume", " must be non-negative finite");
        } else if (!std::isnan(bar.volume)
                   && (!std::isfinite(bar.volume) || bar.volume < 0.0)) {
            reject_begin_bar(i, "volume", " must be non-negative finite or NaN (unavailable)");
        }
        if (bar.low > std::min(bar.open, bar.close))
            reject_begin_bar(i, "low", " must not exceed open or close");
        if (bar.high < std::max(bar.open, bar.close))
            reject_begin_bar(i, "high", " must not be below open or close");
        if (i > 0) {
            const std::int64_t previous = args.bars[i - 1].timestamp;
            if (bar.timestamp <= previous)
                reject_begin_bar(i, "timestamp", " must be strictly increasing");
            if (previous < 0
                && bar.timestamp > std::numeric_limits<std::int64_t>::max() + previous) {
                reject_begin_bar(i, "timestamp", " delta exceeds int64 range");
            }
        }
    }
    if (args.is_stream && args.n > 0
        && (!std::isfinite(args.bars[args.n - 1].close)
            || args.bars[args.n - 1].close <= 0.0)) {
        throw std::invalid_argument("stream warmup final close must be finite and positive");
    }
}

}  // namespace

source::PineStrategyHost::PineStrategyHost(compat::pine::CapAttachment cap)
    : NativeStrategyHost(),
      adapter_(*this, cap),
      _src_series_active_(scheduler_.language()._src_series_active_),
      _src_open_(scheduler_.language()._src_open_),
      _src_high_(scheduler_.language()._src_high_),
      _src_low_(scheduler_.language()._src_low_),
      _src_close_(scheduler_.language()._src_close_),
      _src_volume_(scheduler_.language()._src_volume_),
      _src_hl2_(scheduler_.language()._src_hl2_),
      _src_hlc3_(scheduler_.language()._src_hlc3_),
      _src_ohlc4_(scheduler_.language()._src_ohlc4_),
      _src_hlcc4_(scheduler_.language()._src_hlcc4_),
      is_last_tick_(scheduler_.language().is_last_tick_) {
    // ab9714be LegacyCompatibilityConsumer::refuse was a no-op on this handle.
    host_mutation_guard_inert_ = true;
}

std::uint64_t source::PineStrategyHost::adapter_event_high_water(
        const NativeStrategyHost& base) noexcept {
    const auto& host = static_cast<const PineStrategyHost&>(base);
    return as_native_consumer(const_cast<IExecutionConsumer&>(host.execution_consumer()))
        .event_high_water();
}

std::uint64_t source::PineStrategyHost::adapter_terminal_receipt_high_water(
        const NativeStrategyHost& base) noexcept {
    const auto& host = static_cast<const PineStrategyHost&>(base);
    return as_native_consumer(const_cast<IExecutionConsumer&>(host.execution_consumer()))
        .terminal_receipt_high_water();
}

std::uint64_t source::PineStrategyHost::broker_state_hash_projection() const {
    // Fold current source/generic state with the last script-point
    // continuation. Recording only controls whether the per-bar array is
    // retained; the continuation snapshot keeps the scalar independent of
    // that switch and of NativeCompleted teardown.
    const std::uint64_t execution = last_script_continuation_valid_
        ? last_script_continuation_hash_
        : execution_consumer().continuation_hash();
    return broker_state_hash_from_execution_hash(execution);
}

double source::PineStrategyHost::margin_liquidation_price() const {
    return compute_liquidation_price();
}

double source::PineStrategyHost::compute_liquidation_price() const {
    if (position_side_ == PositionSide::FLAT) return na<double>();
    const double point_value = syminfo_.pointvalue;
    const double quantity = position_qty_;
    if (!(quantity > 0.0) || !(point_value > 0.0)) return na<double>();
    const double direction = position_side_ == PositionSide::LONG ? 1.0 : -1.0;
    const double margin_pct = position_side_ == PositionSide::LONG
        ? config_.margin_long : config_.margin_short;
    const double denominator = (margin_pct / 100.0) - direction;
    if (std::abs(denominator) < 1e-12) return na<double>();
    const double equity_basis =
        (initial_capital_ + net_profit_sum_) / active_account_currency_fx();
    double liquidation =
        (equity_basis / (quantity * point_value) - direction * position_entry_price_)
        / denominator;
    if (syminfo_mintick_ > 0.0) {
        liquidation = position_side_ == PositionSide::SHORT
            ? std::ceil(liquidation / syminfo_mintick_) * syminfo_mintick_
            : std::floor(liquidation / syminfo_mintick_) * syminfo_mintick_;
    }
    return liquidation;
}

PineStrategyConfig source::PineStrategyHost::apply_overrides(
        PineStrategyConfig config, const StrategyOverrides& overrides) {
    if (!std::isnan(overrides.initial_capital)) config.initial_capital = overrides.initial_capital;
    if (!std::isnan(overrides.commission_value)) config.commission_value = overrides.commission_value;
    if (!std::isnan(overrides.default_qty_value)) config.default_qty_value = overrides.default_qty_value;
    if (overrides.pyramiding >= 0) config.pyramiding = overrides.pyramiding;
    if (overrides.slippage >= 0) config.slippage = overrides.slippage;
    if (overrides.commission_type >= 0) config.commission_type = overrides.commission_type;
    if (overrides.default_qty_type >= 0) config.default_qty_type = overrides.default_qty_type;
    if (overrides.process_orders_on_close >= 0)
        config.process_orders_on_close = overrides.process_orders_on_close != 0;
    if (overrides.calc_on_order_fills >= 0)
        config.calc_on_order_fills = overrides.calc_on_order_fills != 0;
    if (overrides.close_entries_rule >= 0)
        config.close_entries_rule_any = overrides.close_entries_rule != 0;
    return config;
}

StagedConfiguration source::PineStrategyHost::staged_configuration() const {
    StagedConfiguration staged;
    staged.syminfo = syminfo_;
    staged.syminfo.mintick = syminfo_mintick_;
    staged.inputs = inputs_;
    staged.chart_timezone = chart_timezone_;
    staged.account_fx = account_currency_fx_;
    staged.account_fx_effective_from_ms = account_currency_fx_timestamps_;
    staged.account_fx_per_quote = account_currency_fx_rates_;
    if (std::isfinite(qty_step_) && qty_step_ > 0.0) staged.quantity_grid = qty_step_;
    return staged;
}

void source::PineStrategyHost::prepare_native_begin(const NativeBeginArgs& args) {
    // Idle abort requests are consumed by the public begin entry even when
    // input validation refuses before a run starts.  Crucially, validation
    // runs before any provider-owned state is changed.
    abort_requested_.store(false, std::memory_order_relaxed);
    validate_source_begin_bars(args);
    if (args.syminfo) {
        syminfo_ = *args.syminfo;
        syminfo_mintick_ = syminfo_.mintick;
        if (std::isfinite(syminfo_.qty_step) && syminfo_.qty_step > 0.0)
            qty_step_ = syminfo_.qty_step;
    }
    if (args.inputs) inputs_ = *args.inputs;

    if (args.is_stream && native_security_feed_enabled()) {
        throw std::runtime_error(
            "native request.security feed supports historical runs only");
    }

    if (!(args.n < 2 && !args.is_stream)) {
        std::string effective_input = args.input_tf;
        if (effective_input.empty() && args.n >= 2 && args.bars != nullptr)
            effective_input = detect_timeframe(args.bars, args.n);
        const std::string effective_script = args.script_tf.empty()
            ? effective_input : args.script_tf;
        try {
            if (!effective_input.empty() && !effective_script.empty()
                && tf_ratio(effective_input, effective_script) == -2) {
                throw std::runtime_error(
                    "script timeframe must be coarser than or equal to input timeframe: requested script_tf "
                    + effective_script + " from input timeframe " + effective_input);
            }
        } catch (const std::runtime_error&) {
            throw;
        } catch (...) {
            // The native specification validator owns malformed literals.
        }
    }

    if (args.is_stream && config_.calc_on_order_fills) {
        throw std::runtime_error(
            "native stream requires close-only calculation; calc_on_order_fills is unsupported");
    }
    if (args.is_stream && (realtime_tail_ || probe_suppress_tail_logic_)) {
        throw std::runtime_error("native stream cannot use historical probe/tail overrides");
    }
    PineStrategyConfig effective = config_;
    if (args.overrides_opaque) {
        const auto* overrides = static_cast<const StrategyOverrides*>(args.overrides_opaque);
        effective = apply_overrides(effective, *overrides);
    }
    const StagedConfiguration staged = staged_configuration();
    if (!staged.account_fx_effective_from_ms.empty() && effective.calc_on_order_fills)
        throw std::logic_error(
            "timestamped account-currency FX does not support calc_on_order_fills");
    if (!staged.account_fx_effective_from_ms.empty() && args.bar_magnifier)
        throw std::logic_error(
            "timestamped account-currency FX is not supported with bar magnifier");

    adapter_.reset_for_run();
    adapter_.set_receipt_high_water_readers(&PineStrategyHost::adapter_event_high_water,
                                            &PineStrategyHost::adapter_terminal_receipt_high_water);
    if (args.n > 0 && static_cast<std::size_t>(args.n)
        <= std::numeric_limits<std::size_t>::max() / 4U) {
        as_native_consumer(execution_consumer()).reserve_driver_log(
            static_cast<std::size_t>(args.n) * 4U);
    }
    // Source placement evidence is retained by request incarnation so a
    // re-issued bracket can preserve its exact historical projection.  Batch
    // callers already disclose their bar count here; reserve the ordinary
    // two-leg-per-bar capacity once instead of repeatedly rehashing that
    // durable table during a long replay.
    if (args.n > 0 && static_cast<std::size_t>(args.n)
        <= adapter_.placement_.max_size() / 2U) {
        adapter_.placement_.reserve(static_cast<std::size_t>(args.n) * 2U);
    }
    adapter_.set_configuration(effective);
    adapter_.set_staged_configuration(staged);
    adapter_.set_begin_mode(args.is_stream, args.bar_magnifier);
    adapter_.set_margin_call_enabled(margin_call_enabled_);
    scheduler_.capture_begin(args);
    scheduler_.set_source_series_active(effective.src_series_active);
    const NativePathOrder path_order = path_order_mode_ == 1
        ? NativePathOrder::HighFirst
        : (path_order_mode_ == 2 ? NativePathOrder::LowFirst
                                 : NativePathOrder::Auto);
    adapter_.set_path_order(path_order);
    const NativeRunSpec spec = adapter_.project(effective, staged, args, path_order);
    const auto setup = configure_native(spec);
    if (setup.status != NativeSetupStatus::Applied)
        throw std::logic_error("Pine native adapter failed to configure projected run spec");
    config_ = effective;
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::on_native_run_begin() {
    source_bar_index_ = -1;
    source_last_bar_index_ = -1;
    source_callback_count_ = 0;
    source_prepare_failed_ = false;
    try {
        scheduler_.run_begin(*this);
    } catch (const std::exception& error) {
        source_prepare_failed_ = true;
        last_error_ = error.what();
    } catch (...) {
        source_prepare_failed_ = true;
        last_error_ = "unknown error during Pine script preparation";
    }
}

void source::PineStrategyHost::capture_script_continuation_hash() {
    last_script_continuation_hash_ = execution_consumer().continuation_hash();
    last_script_continuation_valid_ = true;
    if (broker_state_hash_recording_ && !broker_state_hashes_.empty()) {
        broker_state_hashes_.back() = broker_state_hash();
    }
}

void source::PineStrategyHost::on_native_input(
        const Bar& bar, const NativeInputContext& context) {
    if (source_prepare_failed_) return;
    if (native_state().phase == NativeRunPhase::Realtime)
        stream_warmup_mode_ = false;
    scheduler_.input(bar, context, *this);
    // Aggregation can deliver leftover input after the last script callback.
    // Refresh the last recorded row (and the continuation snapshot) so the
    // scalar stays the same fold with or without recording.
    if (scheduler_.terminal_source_bar()) capture_script_continuation_hash();
}

void source::PineStrategyHost::on_native_tick(
        const Bar& tick, const NativeTickContext& context) {
    if (source_prepare_failed_) return;
    if (native_state().phase == NativeRunPhase::Realtime)
        stream_warmup_mode_ = false;
    {
        // ab9714be pine_stream.cpp:298/:450 samples the excursion at every
        // realtime print (a price point: H == L == C == print).
        const int sample_index = scheduler_.bar_magnifier_enabled()
            ? scheduler_.source_bar_index_for(context.decision)
            : context.decision.coordinate.interval_index;
        sample_open_trade_extremes(
            pyramid_entries_, position_side_, sample_index, tick);
    }
    scheduler_.tick(tick, context, *this);
    adapter_.on_tick(tick, context);
}

void source::PineStrategyHost::on_native_bar_open(
        const Bar& bar, const NativeDecisionContext& context) {
    if (source_prepare_failed_) return;
    bar_magnifier_enabled_ = scheduler_.bar_magnifier_enabled();
    diag_magnifier_sub_bars_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sub_bars_processed) : 0;
    diag_magnifier_sample_ticks_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sample_ticks_processed) : 0;
    adapter_.on_bar_open(bar, context);
    scheduler_.bar_open(bar, context, *this);
}

void source::PineStrategyHost::on_native_bar(
        const Bar& bar, const NativeDecisionContext& context) {
    if (source_prepare_failed_) return;
    bar_magnifier_enabled_ = scheduler_.bar_magnifier_enabled();
    diag_magnifier_sub_bars_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sub_bars_processed) : 0;
    diag_magnifier_sample_ticks_processed_ = bar_magnifier_enabled_
        ? static_cast<std::int64_t>(context.driver_statistics.sample_ticks_processed) : 0;
    adapter_.observe_terminal_receipts();
    {
        const int sample_index = scheduler_.bar_magnifier_enabled()
            ? scheduler_.source_bar_index_for(context)
            : context.coordinate.interval_index;
        sample_open_trade_extremes(
            pyramid_entries_, position_side_, sample_index, bar);
        replace_masked_entry_bar_extremes(
            pyramid_entries_, position_side_, sample_index, bar);
    }
    scheduler_.bar(bar, context, *this);
    adapter_.on_bar_close(bar, context);
    if (adapter_.config_.slippage > 0) {
        for (auto& lot : pyramid_entries_) {
            if (lot.entry_bar_index == context.coordinate.interval_index && lot.qty > 0.0) {
                const auto found = adapter_.placement_.find(lot.entry_incarnation);
                if (found != adapter_.placement_.end()) {
                    const auto& snap = found->second;
                    const bool pure_stop_entry = snap.family == PineOrderFamily::Entry
                        && std::isfinite(snap.exit_levels.stop) && snap.exit_levels.stop > 0.0
                        && !std::isfinite(snap.exit_levels.limit);
                    if (pure_stop_entry) {
                        // ab9714be pine_risk.cpp:276-282: update_per_trade_extremes measures adverse excursion against opposite extreme
                        if (lot.price > bar.high && std::isfinite(bar.low) && bar.low > 0.0) {
                            lot.max_drawdown = std::max(lot.max_drawdown, (lot.price - bar.low) * lot.qty);
                        } else if (lot.price < bar.low && std::isfinite(bar.high) && bar.high > 0.0) {
                            lot.max_drawdown = std::max(lot.max_drawdown, (bar.high - lot.price) * lot.qty);
                        }
                    }
                }
            }
        }
    }
    if (context.is_terminal_sub_bar
        && context.coordinate.interval_index == source_last_bar_index_) {
        scheduler_record_range_end(bar);
    }
    const bool recording = broker_state_hash_recording_ && !broker_state_hashes_.empty();
    const bool last_batch = context.is_terminal_sub_bar
        && context.coordinate.interval_index == source_last_bar_index_;
    const bool stream_script = context.is_terminal_sub_bar
        && stream_phase_ == StreamPhase::REALTIME;
    if (recording || last_batch || stream_script) {
        // ab9714be pine_scheduler.cpp:1753/:1875 records after dispatch_bar,
        // including the terminal source policy updates.  The native hook
        // returns through adapter_.on_bar_close after the scheduler callback,
        // so refresh the continuation snapshot (and the just-appended row)
        // at that boundary.
        capture_script_continuation_hash();
    }
}

void source::PineStrategyHost::on_native_applied(
        const native_order::ExecutionAppliedEvent& event,
        const NativeDecisionContext& context) {
    if (source_prepare_failed_) return;
    if (scheduler_.bar_magnifier_enabled()) {
        const int source_index = scheduler_.source_bar_index_for(context);
        for (auto& lot : pyramid_entries_) {
            if (lot.entry_incarnation == event.handle().incarnation
                && event.opened_units != 0.0) {
                lot.entry_bar_index = source_index;
            }
        }
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const std::size_t index = event.first_trade_index + i;
            if (index >= trades_.size()) continue;
            trades_[index].exit_bar_index = source_index;
        }
    }
    const auto p = adapter_.placement_.find(event.handle().incarnation);
    // ab9714be pine_fills.cpp:42: a priced (stop/limit) entry masks the
    // assumed-OHLC extreme the path reaches BEFORE the fill.
    const bool pine_priced = p != adapter_.placement_.end()
        && ((std::isfinite(p->second.exit_levels.stop) && p->second.exit_levels.stop > 0.0)
            || (std::isfinite(p->second.exit_levels.limit) && p->second.exit_levels.limit > 0.0));
    if (event.opened_units != 0.0) {
        if (pine_priced) {
            const Bar& mask_bar = current_bar_;
            for (auto& lot : pyramid_entries_) {
                if (lot.entry_incarnation != event.handle().incarnation) continue;
                set_entry_fill_excursion_masks(lot, mask_bar, lot.price);
            }
        } else if (event.cursor.point.path_phase == NativePathPhase::Close) {
            for (auto& lot : pyramid_entries_) {
                if (lot.entry_incarnation != event.handle().incarnation) continue;
                lot.skip_entry_bar_high = true;
                lot.skip_entry_bar_low = true;
            }
        }
        // ab9714be pine_orders.cpp:750-753 and pine_fills.cpp:7189-7193
        // (KI-62): a MARKET entry that adds to a live same-side position is
        // flagged so a same-bar from_entry bracket exit covers it.
        const bool market_add = !pine_priced && event.closed_units == 0.0
            && p != adapter_.placement_.end()
            && (p->second.family == PineOrderFamily::Entry
                || p->second.family == PineOrderFamily::Order)
            && std::any_of(pyramid_entries_.begin(), pyramid_entries_.end(),
                [&](const PyramidEntry& lot) {
                    return lot.entry_incarnation != event.handle().incarnation;
                });
        if (market_add) {
            for (auto& lot : pyramid_entries_) {
                if (lot.entry_incarnation == event.handle().incarnation)
                    lot.market_pyramid_add = true;
            }
        }
    }
    // The legacy source observer counted one broker fill for every committed
    // execution event.  The native consumer owns those events now; mirror the
    // count at its notification boundary so restored source tests and public
    // source-side policy reads see the same monotone value.
    // ab9714be pine_fills.cpp:5954/:6300 and the margin/FX sites: one source
    // broker fill sequence is consumed per applied broker instruction, not
    // per closed trade row. Native ordinals remain the execution authority;
    // this is the generated/source-visible diagnostic projection.
    if (broker_fill_event_seq_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("source broker fill sequence exhausted");
    ++broker_fill_event_seq_;
    excursion_priced_fill_ = false;
    excursion_level_fill_ = false;
    excursion_margin_call_ = false;
    excursion_margin_prefix_ = false;
    excursion_margin_fill_only_ = false;
    excursion_trail_offset_ticks_ = std::numeric_limits<double>::quiet_NaN();
    excursion_trail_raw_price_ = std::numeric_limits<double>::quiet_NaN();
    if (position_side_ != PositionSide::FLAT) {
        // ab9714be engine_orders.cpp:531-541: settle_position_after_partial_exit resets to flat when position_qty_ <= kQtyEpsilon or empty
        bool changed = false;
        for (auto it = pyramid_entries_.begin(); it != pyramid_entries_.end(); ) {
            if (it->qty <= internal::kQtyEpsilon) {
                it = pyramid_entries_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (pyramid_entries_.empty() || position_qty_ <= internal::kQtyEpsilon) {
            reset_position_state_to_flat();
        } else if (changed) {
            double total_qty = 0.0;
            double weighted_price = 0.0;
            for (const auto& pe : pyramid_entries_) {
                total_qty += pe.qty;
                weighted_price += pe.price * pe.qty;
            }
            position_qty_ = total_qty;
            position_entry_price_ = weighted_price / total_qty;
            position_entry_count_ = static_cast<int>(pyramid_entries_.size());
        }
    }
    adapter_.on_applied(event, context);
    precommit_held_units_ = std::numeric_limits<double>::quiet_NaN();
    if (adapter_.take_intraday_loss_relabel(event.ordinal)) {
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const std::size_t index = event.first_trade_index + i;
            if (index >= trades_.size()) continue;
            trades_[index].exit_id.clear();
            trades_[index].exit_comment = "Close Position (Max intraday Loss)";
        }
    }
    project_short_seed_report_rows(event);
    scheduler_.applied(event);
    // R6: with calc_on_order_fills the kernel drives this event's
    // recalculation next, in the same drain iteration and at the same cursor
    // (NativeCalculationTrigger::BarCloseAndFills). The range-end row stays
    // after it, exactly where the adapter's own cascade used to put it.
    if (!scheduler_.coof_recalculation_due(event, context, *this))
        record_applied_range_end();
}

void source::PineStrategyHost::record_applied_range_end() {
    if (scheduler_.terminal_source_bar() || barstate_islast_) {
        const Bar terminal = scheduler_.current_script_bar()
            ? *scheduler_.current_script_bar() : current_bar_;
        scheduler_record_range_end(terminal);
    }
}

void source::PineStrategyHost::on_native_recalculate(
        const Bar& bar, const NativeDecisionContext& context,
        NativeCalculationReason reason,
        const native_order::ExecutionAppliedEvent* cause) {
    if (reason != NativeCalculationReason::OrderFill) {
        // BarClose is the script bar's own calculation; Tick and SubBar are
        // cadences the Pine spec never selects.
        on_native_bar(bar, context);
        return;
    }
    if (source_prepare_failed_ || cause == nullptr) return;
    // A fill Pine does not recalculate on (POOC's terminal close fill, a
    // grouped-stop sibling, a replayed ordinal) still spends a kernel
    // recalculation slot; it publishes nothing and books nothing.
    if (!scheduler_.coof_recalculation_due(*cause, context, *this)) return;
    scheduler_.recalculate(*cause, context, *this);
    record_applied_range_end();
}

native_order::ExecutionTerms source::PineStrategyHost::resolve_execution_terms(
        const NativeExecutionTermsFacts& facts) const {
    return adapter_.resolve_terms(facts);
}

bool source::PineStrategyHost::margin_check_allowed(
        const NativeMarginCheckPoint& point) const {
    return adapter_.margin_check_allowed(point);
}

std::optional<NativeMarginDecision> source::PineStrategyHost::resolve_margin_requirement(
        const NativeMarginRequirementView& view) const {
    return adapter_.resolve_margin_requirement(view);
}

std::optional<double> source::PineStrategyHost::resolve_margin_call_units(
        const NativeMarginCallView& view) const {
    return adapter_.resolve_margin_call_units(view);
}

NativePrecommitVerdict source::PineStrategyHost::validate_execution_precommit(
        const NativePrecommitView& view) const {
    // ab9714be pine_fills.cpp:5741: the exit-bar path prefix belongs to the
    // closing lot's excursion only while a priced fill is being applied. The
    // host's own sampler consumes this cache in closed_lot_excursion().
    bool priced = false;
    if (view.definition) {
        const auto& trigger = view.definition->request.trigger;
        priced = priced_opening_trigger(trigger)
            || std::holds_alternative<native_order::Trail>(trigger);
    }
    // L10j: an exit leg carrying priced stop/limit/trailing terms folds its
    // pre-fill path extremes too; the magnifier one-price gate below applies
    // to it as well (L10h), which the former adapter-side override bypassed.
    priced = priced || adapter_.source_priced_exit(view.target.incarnation);
    // When an opposite market entry or script close is pending at the bar open,
    // the legacy owner models the exit as the market order (closing the trade
    // at the open without folding the exit bar's path).
    if (view.cursor.point.path_phase == NativePathPhase::Open
        && adapter_.has_pending_market_exit(view.cursor.point.interval_index)) {
        priced = false;
    }
    double trail_ticks = std::numeric_limits<double>::quiet_NaN();
    if (const auto off = adapter_.source_trail_offset_ticks(view.target.incarnation)) {
        trail_ticks = *off;
    }
    excursion_priced_fill_ = priced;
    excursion_level_fill_ = adapter_.source_post_parent_calc_level_fill(
        view.target.incarnation);
    excursion_margin_call_ = adapter_.source_margin_exit(view.target.incarnation)
        || source::PineExecutionAdapter::source_kernel_liquidation(view.definition);
    excursion_trail_offset_ticks_ = trail_ticks;
    // ab9714be pine_fills.cpp:5766-5770: the peak a TRAIL fill retraces from is
    // taken off the matcher's pre-slip price, which this view still carries; the
    // booked facts.fill_price the sampler reads has already been slipped by
    // resolve_terms.
    excursion_trail_raw_price_ =
        (!std::isnan(trail_ticks) && std::isfinite(view.raw_price)
         && view.raw_price > 0.0)
            ? view.raw_price
            : std::numeric_limits<double>::quiet_NaN();
    // A zero-offset trail's matcher touch is the half-tick boundary of the
    // rounded price path, not its fill; the owner's peak is the pre-slip fill
    // itself, snapped onto the tick grid (ab9714be pine_fills.cpp:5761-5771,
    // engine_internal.hpp:187-198).  Its open-gap fill is no TRAIL event
    // (engine_path_resolve.cpp:620-631) and folds no peak.
    if (trail_ticks == 0.0 && view.cursor.point.path_phase == NativePathPhase::Open) {
        excursion_trail_offset_ticks_ = std::numeric_limits<double>::quiet_NaN();
        excursion_trail_raw_price_ = std::numeric_limits<double>::quiet_NaN();
    } else if (trail_ticks == 0.0 && std::isfinite(view.resolved_price)
               && view.resolved_price > 0.0) {
        const double slip = config_.slippage * syminfo_.mintick;
        excursion_trail_raw_price_ = internal::snap_trail_level_to_tick_grid(
            physical_position().signed_units > 0.0 ? view.resolved_price + slip
                                                   : view.resolved_price - slip,
            syminfo_.mintick);
    }
    // A fill-through (slipped touch) trail leg books its fill slippage ticks
    // past the touch; the owner's peak is still that leg's PRE-slip fill
    // (ab9714be pine_fills.cpp:5760-5769 runs before apply_fill_slippage),
    // which is the booked price with the slippage step taken back.  An
    // open-gap fill is not a TRAIL event at all (ab9714be
    // engine_path_resolve.cpp:620-631 leaves is_trail false), so it folds
    // no peak.
    if (!std::isnan(trail_ticks) && view.definition && std::isfinite(view.resolved_price)) {
        const auto* touch = std::get_if<native_order::Limit>(&view.definition->request.trigger);
        if (touch && touch->fill_through) {
            if (view.cursor.point.path_phase == NativePathPhase::Open) {
                excursion_trail_offset_ticks_ = std::numeric_limits<double>::quiet_NaN();
                excursion_trail_raw_price_ = std::numeric_limits<double>::quiet_NaN();
            } else {
                const double slip = config_.slippage * syminfo_.mintick;
                excursion_trail_raw_price_ = physical_position().signed_units > 0.0
                    ? view.resolved_price + slip : view.resolved_price - slip;
            }
        }
    }
    // The margin slice's sampling chronology is a book fact resolved in the
    // adapter precommit pass; start clean for every request.
    excursion_margin_prefix_ = false;
    excursion_margin_fill_only_ = false;
    precommit_held_units_ = std::abs(physical_position().signed_units);
    return adapter_.validate_precommit(view);
}

ClosedLotExcursion source::PineStrategyHost::closed_lot_excursion(
        const ClosedLotExcursionFacts& facts) const {
    // ab9714be engine_orders.cpp:319 build_close_trade_with_costs, excursion
    // half. Everything here is the owner's model: the carried per-lot extremes
    // already hold every completed source bar's masked H/L/C walk
    // (sample_open_trade_extremes), scaled to the closed slice; the exit fill
    // itself always belongs to the trade; a TRAIL fill retrace contributes the
    // peak that armed it; and for a priced exit the assumed OHLC path prefix
    // the fill sits behind is folded in, honoring the entry-bar masks when the
    // lot opened on this same bar.
    const double slice =
        (facts.lot_qty > 0.0) ? (facts.closed_qty / facts.lot_qty) : 1.0;
    double fill_fav =
        (facts.is_long ? (facts.fill_price - facts.entry_price)
                       : (facts.entry_price - facts.fill_price))
        * facts.closed_qty;
    // Open-gap scratches book the script open on both legs; a 1-ULP
    // entry/exit residual formats as CSV -0.000000 against owner's 0.
    if (std::abs(facts.fill_price - facts.entry_price) < 1e-9) {
        fill_fav = 0.0;
    }
    ClosedLotExcursion owned;
    // ab9714be src/source/pine_scheduler.cpp:242,257 samples the bar's H/L/C
    // into every OPEN trade (update_per_trade_extremes, step 2) only after the
    // resting priced exits of step 1 have closed. A leg this route force-fills
    // at its level on its own entry bar therefore keeps the owner's unsampled
    // entry seed: no bar of this trade was ever walked by the sampler, so the
    // exit fill and the pre-fill path prefix below are the whole excursion.
    const bool unsampled_entry_bar = facts.entry_bar_index == facts.exit_bar_index
        && excursion_level_fill_;
    const double carried_favorable = unsampled_entry_bar ? 0.0 : facts.carried_favorable;
    const double carried_adverse = unsampled_entry_bar ? 0.0 : facts.carried_adverse;
    owned.favorable = std::max(carried_favorable * slice, fill_fav);
    owned.adverse = std::max(carried_adverse * slice, -fill_fav);
    // ab9714be pine_fills.cpp:5766-5770: a TRAIL fill retraces exactly the
    // trailing offset from the peak that armed it, so that peak is a pre-fill
    // favorable excursion no bar-boundary sample ever sees.
    if (!std::isnan(excursion_trail_offset_ticks_)) {
        const double off = excursion_trail_offset_ticks_ * syminfo_.mintick;
        const double basis = std::isnan(excursion_trail_raw_price_)
                                 ? facts.fill_price
                                 : excursion_trail_raw_price_;
        const double peak = facts.is_long ? (basis + off) : (basis - off);
        const double peak_fav = (facts.is_long ? (peak - facts.entry_price)
                                               : (facts.entry_price - peak))
                                * facts.closed_qty;
        owned.favorable = std::max(owned.favorable, peak_fav);
    }
    // A range-end report row is a projection at the terminal close: the owner
    // folds no exit-bar path prefix there. A market fill lands on a bar
    // boundary the sampler already walked, and under the bar magnifier a
    // one-price open bar has no path left to fold.
    if (excursion_range_end_projection_) return owned;
    if (excursion_margin_call_ && excursion_margin_fill_only_) return owned;
    if (excursion_margin_call_) {
        // ab9714be pine_risk.cpp:256-292: a margin-call liquidation at the
        // adverse extreme owns the rest of the bar. Which part of the bar it
        // owns is the slice's birth chronology: the non-POOC opening trim
        // inherits the complete bar, while the POOC/pre-exit prefix routes
        // sample only the traversed waypoint prefix.
        const Bar sample_bar = margin_call_sample_bar(
            current_bar_, facts.fill_price, excursion_margin_prefix_,
            internal::bar_path_uses_high_first(current_bar_),
            syminfo_.mintick, config_.slippage);
        const bool margin_same_bar = facts.entry_bar_index == facts.exit_bar_index;
        const double margin_high = (margin_same_bar && facts.entry_bar_high_masked)
                                       ? facts.entry_price : sample_bar.high;
        const double margin_low = (margin_same_bar && facts.entry_bar_low_masked)
                                      ? facts.entry_price : sample_bar.low;
        const double fav_px = facts.is_long ? margin_high : margin_low;
        const double adv_px = facts.is_long ? margin_low : margin_high;
        const double fav = (facts.is_long ? (fav_px - facts.entry_price)
                                          : (facts.entry_price - fav_px))
                           * facts.closed_qty;
        const double adv = (facts.is_long ? (facts.entry_price - adv_px)
                                          : (adv_px - facts.entry_price))
                           * facts.closed_qty;
        owned.favorable = std::max(owned.favorable, fav);
        owned.adverse = std::max(owned.adverse, adv);
        const double closing = (facts.is_long ? (sample_bar.close - facts.entry_price)
                                              : (facts.entry_price - sample_bar.close))
                               * facts.closed_qty;
        owned.favorable = std::max(owned.favorable, closing);
        owned.adverse = std::max(owned.adverse, -closing);
        return owned;
    }
    if (!excursion_priced_fill_) return owned;
    // ab9714be pine_scheduler.cpp:99-106 and 548-552: the calc_on_order_fills
    // historical dispatch books an O-point fill against a one-price point
    // bar, so no path extreme precedes it.
    if ((scheduler_.bar_magnifier_enabled() || config_.calc_on_order_fills)
        && std::abs(facts.fill_price - current_bar_.open) < 1e-7) {
        return owned;
    }
    const double touch_price = bar_fill_price(facts.fill_price);
    double fill_pos = 0.0;
    if (!internal::first_touch_position(current_bar_, touch_price, &fill_pos))
        return owned;
    const bool high_first = internal::bar_path_uses_high_first(current_bar_);
    const double high_pos = high_first ? 1.0 : 2.0;
    const double low_pos = high_first ? 2.0 : 1.0;
    const bool same_bar = facts.entry_bar_index == facts.exit_bar_index;
    const bool mask_high = same_bar && facts.entry_bar_high_masked;
    const bool mask_low = same_bar && facts.entry_bar_low_masked;
    if (high_pos < fill_pos && !mask_high) {
        const double hi_fav = (facts.is_long
                                   ? (current_bar_.high - facts.entry_price)
                                   : (facts.entry_price - current_bar_.high))
                              * facts.closed_qty;
        owned.favorable = std::max(owned.favorable, hi_fav);
        owned.adverse = std::max(owned.adverse, -hi_fav);
    }
    if (low_pos < fill_pos && !mask_low) {
        const double lo_fav = (facts.is_long
                                   ? (current_bar_.low - facts.entry_price)
                                   : (facts.entry_price - current_bar_.low))
                              * facts.closed_qty;
        owned.favorable = std::max(owned.favorable, lo_fav);
        owned.adverse = std::max(owned.adverse, -lo_fav);
    }
    return owned;
}

void source::PineStrategyHost::configure_pine_strategy(const PineStrategyConfig& config) {
    guard_native_mutation("configure_pine_strategy");
    config_ = config;
    adapter_.set_configuration(config_);
    scheduler_.set_source_series_active(config_.src_series_active);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_strategy_override(const StrategyOverrides& overrides) {
    guard_native_mutation("set_strategy_override");
    override_ = overrides;
    config_ = apply_overrides(config_, override_);
    adapter_.set_configuration(config_);
    scheduler_.set_source_series_active(config_.src_series_active);
    source_configuration_captured_ = true;
}

void source::PineStrategyHost::set_syminfo_session(const std::string& session) {
    if (stream_warmup_mode_) {
        (void)session;
        return;
    }
    BacktestEngine::set_syminfo_session(session);
}

void source::PineStrategyHost::set_pine_risk_direction(int direction) {
    adapter_.set_risk_direction(direction);
}

void source::PineStrategyHost::set_pine_risk_max_cons_loss_days(int value) {
    adapter_.set_risk_max_cons_loss_days(value);
}

void source::PineStrategyHost::set_pine_risk_max_drawdown(double value, bool percent) {
    adapter_.set_risk_max_drawdown(value, percent);
}

void source::PineStrategyHost::set_pine_risk_max_intraday_loss(double value, bool percent) {
    adapter_.set_risk_max_intraday_loss(value, percent);
}

void source::PineStrategyHost::set_pine_risk_max_intraday_filled_orders(int limit) {
    adapter_.cap = limit;
}

void source::PineStrategyHost::set_pine_risk_max_position_size(double value) {
    adapter_.set_risk_max_position_size(value);
}

int source::PineStrategyHost::pine_bar_index() const {
    return source_bar_index_ + scheduler_.bar_index_offset();
}

int source::PineStrategyHost::pine_last_bar_index() const {
    return source_last_bar_index_ + scheduler_.bar_index_offset();
}

bool source::PineStrategyHost::is_first_tick() const noexcept {
    return scheduler_.is_first_tick();
}

bool source::PineStrategyHost::is_last_tick() const noexcept {
    return scheduler_.is_last_tick();
}

compat::pine::CapClock source::PineStrategyHost::fixture_cap_clock() const {
    NativeDecisionContext context;
    if (const auto point = current_execution_point()) {
        context = point->decision;
    } else {
        context.coordinate.interval_index = bar_index_;
        context.sub_bar_open_ms = current_bar_.timestamp;
        context.script_bar_open_ms = current_bar_.timestamp;
    }
    const BarTime time = fixture_chart_time(context.sub_bar_open_ms);
    return {context.sub_bar_open_ms,
            syminfo_.session.empty() ? "24x7" : syminfo_.session,
            syminfo_.timezone.empty() ? "UTC" : syminfo_.timezone,
            time.dayofmonth, time.month};
}

compat::pine::Calculation source::PineStrategyHost::fixture_cap_calculation() const {
    NativeDecisionContext context;
    if (const auto point = current_execution_point()) {
        context = point->decision;
    } else {
        context.coordinate.interval_index = bar_index_;
        context.sub_bar_open_ms = current_bar_.timestamp;
        context.script_bar_open_ms = current_bar_.timestamp;
    }
    return adapter_.cap_calculation(context);
}

bool source::PineStrategyHost::fixture_intraday_cap_latched() {
    return adapter_.cap.placement(fixture_cap_clock())
        == compat::pine::Placement::Deny;
}

source::PineStrategyHost::BarTime source::PineStrategyHost::fixture_chart_time(
        std::int64_t timestamp_ms) const {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm{};
    const auto utc = [&]() {
        return ::gmtime_r(&seconds, &tm) != nullptr;
    };
    if (chart_timezone_.empty() || chart_timezone_ == "UTC"
        || chart_timezone_ == "Etc/UTC") {
        (void)utc();
    } else {
        try {
            pine_tz::ScopedTimezone guard(chart_timezone_);
            if (::localtime_r(&seconds, &tm) == nullptr) (void)utc();
        } catch (...) {
            (void)utc();
        }
    }
    BarTime result;
    result.year = tm.tm_year + 1900;
    result.month = tm.tm_mon + 1;
    result.dayofmonth = tm.tm_mday;
    result.hour = tm.tm_hour;
    result.minute = tm.tm_min;
    result.second = tm.tm_sec;
    result.dayofweek = tm.tm_wday + 1;
    result.weekofyear = (tm.tm_yday + 7 - ((tm.tm_wday + 6) % 7)) / 7;
    return result;
}

std::uint64_t source::PineStrategyHost::fixture_applied_receipt_count() const {
    std::uint64_t count = 0;
    for (const auto& event : native_events(0)) {
        if (!event.command
            || !std::holds_alternative<native_order::ExecutionAppliedEvent>(*event.command)) {
            continue;
        }
        ++count;
    }
    return count;
}

bool source::PineStrategyHost::history_advances_new_bar() const noexcept {
    return scheduler_.history_advances_new_bar();
}

bool source::PineStrategyHost::security_series_slot_is_new(int slot) const noexcept {
    return BacktestEngine::security_series_slot_is_new(slot);
}

double source::PineStrategyHost::prev_chart_close() const {
    return scheduler_.previous_chart_close();
}

int source::PineStrategyHost::last_bar_dual_entry_path() const {
    return adapter_.pending_intent_view().last_bar_dual_entry_path();
}

double source::PineStrategyHost::signed_position_size() const {
    return scheduler_.script_position_view(bar_index_, position_side_, position_qty_);
}

void source::PineStrategyHost::freeze_script_position_view() {
    scheduler_.freeze_script_position_view(
        bar_index_, position_side_, position_qty_, pyramid_entries_);
}

void source::PineStrategyHost::clear_script_position_view() {
    scheduler_.clear_script_position_view();
}

const Series<double>& source::PineStrategyHost::source_series(const std::string& key) const {
    return scheduler_.source_series(key);
}

const Series<double>& source::PineStrategyHost::source_input_series(
        const std::string& key, const Series<double>& fallback) const {
    const auto found = inputs_.find(key);
    if (found == inputs_.end() || found->second.empty()) return fallback;
    try {
        return scheduler_.source_series(found->second);
    } catch (const std::invalid_argument&) {
        return fallback;
    }
}

double source::PineStrategyHost::live_position_size() const {
    return physical_position().signed_units;
}

int source::PineStrategyHost::pending_order_count() const {
    return pending_intent_view().size();
}

MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() {
    return adapter_.admission_journal;
}

const MarketAdmissionJournal& source::PineStrategyHost::market_admission_journal() const {
    return adapter_.admission_journal;
}

std::vector<admission::Field> source::PineStrategyHost::market_admission_fields() const {
    std::vector<admission::Field> fields;
    adapter_.admission_journal.reflect("journal", [&](const admission::Field& field) {
        fields.push_back(field);
    });
    return fields;
}

int source::PineStrategyHost::probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only, int* partition) const {
    return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only, partition);
}

int source::PineStrategyHost::pending_order_level_resolved(int index) const {
    return pending_intent_view().level_resolved(index);
}

int source::PineStrategyHost::pending_order_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
}

const PendingIntentView& source::PineStrategyHost::pending_intent_view() const noexcept {
    return adapter_.pending_intent_view();
}

int source::PineStrategyHost::short_seed_collision_role_v1(
        native_order::RequestHandle handle) const noexcept {
    return adapter_.short_seed_collision_role_v1(std::move(handle));
}

void source::PineStrategyHost::enable_pine_intraday_cap() {
    adapter_.enable_intraday_cap();
}

void source::PineStrategyHost::attach_pine_execution_adapter() {
    adapter_.attach_execution_adapter();
    // Generated constructors attach the source execution bridge before their
    // risk statements and metadata arrive.  The intraday-cap configuration is
    // part of that same source-policy attachment; leaving it detached makes a
    // later max_intraday_filled_orders declaration silently inert.
    adapter_.enable_intraday_cap();
}

void source::PineStrategyHost::set_syminfo_metadata(
        const std::string& key, double value) {
    BacktestEngine::set_syminfo_metadata(key, value);
    if (key == "bar_index_offset") {
        scheduler_.set_bar_index_offset(std::isfinite(value)
            ? static_cast<int>(std::llround(value)) : 0);
    }
    if (key == "security_range_start_na_warmup") {
        if (std::isfinite(value) && value > 0.0) {
            security_range_start_na_warmup_ = true;
            security_range_start_ms_ = static_cast<int64_t>(std::llround(value));
        } else {
            security_range_start_na_warmup_ = false;
            security_range_start_ms_ = 0;
        }
    }
    if (key == "chart_ema_na_warmup")
        chart_ema_na_warmup_ = std::isfinite(value) && value > 0.0;
    if (key == "historical_security_lookahead_projection")
        historical_security_lookahead_projection_ = std::isfinite(value) && value > 0.0;
    if (key == "margin_long" && config_.margin_long == 100.0)
        config_.margin_long = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    if (key == "margin_short" && config_.margin_short == 100.0)
        config_.margin_short = (std::isfinite(value) && value > 0.0) ? value : 100.0;
    adapter_.set_configuration(config_);
    adapter_.priority.metadata(key, value);
    adapter_.cap.metadata(key, value);
}

int source::PineStrategyHost::observe_last_bar_dual_entry_path_v1() const {
    return pending_intent_view().last_bar_dual_entry_path();
}

int source::PineStrategyHost::observe_pending_count_v1() const {
    return pending_intent_view().size();
}

int source::PineStrategyHost::observe_pending_copy_v1(
        int index, pf_pending_order_v1_t* out) const {
    return pending_intent_view().copy_v1(index, out);
}

int source::PineStrategyHost::observe_probe_fill_qty(
        int index, double fill_price, double* qty, int* close_only, int* partition) const {
    return pending_intent_view().probe_fill_qty(index, fill_price, qty, close_only, partition);
}

int source::PineStrategyHost::observe_pending_level_resolved(int index) const {
    return pending_intent_view().level_resolved(index);
}

int source::PineStrategyHost::observe_pending_effective_levels(
        int index, double* stop, double* limit, double* trail_activation) const {
    return pending_intent_view().effective_levels(index, stop, limit, trail_activation);
}

double source::PineStrategyHost::observe_trail_best_price_v1() const {
    return adapter_.pending_intent_view().trail_best_price();
}

void source::PineStrategyHost::adapter_label_bracket_trades(
        const native_order::ExecutionAppliedEvent& event, bool from_bracket) {
    // ab9714be pine_fills.cpp:6232-6252: every trade row emitted by a real
    // strategy.exit leg carries the bracket cause; strategy.close and
    // close_all requests remain script closes.
    for (std::size_t offset = 0; offset < event.closed_trade_count; ++offset) {
        const std::size_t index = event.first_trade_index + offset;
        if (index >= trades_.size()) continue;
        auto& trade = trades_[index];
        trade.exit_from_bracket = from_bracket;
    }
}

bool source::PineStrategyHost::adapter_has_open_entry_id(
        const std::string& id) const {
    return std::any_of(pyramid_entries_.begin(), pyramid_entries_.end(),
        [&](const PyramidEntry& row) { return row.entry_id == id && row.qty > 0.0; });
}

const std::vector<source::PineStrategyHost::FixtureIntentRow>&
source::PineStrategyHost::source_pending_view() const {
    source_pending_view_cache_.clear();
    source_pending_view_cache_.reserve(adapter_.pending_same_bar_commands_.size()
        + adapter_.pending_entries_.size() + adapter_.pending_bracket_legs_.size()
        + adapter_.pending_coof_requests_.size()
        + adapter_.delayed_market_orders_.size()
        + adapter_.source_shadow_pending_.size() + adapter_.live_handles_.size());
    const auto append = [&](const PlacementSnapshot& snapshot, const std::string& label) {
        FixtureIntentKind type = FixtureIntentKind::MARKET;
        switch (snapshot.family) {
        case PineOrderFamily::Close:
        case PineOrderFamily::CloseAll:
        case PineOrderFamily::ExitLimit:
        case PineOrderFamily::ExitStop:
        case PineOrderFamily::ExitTrail:
        case PineOrderFamily::Margin:
        case PineOrderFamily::Risk:
            type = FixtureIntentKind::EXIT;
            break;
        case PineOrderFamily::Order:
            type = FixtureIntentKind::RAW_ORDER;
            break;
        case PineOrderFamily::Entry:
            break;
        }
        FixtureIntentRow row;
        row.id = snapshot.family == PineOrderFamily::Close
            ? "__close__" + snapshot.source_id
            : (snapshot.frozen_market_targeted_close ? label : snapshot.source_id);
        row.type = type;
        const bool default_stop = snapshot.family == PineOrderFamily::Entry
            && !std::isfinite(snapshot.exit_levels.limit)
            && std::isfinite(snapshot.exit_levels.stop)
            && std::isnan(snapshot.requested_qty)
            && config_.default_qty_type == static_cast<int>(QtyType::PERCENT_OF_EQUITY)
            && config_.default_qty_value <= 100.0;
        const double absent = std::numeric_limits<double>::quiet_NaN();
        row.default_stop_placement_qty = default_stop ? snapshot.sizing.frozen_units : absent;
        row.default_stop_sizing_price = snapshot.sizing.price;
        row.frozen_market_own_units = snapshot.frozen_market_own_units;
        row.frozen_market_transaction_units = snapshot.frozen_market_transaction_units;
        row.from_entry = snapshot.from_entry;
        row.is_long = snapshot.is_long;
        row.qty = snapshot.family == PineOrderFamily::Close
            ? snapshot.requested_qty
            : (std::isfinite(snapshot.projection_remaining_qty)
                ? snapshot.projection_remaining_qty : snapshot.requested_qty);
        row.qty_percent = snapshot.qty_percent;
        row.created_bar = snapshot.projection_created_bar;
        row.created_seq = static_cast<std::int64_t>(snapshot.source_sequence);
        row.incarnation = snapshot.source_sequence;
        row.over_pyramiding_cap_at_placement = snapshot.projection_over_pyramiding;
        row.paired_flat_market_peer_seq = 0;
        row.paired_flat_market_transaction_qty = std::numeric_limits<double>::quiet_NaN();
        row.frozen_default_qty = default_stop ? absent : snapshot.sizing.frozen_units;
        row.default_stop_placement_equity = default_stop
            ? snapshot.projection_default_stop_equity : absent;
        row.default_stop_placement_signal_close = default_stop
            ? snapshot.projection_default_stop_signal_close : absent;
        row.affordability_placement_equity = snapshot.projection_affordability_equity;
        row.market_admission = snapshot.market_admission;
        if (!row.market_admission.observation()
            && snapshot.family == PineOrderFamily::Entry
            && snapshot.frozen_market_instruction
            && std::isfinite(snapshot.requested_qty)) {
            auto observation = std::make_shared<admission::CommandObservation>();
            observation->command = snapshot.command_ordinal;
            observation->kind = admission::CommandKind::Entry;
            observation->birth = snapshot.birth.cause() == OrderBirthCause::Unattributed
                ? OrderBirth::chart_evaluation(source_bar_index_, current_bar_.timestamp)
                : snapshot.birth;
            observation->id = snapshot.source_id;
            observation->requested_quantity = snapshot.requested_qty;
            observation->quantity_type = snapshot.qty_type;
            observation->buy = snapshot.is_long;
            observation->prices = {snapshot.exit_levels.limit, snapshot.exit_levels.stop};
            observation->oca_name = snapshot.oca_name;
            observation->oca_type = snapshot.oca_type;
            auto& configuration = observation->configuration;
            configuration.process_on_close = config_.process_orders_on_close;
            configuration.calc_on_fills = config_.calc_on_order_fills;
            configuration.slippage = config_.slippage;
            configuration.pyramiding = config_.pyramiding;
            configuration.default_quantity_type = config_.default_qty_type;
            configuration.default_quantity_value = config_.default_qty_value;
            configuration.long_margin = config_.margin_long;
            configuration.short_margin = config_.margin_short;
            configuration.commission_value = config_.commission_value;
            configuration.commission_type = config_.commission_type;
            configuration.pointvalue = staged_configuration().syminfo.pointvalue;
            configuration.fx = snapshot.sizing.fx;
            configuration.quantity_step = staged_configuration().quantity_grid
                ? *staged_configuration().quantity_grid : 0.0;
            configuration.mintick = staged_configuration().syminfo.mintick;
            observation->bar = source_bar_index_;
            observation->placement_side = static_cast<int>(PositionSide::FLAT);
            observation->placement_cycle = snapshot.placement_cycle;
            observation->held_quantity = 0.0;
            observation->held_entries = 0;
            observation->realized_equity = snapshot.sizing.equity;
            observation->placement_equity = snapshot.sizing.equity;
            observation->signal_close = snapshot.sizing.price;
            observation->quantized_fixed_quantity =
                snapshot.frozen_market_own_units;
            observation->original_sizing = admission::SizingObservation{
                snapshot.requested_qty, snapshot.sizing.equity,
                snapshot.sizing.price, snapshot.sizing.mark, snapshot.sizing.fx};
            row.market_admission.bind(std::move(observation));
        }
        source_pending_view_cache_.push_back(std::move(row));
    };
    for (const auto& command : adapter_.pending_same_bar_commands_) {
        // ab9714be strategy.close under process_orders_on_close is held in the
        // same-bar close accumulator until the callback returns; the legacy
        // pending_orders_ observer therefore sees the two entry commands but
        // not that staged close during the source body.
        if (config_.process_orders_on_close
            && command.snapshot.family == PineOrderFamily::Close
            && !command.snapshot.birth.at_terminal_fill()) {
            continue;
        }
        append(command.snapshot, command.request.label);
    }
    for (const auto& pending : adapter_.pending_entries_)
        append(pending.snapshot, pending.request.label);
    for (const auto& delayed : adapter_.delayed_market_orders_)
        append(delayed.snapshot, delayed.request.label);
    for (const auto& leg : adapter_.pending_bracket_legs_)
        append(leg.snapshot, leg.request.label);
    for (const auto& pending : adapter_.pending_coof_requests_)
        append(pending.snapshot, pending.request.label);
    for (const auto& shadow : adapter_.source_shadow_pending_)
        append(shadow.snapshot, shadow.label);
    for (const auto& handle : adapter_.live_handles_) {
        const auto found = adapter_.placement_.find(handle.incarnation);
        if (found == adapter_.placement_.end()) continue;
        if (config_.process_orders_on_close
            && found->second.family == PineOrderFamily::Close
            && found->second.projection_created_bar == source_bar_index_
            && !found->second.birth.at_terminal_fill()) {
            continue;
        }
        append(found->second, found->second.source_id);
    }
    return source_pending_view_cache_;
}

void source::PineStrategyHost::source_stream_entry_comment(
        const PyramidEntry&, std::string&) const {}

void source::PineStrategyHost::project_short_seed_report_rows(
        const native_order::ExecutionAppliedEvent& event) {
    const ShortSeedPlan plan = adapter_.short_seed_;
    if (!plan.report_swap_pending || event.closed_trade_count == 0
        || event.handle() == plan.final_short) {
        return;
    }
    std::optional<PlacementSnapshot> placement_snapshot;
    if (const auto placement = adapter_.placement_.find(event.handle().incarnation);
        placement != adapter_.placement_.end()) {
        placement_snapshot = placement->second;
    }
    if (!placement_snapshot || placement_snapshot->family != PineOrderFamily::Close
        || (placement_snapshot->from_entry != plan.seed_id
            && placement_snapshot->source_id != plan.seed_id)) {
        return;
    }
    for (auto& trade : trades_) {
        if (trade.entry_incarnation == plan.materialize_long.incarnation
            && trade.entry_id == plan.materialize_label) {
            trade.entry_incarnation = plan.final_short.incarnation;
        }
    }
    const std::size_t begin = event.first_trade_index;
    const std::size_t end = begin + event.closed_trade_count;
    for (std::size_t index = begin; index < end && index < trades_.size(); ++index) {
        if (trades_[index].entry_incarnation == plan.final_short.incarnation
            && trades_[index].entry_id == plan.final_short_id) {
            trades_[index].entry_incarnation = plan.materialize_long.incarnation;
        }
    }
    adapter_.short_seed_.report_swap_pending = false;
}

void source::PineStrategyHost::scheduler_prepare_script_run(
        const std::vector<Bar>& bars, bool static_eligible,
        int expected_script_bars, bool script_bar_geometry) {
    if (const auto state = native_state(); state.spec) {
        input_tf_ = state.spec->timeframe_undetected ? "" : state.spec->input_tf;
        script_tf_ = state.spec->timeframe_undetected ? "" : state.spec->script_tf;
        script_tf_seconds_ = tf_to_seconds(script_tf_);
    }
    prepare_script_run(bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()),
                       static_eligible);
    last_bar_index_ = expected_script_bars - 1;
    last_bar_time_ = bars.empty() ? 0 : bars.back().timestamp;
    apply_realtime_tail_horizon(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()),
        script_bar_geometry);
    source_last_bar_index_ = last_bar_index_;
}

void source::PineStrategyHost::scheduler_configure_security_evaluators() {
    configure_security_evaluators();
}

bool source::PineStrategyHost::scheduler_uses_aux_security_feed() const noexcept {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    return aux_security_feed_enabled();
#else
    return false;
#endif
}

void source::PineStrategyHost::scheduler_prepare_security_sequence(
        const std::vector<Bar>& bars) {
    security_input_tf_ = input_tf_;
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) security_input_tf_ = aux_security_input_tf_;
#endif
    validate_security_timeframes(security_input_tf_);
    security_first_chart_bar_ms_ = bars.empty() ? 0 : bars.front().timestamp;
    init_security_eval_states_for_run(security_input_tf_);
    prepare_native_security_feeds(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()));
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) {
        prepare_aux_security_chart_ranges(
            bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()), script_tf_);
    }
#endif
    prepare_historical_security_lookahead_projections(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()), input_tf_);
    prepare_chart_day_partition(
        bars.empty() ? nullptr : bars.data(), static_cast<int>(bars.size()));
}

bool source::PineStrategyHost::scheduler_feed_security_input(
        const Bar& bar, std::int64_t next_input_ms, bool calling_bar_complete,
        bool defer_boundary_gate) {
    security_next_input_ms_ = next_input_ms;
    security_calling_close_ms_ = 0;
    bool deferred = false;
    for (auto& state : security_eval_states_) {
        if (defer_boundary_gate && state.publish_gate_tf_seconds > 0) {
            deferred = true;
            continue;
        }
        feed_security_eval_state(state, bar, calling_bar_complete);
    }
    return deferred;
}

void source::PineStrategyHost::scheduler_publish_security_boundary() {
    for (auto& state : security_eval_states_) {
        if (state.publish_gate_tf_seconds > 0)
            publish_security_eval_state_at_calling_boundary(state);
    }
}

void source::PineStrategyHost::scheduler_feed_deferred_security_input(
        const Bar& bar, std::int64_t next_input_ms) {
    security_next_input_ms_ = next_input_ms;
    security_calling_close_ms_ = 0;
    for (auto& state : security_eval_states_) {
        if (state.publish_gate_tf_seconds > 0)
            feed_security_eval_state(state, bar, false);
    }
}

void source::PineStrategyHost::scheduler_feed_aux_security(int chart_index) {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled() && scheduler_coof_enabled()) {
        // ab9714be pine_scheduler.cpp:1705-1708 feeds the auxiliary slice
        // before dispatch_bar, whose COOF bar takes its script checkpoint
        // only afterwards (pine_scheduler.cpp:467).  The native scheduler
        // checkpoints at the script-bar open and restores that checkpoint
        // after this feed, which would discard the fed request.security
        // values every bar.  Rebase the checkpoint on the open state plus
        // this feed so the restore keeps them.
        restore_script_state();
        feed_aux_security_for_chart_bar(chart_index);
        snapshot_script_state();
        return;
    }
    if (aux_security_feed_enabled()) feed_aux_security_for_chart_bar(chart_index);
#else
    (void)chart_index;
#endif
}

void source::PineStrategyHost::scheduler_feed_deferred_aux_security(int chart_index) {
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    if (aux_security_feed_enabled()) feed_deferred_aux_security_for_chart_bar(chart_index);
#else
    (void)chart_index;
#endif
}

void source::PineStrategyHost::scheduler_finish_security_sequence() {
    clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    clear_aux_security_chart_ranges();
#endif
}

static void sort_same_bar_exit_trades(std::vector<Trade>&, source::PineExecutionAdapter&);

void source::PineStrategyHost::scheduler_record_range_end(const Bar& terminal_bar) {
    range_end_trades_.clear();
    if (stream_warmup_mode_ || realtime_tail_
        || position_side_ == PositionSide::FLAT || equity_curve_.empty()
        || !std::isfinite(terminal_bar.close)) return;
    const Bar saved = current_bar_;
    current_bar_ = terminal_bar;
    const bool was_long = position_side_ == PositionSide::LONG;
    const double fill_price = bar_fill_price(current_bar_.close);
    const auto saved_timestamp = current_bar_.timestamp;
    current_bar_.timestamp = equity_curve_.back().time_ms;
    double range_end_pnl = 0.0;
    excursion_range_end_projection_ = true;
    for (const auto& lot : pyramid_entries_) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = current_bar_.timestamp;
        context.interval_index = bar_index_;
        if (!std::isnan(fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = fold_exit_trail_peak_;
        Trade row = build_close_trade_with_costs(
            lot, lot.qty, fill_price, was_long,
            allocated_entry_commission(lot, lot.qty), calc_commission(fill_price, lot.qty),
            context);
        row.open_at_end = true;
        range_end_pnl += row.pnl;
        range_end_trades_.push_back(std::move(row));
    }
    excursion_range_end_projection_ = false;
    current_bar_.timestamp = saved_timestamp;
    auto& last = equity_curve_.back();
    last.open_profit = 0.0;
    last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl;
    max_equity_ = initial_capital_;
    min_equity_ = initial_capital_;
    max_drawdown_ = 0.0;
    max_runup_ = 0.0;
    for (const auto& point : equity_curve_) fold_equity_extreme(point.equity);
    sort_same_bar_exit_trades(trades_, adapter_);
    current_bar_ = saved;
}

// ab9714be pine_fills.cpp:664-670: same-bar bracket exit trades sort by script command sequence created_seq
static void sort_same_bar_exit_trades(std::vector<Trade>& trades,
                                      source::PineExecutionAdapter& adapter) {
    if (trades.size() < 2) return;
    const std::size_t end = trades.size();
    std::size_t start = end - 1;
    while (start > 0
           && trades[start - 1].exit_time == trades[end - 1].exit_time
           && trades[start - 1].entry_time == trades[end - 1].entry_time
           && trades[start - 1].entry_id == trades[end - 1].entry_id
           && trades[start - 1].exit_from_bracket
           && trades[end - 1].exit_from_bracket) {
        --start;
    }
    if (end - start > 1) {
        // ab9714be pine_scheduler.cpp:262-283 and pine_fills.cpp:3710-3730:
        // fills on the bar OPEN precede intrabar fills chronologically. Sibling
        // bracket exits that fill within the same phase tie-break by command
        // sequence (test_l10m_corpus_parity.cpp).
        std::vector<std::size_t> indices(end - start);
        std::iota(indices.begin(), indices.end(), start);
        std::stable_sort(indices.begin(), indices.end(),
            [&](std::size_t ia, std::size_t ib) {
                const bool open_a = adapter.is_open_phase_exit(ia);
                const bool open_b = adapter.is_open_phase_exit(ib);
                if (open_a != open_b) return open_a;
                const auto sa = adapter.command_sequence_for_exit(trades[ia].exit_id, trades[ia].entry_id);
                const auto sb = adapter.command_sequence_for_exit(trades[ib].exit_id, trades[ib].entry_id);
                return sa < sb;
            });
        std::vector<Trade> sorted;
        sorted.reserve(end - start);
        for (std::size_t idx : indices) sorted.push_back(std::move(trades[idx]));
        for (std::size_t i = 0; i < sorted.size(); ++i) trades[start + i] = std::move(sorted[i]);
        // The adapter's exit phases are keyed by trade index: move them with
        // the trades so the next call reads each trade's own phase.
        adapter.permute_exit_phases(start, indices);
    }
}

void source::PineStrategyHost::scheduler_update_session_state(
        const Bar& bar, std::optional<std::int64_t> next_script_open_ms) {
    const bool in_session = chart_bar_ismarket(bar.timestamp);
    bool next_in_session = false;
    if (in_session && next_script_open_ms) {
        next_in_session = chart_bar_ismarket(*next_script_open_ms);
    } else if (in_session && realtime_tail_ && script_tf_seconds_ > 0
               && bar.timestamp <= std::numeric_limits<std::int64_t>::max()
                    - static_cast<std::int64_t>(script_tf_seconds_) * 1000) {
        next_in_session = chart_bar_ismarket(
            bar.timestamp + static_cast<std::int64_t>(script_tf_seconds_) * 1000);
    } else if (in_session && realtime_tail_) {
        next_in_session = true;
    }
    session_ismarket_ = in_session;
    if (tf_is_daily_or_higher(script_tf_)) {
        session_isfirstbar_ = in_session;
        session_islastbar_ = in_session;
    } else {
        session_isfirstbar_ = in_session && !prev_in_session_;
        session_islastbar_ = in_session && !next_in_session;
    }
    prev_in_session_ = in_session;
}

void source::PineStrategyHost::scheduler_publish_source_bar(
        const Bar& bar, bool, bool advance_source_index) {
    current_bar_ = bar;
    const bool temporary_index = !advance_source_index
        && (!scheduler_.current_script_bar()
            || scheduler_.current_script_bar()->timestamp != bar.timestamp);
    const int previous_bar_index = bar_index_;
    const bool previous_barstate_islast = barstate_islast_;
    if (advance_source_index || temporary_index) ++source_bar_index_;
    ++source_callback_count_;
    bar_index_ = source_bar_index_;
    const auto lifecycle = native_state();
    if (lifecycle.kind == NativeLifecycleKind::Running
        && lifecycle.phase == NativeRunPhase::Warmup) {
        barstate_islast_ = false;
    } else if (lifecycle.kind == NativeLifecycleKind::Running
               && lifecycle.phase == NativeRunPhase::Realtime) {
        barstate_islast_ = true;
    } else {
        barstate_islast_ = source_bar_index_ == source_last_bar_index_;
    }
    NativeDayPartitionScope chart_day_partition(
        chart_day_partition_.empty() ? nullptr : &chart_day_partition_);
    // A named-entry cancellation token has source-evaluation scope.  Clear a
    // prior callback before publishing receipts and entering this body.
    adapter_.begin_source_evaluation();
    // Publish terminal and group-adjustment receipts before the source body
    // reads its public pending projection at this decision boundary.
    sort_same_bar_exit_trades(trades_, adapter_);
    adapter_.observe_terminal_receipts();
    // ab9714be src/source/pine_scheduler.cpp:242,258: under
    // process_orders_on_close the orders that were already resting fill at step 1
    // (process_pending_orders(before_pooc_script=true)) BEFORE the strategy body
    // runs at step 3, so a bracket leg this route parked while its parent entry
    // was pending settles on the touch bar ahead of this bar's source
    // evaluation. Draining it here keeps the body's position state, and the
    // levels it re-prices, on the same side of the fill as the owner; a leg that
    // carries a predecessor receipt stays staged for the flush below the body.
    adapter_.flush_pending_bracket_legs({}, /*post_calculation=*/false,
                                       /*pre_script_drain=*/true);
    struct ChartEmaNaWarmupScope {
        bool previous;
        explicit ChartEmaNaWarmupScope(bool enabled)
            : previous(ta::ema_na_warmup_flag()) {
            ta::ema_na_warmup_flag() = enabled;
        }
        ~ChartEmaNaWarmupScope() { ta::ema_na_warmup_flag() = previous; }
    } ema_scope(chart_ema_na_warmup_);
    ta::BarContextScope bar_scope(pine_bar_index(), scheduler_.bar_index_offset());
    position_entry_count_ = physical_position().signed_units == 0.0
        ? 0 : adapter_.source_entry_slot_count();
    on_source_bar(bar);
    // Handwritten/source-generated callbacks historically read and could
    // update the live Pine configuration fields directly.  Keep the adapter's
    // source policy view synchronized at the callback boundary; the generic
    // NativeRunSpec remains immutable for the run.
    adapter_.set_configuration(config_);
    if (temporary_index) {
        --source_bar_index_;
        bar_index_ = previous_bar_index;
        barstate_islast_ = previous_barstate_islast;
    }
    adapter_.flush_pending_closes();
    adapter_.flush_pending_entries();
    adapter_.flush_pending_bracket_legs();
    if (advance_source_index) {
        scheduler_mark_report_point(bar.timestamp);
        prev_bar_timestamp_ = bar.timestamp;
    }
}

void source::PineStrategyHost::scheduler_publish_suppressed_tail(const Bar& bar) {
    // ab9714be pine_scheduler.cpp:222-231: the forming probe tail advances
    // source history and settles the already-matched broker book, but does
    // not invoke generated code or synthesize a range-end close.
    current_bar_ = bar;
    ++source_bar_index_;
    bar_index_ = source_bar_index_;
    barstate_islast_ = false;
    NativeDayPartitionScope chart_day_partition(
        chart_day_partition_.empty() ? nullptr : &chart_day_partition_);
    adapter_.begin_source_evaluation();
    adapter_.observe_terminal_receipts();
    scheduler_mark_report_point(bar.timestamp);
    prev_bar_timestamp_ = bar.timestamp;
}

// The Pine report series has one point per SOURCE slot this host published,
// which is not the kernel's per-calculation cadence: a calc_on_order_fills
// re-entry marks the slot it opened at the fill, the ordinary close
// calculation then marks nothing, and the probe's suppressed tail marks a
// slot generated code never calculated. The point also has to land inside
// this callback, before scheduler_record_broker_hash() folds the extremes it
// just moved and before scheduler_record_range_end() re-marks the curve's
// last point. So the cadence stays here and the recording does not: the
// kernel owns what a report point is — the extremes fold and the curve
// append in engine.hpp — reached through the run spec's report policy.
void source::PineStrategyHost::scheduler_mark_report_point(std::int64_t script_bar_ts) {
    as_native_consumer(execution_consumer())
        .mark_script_report_point(*this, script_bar_ts);
}

void source::PineStrategyHost::scheduler_record_broker_hash() {
    if (!broker_state_hash_recording_) return;
    last_script_continuation_hash_ = execution_consumer().continuation_hash();
    last_script_continuation_valid_ = true;
    broker_state_hashes_.push_back(broker_state_hash());
}

void source::PineStrategyHost::scheduler_set_session_bar_state(
        bool in_session, bool intraday_is_last_bar) {
    // ab9714be pine_scheduler.cpp:1661-1675.  These generated Pine facts are
    // sourced by the scheduler immediately before the source callback; they
    // are not generic native-calendar policy.
    session_ismarket_ = in_session;
    if (tf_is_daily_or_higher(script_tf_)) {
        session_isfirstbar_ = in_session;
        session_islastbar_ = in_session;
        return;
    }
    session_isfirstbar_ = in_session && !prev_in_session_;
    session_islastbar_ = intraday_is_last_bar;
}

execution::AccountEffectProjection source::PineStrategyHost::adapter_project_flatten(
        double price, const std::string& id, const std::string& comment,
        std::uint64_t incarnation) const {
    return project_native_settlement_v1(
        execution::Flatten{}, execution::Fill{price, id, comment, incarnation});
}

bool source::PineStrategyHost::adapter_core_sizes_default_opening(bool is_long) const {
    return adapter_.core_sizes_default_opening(is_long);
}

} // namespace pineforge
