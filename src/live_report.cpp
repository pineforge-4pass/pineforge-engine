#include "live_report.hpp"
#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"
#include "report_scratch.hpp"
#include "report_value_invalid.hpp"
#include <pineforge/metrics_stream.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge {
inline namespace engine_script_run_v20 {

struct LiveReportAccumulator::State {
    const BacktestEngine* owner = nullptr;
    const NativeExecutionConsumer* consumer = nullptr;
    std::uint64_t generation = 0, cursor = 0, interval;
    std::size_t closed = 0, hashes = 0, names = 0, reported_points = 0;
    double capital = 0.0, net_prefix = 0.0;
    std::unique_ptr<metrics::TradeStatsAccumulator> all, longs, shorts;
    metrics::EquityMetricsFold equity_fold;
    // Raw engine points staged so far, including its held point. Presentation
    // is confined to a value copy of the fold and the ratio workspace.
    std::size_t points = 0;
    // Reuse the presentation suffix and the batch ratio workspace on each
    // read. The workspace carries the raw prefix and a presented held point:
    // a resident array, or (scratch) the engine's binary curve scratch file,
    // which keeps one fixed batch of points in the process.
    std::vector<TradeC> trade_rows;
    std::vector<pf_equity_point_t> ratio_curve;
    const bool scratch = false;
    std::unique_ptr<report_sink::ScratchCurveWriter> scratch_curve;
    // The stream's engine when the points this reader has taken leave its own
    // vector too. engine.equity_curve_[k] is then report point retired + k.
    BacktestEngine* const released = nullptr;
    std::size_t retired = 0;
    pf_equity_stats_t ratios{};
    bool have_ratios = false;
    std::uint64_t ratios_cursor = 0;
    report::LiveReportWork work;

    explicit State(std::uint64_t k) : interval(k) {}
    // Opens the scratch file here, so a directory that cannot back it (a
    // memory filesystem, no permission) is refused before any report read.
    State(std::uint64_t k, const std::string& directory, BacktestEngine* engine)
        : interval(k), scratch(true),
          scratch_curve(std::make_unique<report_sink::ScratchCurveWriter>(directory)),
          released(engine) {}

    report_sink::ScratchCurveWriter& writer() { return *scratch_curve; }
    void stage_append(const pf_equity_point_t& point) {
        if (scratch) writer().append(point);
        else ratio_curve.push_back(point);
        ++points;
    }
    void stage_last(const pf_equity_point_t& point) {
        if (scratch) writer().replace_last(point);
        else ratio_curve.back() = point;
    }
};

LiveReportAccumulator::LiveReportAccumulator(std::uint64_t ratio_interval)
    : state_(std::make_unique<State>(ratio_interval)) {
    if (!ratio_interval) throw std::invalid_argument("ratio-interval must be positive");
}
LiveReportAccumulator::LiveReportAccumulator(
        std::uint64_t ratio_interval, const std::string& scratch_directory,
        BacktestEngine* released)
    : state_(std::make_unique<State>(ratio_interval, scratch_directory, released)) {
    if (!ratio_interval) throw std::invalid_argument("ratio-interval must be positive");
}
LiveReportAccumulator::~LiveReportAccumulator() = default;
const report::LiveReportWork& LiveReportAccumulator::work() const noexcept { return state_->work; }
std::uint64_t LiveReportAccumulator::ratios_as_of_input() const noexcept {
    return state_->ratios_cursor;
}
report::LiveReportPatch LiveReportAccumulator::update(
        const BacktestEngine& engine, std::uint64_t input_index, bool final_input) {
    return NativeExecutionConsumer::fill_live_report_patch(engine, *this, input_index, final_input);
}

void keep_live_event_window(BacktestEngine& engine, NativeEventRetention retention) {
    // A configured native host has bound its consumer: nothing is created here.
    NativeExecutionConsumer::bound(engine).set_retention_override(retention);
}

report::LiveReportPatch NativeExecutionConsumer::fill_live_report_patch(
        const BacktestEngine& engine, LiveReportAccumulator& accumulator,
        std::uint64_t input_index, bool final_input) try {
    const BacktestEngine::ReportSinkCallScope call(engine, true);
    report::LiveReportPatch patch;
    ReportC gate{};
    if (refuse_report_in_dispatch(engine, &gate)) return patch;
    const auto* consumer = static_cast<const NativeExecutionConsumer*>(
        engine.execution_consumer_slot_.ptr.get());
    auto& state = *accumulator.state_;
    if (state.released && state.released != &engine)
        throw std::logic_error("live report scratch is bound to another engine");
    const auto fallback = [&] {
        if (state.retired) {
            // A whole-report read needs the whole curve: put the points this
            // reader took back in front of the ones the engine still holds.
            std::vector<pf_equity_point_t> prefix(state.retired);
            report_sink::read_scratch_points(state.writer().region(), 0, prefix.data(),
                                             static_cast<std::int64_t>(prefix.size()));
            auto& curve = state.released->equity_curve_;
            curve.insert(curve.begin(), prefix.begin(), prefix.end());
            state.retired = 0;
        }
        ReportC full{};
        struct Release {
            ReportC& value;
            ~Release() { BacktestEngine::free_report(&value); }
        } release{full};
        ++state.work.fallback_report_reads;
        fill_snapshot_report(engine, &full);
        report::LiveReportPatch result;
        std::memcpy(&result.scalars, &full, sizeof result.scalars);
        result.scalars.trades = nullptr; result.scalars.equity_curve = nullptr;
        result.scalars.security_diag = nullptr; result.scalars.broker_state_hash = nullptr;
        if (full.trace_len)
            throw std::runtime_error("cumulative report deltas require tracing to remain disabled");
        result.scalars.trace = nullptr; result.scalars.trace_names = nullptr;
        const auto suffix = [](auto& to, const auto* rows, std::size_t n, std::size_t start,
                               std::uint64_t& counter) {
            to.offset = std::min(start, n);
            to.logical_length = n;
            for (auto i = to.offset; i < n; ++i) {
                static_assert(sizeof to.rows.back() == sizeof rows[i], "report row wire layout");
                to.rows.emplace_back();
                std::memcpy(&to.rows.back(), &rows[i], sizeof rows[i]);
                ++counter;
            }
        };
        suffix(result.trades, full.trades, static_cast<std::size_t>(full.trades_len),
               state.closed ? state.closed - 1 : 0, state.work.trade_rows_materialized);
        suffix(result.equity, full.equity_curve, static_cast<std::size_t>(full.equity_curve_len),
               state.reported_points ? state.reported_points - 1 : 0, state.work.equity_rows_materialized);
        suffix(result.security, full.security_diag, static_cast<std::size_t>(full.security_diag_len),
               0, state.work.security_rows_materialized);
        suffix(result.hashes, full.broker_state_hash, static_cast<std::size_t>(full.broker_state_hash_len),
               state.hashes ? state.hashes - 1 : 0, state.work.hash_rows_materialized);
        result.trace_names.offset = std::min(state.names ? state.names - 1 : 0,
                                             static_cast<std::size_t>(full.trace_names_len));
        result.trace_names.logical_length = static_cast<std::size_t>(full.trace_names_len);
        for (auto i = result.trace_names.offset; i < result.trace_names.logical_length; ++i) {
            result.trace_names.rows.emplace_back(full.trace_names[i] ? full.trace_names[i] : "");
            ++state.work.trace_names_materialized;
        }
        result.closed_trades = state.closed;
        for (std::size_t i = state.closed; i < static_cast<std::size_t>(full.trades_len); ++i) {
            ++state.work.trade_rows_materialized;
            if (full.trades[i].open_at_end) break;
            ++result.closed_trades;
        }
        state.closed = result.closed_trades;
        state.reported_points = result.equity.logical_length;
        state.hashes = result.hashes.logical_length;
        state.names = result.trace_names.logical_length;
        // Captured/omitted reports retain their original complete semantics.
        // Interval deferral is confined to the ordinary live reader below.
        return result;
    };
    // Unlike a batch read, the explicit stream reader asks whose rows these
    // are, even after a refused batch call over a live stream.
    if ((consumer && !consumer->rows_current_)
        || (engine.last_run_status_ != 0
            && run_failure_value_of(engine).code == RunFailureCode::report_sink_failed))
        return fallback();
    // Preserve the selected router and its refusal/capture behavior. These
    // modes are outside this ordinary-live reader; no ledger is read first.
    if (consumer && (consumer->selected_epoch_.admitted_selected
                     || consumer->selected_epoch_.phase_observed)) {
        return fallback();
    }
    if (engine.report_sink_state_.selected_equity_stream) {
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "live_selected_sink"}},
            "native selected report: a live sink report is unavailable before capture");
    }
    if (state.work.fallback_report_reads) return fallback();
    if (!engine.stream_is_realtime() || engine.report_mode_ != ReportMode::FULL
        || engine.active_equity_report_mode_ != ReportMode::FULL
        || engine.report_sink_state_.enabled || engine.report_sink_state_.receipt.complete
        || engine.trades_.base() != 0) {
        return fallback();
    }
    // Native presenters declare bounded support. The host-mark provider's
    // ordinary REALTIME presentation is also a held-point operation; its
    // completed-batch terminal-quote presenter is excluded by the live gate.
    const auto view = consumer ? consumer->view() : NativeStateView{};
    if (!engine.report_presenter_supports_non_full()
        && (!view.spec || view.spec->report_policy != NativeReportPolicy::KernelRecordedAtHostMarks
            || view.kind != NativeLifecycleKind::Running))
        return fallback();
    if (!engine.trace_buffer_.empty())
        throw std::runtime_error("cumulative report deltas require tracing to remain disabled");

    const auto generation = consumer ? consumer->selected_epoch_.generation_counter : 0;
    const std::size_t closed = engine.trades_.size();
    const std::size_t open = engine.range_end_trades_.size();
    // Report points: the ones this reader took out of the engine, then its own.
    const std::size_t engine_points = state.retired + engine.equity_curve_.size();
    if (closed > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || open > static_cast<std::size_t>(std::numeric_limits<int>::max()) - closed)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report trade count exceeds the report row limit");
    if (!state.owner) {
        if (input_index != 0)
            throw std::logic_error("carried live report must begin at the warmup cursor");
        state.owner = &engine;
        state.consumer = consumer;
        state.generation = generation;
        state.capital = engine.initial_capital_;
        state.all = std::make_unique<metrics::TradeStatsAccumulator>(
            metrics::TradeFilter::ALL, state.capital);
        state.longs = std::make_unique<metrics::TradeStatsAccumulator>(
            metrics::TradeFilter::LONG, state.capital);
        state.shorts = std::make_unique<metrics::TradeStatsAccumulator>(
            metrics::TradeFilter::SHORT, state.capital);
    } else if (state.owner != &engine || state.consumer != consumer
               || state.generation != generation || input_index < state.cursor
               || closed < state.closed || engine_points < state.points
               || engine.broker_state_hashes_.size() < state.hashes) {
        throw std::logic_error("carried live report stream or immutable prefix changed");
    }

    engine.fold_pending_hashes_into_witness();
    engine.fold_current_output_row_witness();
    const auto trade_start = state.closed ? state.closed - 1 : 0;
    patch.closed_trades = closed;
    patch.trades.offset = std::min(trade_start, closed + open);
    patch.trades.logical_length = closed + open;
    auto& rows = state.trade_rows;
    rows.clear();
    rows.reserve(closed + open - trade_start);
    double net = state.net_prefix;
    for (std::size_t i = trade_start; i < closed + open; ++i) {
        const Trade row = i < closed ? engine.trades_.numeric_at(i)
                                    : engine.range_end_trades_.row(i - closed);
        ++state.work.trade_rows_materialized;
        report::validate_trade(row);
        rows.push_back(internal::report_trade_row(row));
        net = report::finite_value(net + row.pnl, "summary.net_pnl");
        ++state.work.trade_rows_folded;
    }

    const auto old_points = state.points;
    patch.equity.offset = old_points ? old_points - 1 : 0;
    patch.equity.logical_length = engine_points;
    for (std::size_t i = patch.equity.offset; i < patch.equity.logical_length; ++i) {
        const auto& point = engine.equity_curve_[i - state.retired];
        ++state.work.equity_rows_materialized;
        const auto key = metrics::equity_month_key(point.time_ms, engine.chart_timezone_);
        if (i < old_points) {
            state.equity_fold.replace_last(point, key);
            // Restore the previous presented tail before it becomes prefix.
            state.stage_last(point);
            ++state.work.ratio_points_supplied;
        } else {
            state.equity_fold.fold(point, key);
            state.stage_append(point);
            ++state.work.ratio_points_supplied;
        }
        ++state.work.equity_rows_folded;
        patch.equity.rows.push_back(point);
    }

    ReportC presented{};
    presented.total_trades = static_cast<int>(closed + open);
    presented.trades = rows.empty() ? nullptr : rows.data();
    presented.trades_len = static_cast<int>(rows.size());
    presented.net_profit = net;
    presented.input_bars_processed = engine.diag_input_bars_processed_;
    presented.script_bars_processed = engine.diag_script_bars_processed_;
    presented.magnifier_sub_bars_total = engine.diag_magnifier_sub_bars_processed_;
    presented.magnifier_sample_ticks_total = engine.diag_magnifier_sample_ticks_processed_;
    presented.input_tf_seconds = internal::tf_to_clock_seconds(engine.input_tf_);
    presented.script_tf_seconds = engine.script_tf_seconds_;
    presented.script_tf_ratio = engine.diag_script_tf_ratio_;
    presented.needs_aggregation = engine.diag_needs_aggregation_ ? 1 : 0;
    presented.bar_magnifier_enabled = engine.bar_magnifier_enabled_ ? 1 : 0;
    // The live presentation sees the independent held point. Completed-batch
    // whole-array presenters are never called through this entry.
    pf_equity_point_t tail{};
    if (!patch.equity.rows.empty()) tail = patch.equity.rows.back();
    presented.equity_curve = patch.equity.rows.empty() ? nullptr : &tail;
    presented.equity_curve_len = patch.equity.rows.empty() ? 0 : 1;
    const auto* trade_pointer = presented.trades;
    const auto trade_count = presented.trades_len;
    const auto* equity_pointer = presented.equity_curve;
    const auto equity_count = presented.equity_curve_len;
    engine.present_report(&presented);
    if (presented.trades != trade_pointer || presented.trades_len != trade_count
        || presented.equity_curve != equity_pointer || presented.equity_curve_len != equity_count)
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "live_report_tail"}},
            "live report presentation must preserve its suffix and held-point views");
    if (!patch.equity.rows.empty()) {
        patch.equity.rows.back() = tail;
        state.stage_last(tail);
        ++state.work.ratio_points_supplied;
    }

    // Retain the prefix BEFORE the last closed row. Its report replacement
    // never needs a subtraction or a reassociated partial sum.
    const auto fold_trade = [&](auto& all, auto& longs, auto& shorts, const TradeC& row) {
        all.fold(row); ++state.work.trade_rows_folded;
        longs.fold(row); ++state.work.trade_rows_folded;
        shorts.fold(row); ++state.work.trade_rows_folded;
    };
    for (std::size_t i = trade_start; i + 1 < closed; ++i) {
        const auto& row = rows[i - trade_start];
        state.net_prefix = report::finite_value(state.net_prefix + row.pnl, "summary.net_pnl");
        ++state.work.trade_rows_folded;
        fold_trade(*state.all, *state.longs, *state.shorts, row);
    }
    auto all = *state.all, longs = *state.longs, shorts = *state.shorts;
    const auto unsealed = closed ? closed - 1 : 0;
    for (std::size_t i = unsealed; i < closed + open; ++i)
        fold_trade(all, longs, shorts, rows[i - trade_start]);
    presented.metrics.all = all.finish();
    presented.metrics.longs = longs.finish();
    presented.metrics.shorts = shorts.finish();

    auto equity = state.equity_fold;
    if (equity_count) {
        equity.replace_last(tail, metrics::equity_month_key(tail.time_ms, engine.chart_timezone_));
        ++state.work.equity_rows_folded;
    }
    auto first = equity.first_pass();
    if (equity_count) ++state.work.equity_rows_folded;
    first.bars_in_market = engine.bars_in_market_;
    const metrics::EquityMetricInputs inputs{state.capital, engine.first_bar_open_,
                                            engine.current_bar_.close, presented.net_profit};
    presented.metrics.equity = metrics::finalize_equity_metrics(first, inputs);
    const bool boundary = input_index % state.interval == 0 || final_input;
    if ((!state.have_ratios || boundary)
        && (!state.have_ratios || state.ratios_cursor != input_index)) {
        // Use precisely the batch routine, including both generated-return
        // passes and their validation/NaN rules. Its cost is accounted apart.
        if (input_index == 0) ++state.work.warmup_ratio_recomputations;
        else ++state.work.ratio_recomputations;
        if (state.scratch && state.points) {
            // The same metric body and operation order as the array entry
            // point, reading the scratch curve through its one fixed window.
            const auto region = state.writer().region();
            const metrics::SpillCurve ratio_points(region);
            state.ratios = metrics::compute_equity_stats<metrics::SpillCurve>(
                ratio_points, region.point_count, state.capital,
                engine.chart_timezone_, inputs.first_open, inputs.last_close,
                engine.bars_in_market_, presented.net_profit);
        } else {
            // A mutable pointer would select the structural-source template.
            // The existing array entry point takes a pointer to const points.
            const pf_equity_point_t* ratio_points =
                state.ratio_curve.empty() ? nullptr : state.ratio_curve.data();
            state.ratios = metrics::compute_equity_stats(
                ratio_points,
                static_cast<std::int64_t>(state.ratio_curve.size()), state.capital,
                engine.chart_timezone_, inputs.first_open, inputs.last_close,
                engine.bars_in_market_, presented.net_profit);
        }
        state.ratios_cursor = input_index;
        state.have_ratios = true;
    }
    auto& e = presented.metrics.equity;
    e.sharpe_monthly = state.ratios.sharpe_monthly;
    e.sortino_monthly = state.ratios.sortino_monthly;
    e.sharpe_bar = state.ratios.sharpe_bar;
    e.sortino_bar = state.ratios.sortino_bar;

    for (const auto& row : rows) {
        pf_trade_t value{};
        static_assert(sizeof value == sizeof row, "trade wire layout");
        std::memcpy(&value, &row, sizeof value);
        patch.trades.rows.push_back(value);
        ++state.work.trade_rows_materialized;
    }
    patch.security.logical_length = engine.security_eval_states_.size();
    for (const auto& s : engine.security_eval_states_) {
        pf_security_diag_t diag{};
        diag.sec_id = s.sec_id; diag.feed_count = s.feed_count;
        diag.complete_count = s.eval_complete_count; diag.partial_count = s.eval_partial_count;
        patch.security.rows.push_back(diag);
        ++state.work.security_rows_materialized;
        presented.security_feeds_total += s.feed_count;
        presented.security_eval_complete_total += s.eval_complete_count;
        presented.security_eval_partial_total += s.eval_partial_count;
    }
    const auto hash_count = engine.optional_report_mode_ == report_sequence::ReportMode::SummaryOnly
        ? std::size_t{0} : engine.broker_state_hashes_.size();
    patch.hashes.offset = state.hashes ? state.hashes - 1 : 0;
    patch.hashes.logical_length = hash_count;
    for (std::size_t i = patch.hashes.offset; i < hash_count; ++i) {
        patch.hashes.rows.push_back(engine.broker_state_hashes_[i]);
        ++state.work.hash_rows_materialized;
    }
    const auto name_count = engine.optional_report_mode_ == report_sequence::ReportMode::SummaryOnly
        ? std::size_t{0} : engine.trace_names_.size();
    patch.trace_names.offset = state.names ? state.names - 1 : 0;
    patch.trace_names.logical_length = name_count;
    for (std::size_t i = patch.trace_names.offset; i < name_count; ++i) {
        patch.trace_names.rows.push_back(engine.trace_names_[i]);
        ++state.work.trace_names_materialized;
    }
    presented.trades = nullptr;
    presented.trades_len = static_cast<int>(patch.trades.logical_length);
    presented.equity_curve = nullptr;
    presented.equity_curve_len = static_cast<std::int64_t>(patch.equity.logical_length);
    presented.security_diag_len = static_cast<int>(patch.security.logical_length);
    presented.broker_state_hash_len = static_cast<std::int64_t>(patch.hashes.logical_length);
    presented.trace_names_len = static_cast<int>(patch.trace_names.logical_length);
    std::memcpy(&patch.scalars, &presented, sizeof patch.scalars);
    patch.interval_ratios = state.interval > 1;
    patch.ratios_as_of_input = state.ratios_cursor;
    patch.chart_timezone = engine.chart_timezone_;
    state.closed = closed;
    state.hashes = hash_count;
    state.names = name_count;
    state.reported_points = patch.equity.logical_length;
    state.cursor = input_index;
    if (state.released && engine.equity_curve_.size() > 1) {
        // Every point below the held one is in this patch and in the scratch
        // curve. Keep only the held point; the vector's own storage stays.
        // Nothing else walks the whole vector in a realtime stream: a
        // whole-report read comes through fallback() above, which puts the
        // points back first, and the range-end refold of the curve runs only
        // before the stream is realtime (this route requires realtime).
        // Points may leave only a realtime stream: erased before it, a later
        // range-end refold (pine_strategy_host.cpp, stream_phase_ != REALTIME)
        // would walk a curve that is no longer whole.
        // Readers of the whole engine curve, and why none sees it released:
        // fill_report (src/engine_report.cpp), through fill_snapshot_report
        // (src/native_execution_consumer.cpp), is reached on this stream
        // only through fallback() above (the runner's direct
        // strategy_stream_fill_report serves modules without the stamp,
        // which never release); the selected and observer captures
        // (capture_presentation, same file, from terminal_success) run only
        // with a window or observer registered, which the runner never
        // sets; the full refolds (same file) run from the range-end refold
        // (src/source/pine_strategy_host.cpp) only outside realtime, or
        // with a report sink, which this route refuses
        // (report_sink_state_.enabled, above), as does the sink restore
        // (same file). The envelope's count (include/pineforge/engine.hpp)
        // is read only by fill_report_envelope_v1 (src/engine_report.cpp),
        // which the runner never calls; other readers take the held point
        // only (last_equity_point, src/native_execution_consumer.cpp).
        if (!engine.stream_is_realtime())
            throw std::logic_error("live report released equity points before the stream is realtime");
        auto& curve = state.released->equity_curve_;
        const auto taken = curve.size() - 1;
        curve.erase(curve.begin(), curve.begin() + static_cast<std::ptrdiff_t>(taken));
        state.retired += taken;
    }
    return patch;
} catch (const std::exception& error) {
    if (!engine.last_error().empty()
        && classify_run_failure(error).code == RunFailureCode::report_result_invalid)
        throw coded<std::runtime_error>(run_failure_value_of(engine), engine.last_error());
    throw;
}

}
}
