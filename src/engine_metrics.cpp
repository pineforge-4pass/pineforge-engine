/*
 * engine_metrics.cpp — pure metric computations (trade stats + equity
 * stats) consumed by fill_report. Conventions (NaN rules, sign/percent
 * bases, annualization) are pinned in the per-field doxygen of
 * <pineforge/pineforge.h>; docs/pages/report-schema.md summarizes them.
 */
#include <pineforge/metrics_stream.hpp>
#include <pineforge/metrics_fold.hpp>
#include <pineforge/native_calendar.hpp>

#include "report_scratch.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "metrics_month_memo.hpp"
#include "metrics_annualization.hpp"
#include "report_value_invalid.hpp"
#include "timezone.hpp"

namespace pineforge {
namespace metrics {

ArrayCurve::ArrayCurve(const pf_equity_point_t* data, std::int64_t point_count)
    : data_(data), point_count_(point_count) {}

std::int64_t ArrayCurve::size() const {
    return point_count_;
}

pf_equity_point_t ArrayCurve::operator[](std::int64_t index) const {
    return data_[index];
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline bool match(const TradeC& t, TradeFilter f) {
    return f == TradeFilter::ALL || (f == TradeFilter::LONG) == (t.is_long != 0);
}

// (year*12 + month) bucket key for ts in chart tz. Caller hoists ONE
// ScopedTimezone guard around the whole walk — per-point guards are a
// process-global setenv/tzset round trip each (see timezone.hpp). The UTC
// key is detail::month_key_utc below.
inline int64_t month_key_local(int64_t ts_ms) {   // call ONLY under ScopedTimezone
    // Preserve the report's whole-second truncation, including before 1970.
    const time_t secs = static_cast<time_t>(ts_ms / 1000);
    struct tm tb {};
    if (localtime_r(&secs, &tb) == nullptr)
        report::throw_unrepresentable_time("equity_curve.time_ms");
    return (static_cast<int64_t>(tb.tm_year) + 1900) * 12 + tb.tm_mon;
}

// Sharpe/Sortino over a generated simple-returns series. The generator is
// called twice: once for the plain left-fold mean/count, then again for the
// variance/downside fold. This preserves the old vector walk's operation order
// without retaining either the month-end values or the returns.
template <class Generate>
void sharpe_sortino_generated(Generate generate, double rf_period, double ann,
                              double* sharpe, double* sortino,
                              const char* sharpe_field, const char* sortino_field) {
    *sharpe = kNaN;
    *sortino = kNaN;
    std::size_t n = 0;
    double sum = 0.0;
    const char* sum_failure = nullptr;
    generate([&](double value) {
        // Main leaves a series shorter than two undefined. Remember each
        // return/sum check until that count is known, without retaining rows.
        fold_detail::defer_nonfinite(value, sharpe_field, sum_failure);
        sum = fold_detail::defer_nonfinite(sum + value, sharpe_field, sum_failure);
        ++n;
    });
    if (n < 2) return;
    report::finite_value(rf_period, sharpe_field);
    report::finite_value(ann, sharpe_field);
    if (sum_failure) report::finite_value(kNaN, sum_failure);
    report::finite_value(sum, sharpe_field);
    const double mean = report::finite_value(sum / (double)n, sharpe_field);
    double var = 0.0, down = 0.0;
    generate([&](double value) {
        report::finite_value(value, sharpe_field);
        const double delta = report::finite_value(value - mean, sharpe_field);
        var = report::finite_value(var + delta * delta, sharpe_field);
        const double d = std::min(0.0, report::finite_value(value - rf_period, sortino_field));
        down = report::finite_value(down + d * d, sortino_field);
    });
    const double sd = report::finite_value(std::sqrt(var / (double)(n - 1)), sharpe_field);
    const double dd = report::finite_value(std::sqrt(down / (double)n), sortino_field);
    if (sd > 0.0)
        *sharpe = report::finite_value((mean - rf_period) / sd * ann, sharpe_field);
    if (dd > 0.0)
        *sortino = report::finite_value((mean - rf_period) / dd * ann, sortino_field);
}

// Emit the exact return sequence produced by the former month_end vector:
// close each civil bucket, compare it with the preceding closed bucket, and
// skip a return whose preceding equity is not positive. The first bucket only
// seeds the preceding value, so a one-bucket curve emits no return.
template <class PointSource, class KeyFn, class Emit>
void for_each_month_return(const PointSource& curve, std::int64_t n,
                           KeyFn key_fn, Emit emit) {
    if (n <= 0) return;

    const pf_equity_point_t first = curve[0];
    std::int64_t current_key = key_fn(first.time_ms);
    double current_month_end = first.equity;
    double previous_month_end = 0.0;
    bool have_previous_month = false;

    for (std::int64_t i = 1; i < n; ++i) {
        const pf_equity_point_t point = curve[i];
        const std::int64_t key = key_fn(point.time_ms);
        if (key != current_key) {
            if (have_previous_month && previous_month_end > 0.0)
                emit(current_month_end / previous_month_end - 1.0);
            previous_month_end = current_month_end;
            have_previous_month = true;
            current_key = key;
        }
        current_month_end = point.equity;
    }

    if (have_previous_month && previous_month_end > 0.0)
        emit(current_month_end / previous_month_end - 1.0);
}

// Emit the exact per-bar return sequence from the former temporary vector.
template <class PointSource, class Emit>
void for_each_bar_return(const PointSource& curve, std::int64_t n, Emit emit) {
    if (n <= 1) return;
    double previous_equity = curve[0].equity;
    for (std::int64_t i = 1; i < n; ++i) {
        const pf_equity_point_t point = curve[i];
        if (previous_equity > 0.0)
            emit(point.equity / previous_equity - 1.0);
        previous_equity = point.equity;
    }
}

// Streaming-ratio arithmetic is report-only. No strategy callback, floating
// environment change, shared state, or regrouped downside sum enters it.
// Return bytes are copied through the run-owned positional scratch stream.
namespace streaming_v1 {
constexpr const char* sf = "metrics.equity.sharpe_bar";
constexpr const char* df = "metrics.equity.sortino_bar";

double years(const MetricsExtentV1& extent) {
    if (extent.empty) return 0.0;
    const auto first = extent.first_time_ms, last = extent.last_time_ms;
    const double span = last >= first
        ? static_cast<double>(static_cast<std::uint64_t>(last)
                              - static_cast<std::uint64_t>(first))
        : -static_cast<double>(static_cast<std::uint64_t>(first)
                               - static_cast<std::uint64_t>(last));
    return span / (365.25 * 86400.0 * 1000.0);
}
bool operands(const MetricsExtentV1& extent, double& rf, double& ann) {
    const double span_years = years(extent);
    if (extent.count < 3 || !(span_years > 0.0)) return false;
    const double bars_per_year = static_cast<double>(extent.count - 1) / span_years;
    rf = 0.02 / bars_per_year;
    ann = std::sqrt(bars_per_year);
    return true;
}
void trace(MetricsRatioTraceSinkV1 sink, std::int64_t i, const char* operation,
           const char* field, bool failed = false, const char* stage = "bar") {
    if (sink.emit)
        sink.emit(sink.context, {stage, i, operation,
            failed ? "report_result_invalid" : nullptr, field,
            failed ? "nonfinite" : nullptr});
}

// Exact fallback, including the old interleaving and promotion order.
// Only our local arithmetic sentinel is caught; provider/I/O exceptions keep
// their existing type, code and args.
struct Nonfinite {};
template<class Generate>
void legacy(Generate generate, const MetricsExtentV1& extent,
            MetricsStreamingResultV1& out, MetricsRatioTraceSinkV1 sink) {
    std::uint64_t m = 0;
    double sum = 0.0, rf = kNaN, ann = kNaN;
    std::int64_t deferred = -1;
    const char* deferred_operation = nullptr;
    generate([&](double r) {
        if (!std::isfinite(r) && deferred < 0) {
            deferred = static_cast<std::int64_t>(m);
            deferred_operation = "promote.return.first_pass";
        }
        trace(sink, static_cast<std::int64_t>(m), "return.first_pass", sf,
              false, "bar.first_pass");
        sum += r;
        if (!std::isfinite(sum) && deferred < 0) {
            deferred = static_cast<std::int64_t>(m);
            deferred_operation = "promote.sum";
        }
        trace(sink, static_cast<std::int64_t>(m), "sum", sf, false, "bar.first_pass");
        ++m;
    });
    out.receipt.eligible_returns = m;
    if (!operands(extent, rf, ann) || m < 2) return;
    const auto check = [&](double value, std::int64_t i,
                           const char* operation, const char* field) {
        const bool bad = !std::isfinite(value);
        trace(sink, i, operation, field, bad);
        if (bad) {
            out.failure_code = "report_result_invalid";
            out.failure_field = field;
            out.failure_reason = "nonfinite";
            throw Nonfinite{};
        }
        return value;
    };
    try {
        check(rf, -1, "rf", sf);
        check(ann, -1, "annualizer", sf);
        if (deferred >= 0) check(kNaN, deferred, deferred_operation, sf);
        check(sum, -1, "sum.final", sf);
        const double mean = check(sum / static_cast<double>(m), -1, "mean", sf);
        std::int64_t i = 0;
        generate([&](double r) {
            check(r, i, "return.second_pass", sf);
            const double delta = check(r - mean, i, "delta", sf);
            out.centered_sum = check(out.centered_sum + delta * delta,
                                     i, "variance.add_square", sf);
            const double d = std::min(0.0, check(r - rf, i, "downside.subtract", df));
            out.downside_sum = check(out.downside_sum + d * d,
                                     i, "downside.add_square", df);
            ++i;
        });
        const double sd = check(std::sqrt(out.centered_sum / static_cast<double>(m - 1)),
                                -1, "sample.sd", sf);
        const double dd = check(std::sqrt(out.downside_sum / static_cast<double>(m)),
                                -1, "downside.sd", df);
        if (sd > 0.0)
            out.sharpe_bar = check((mean - rf) / sd * ann, -1, "sharpe", sf);
        if (dd > 0.0)
            out.sortino_bar = check((mean - rf) / dd * ann, -1, "sortino", df);
    } catch (const Nonfinite&) {
    }
}

// The metric route stores only the binary64 return values. One pending batch
// is both the append buffer and the readback window; no return cache or
// numerical certificate is retained in the fold state.
class PackedReturnStream {
public:
    static constexpr std::size_t kBatchDoubles =
        report_sink::kScratchWindowBytes / sizeof(double);
    static_assert(sizeof(double) == 8, "packed return stream requires binary64");
    static_assert(kBatchDoubles == 8192, "return stream batch is one 64 KiB window");

    PackedReturnStream()
        : file_("", ::pineforge::detail::ScratchStore::curve) {}
    PackedReturnStream(const PackedReturnStream&) = delete;
    PackedReturnStream& operator=(const PackedReturnStream&) = delete;

    void append(double value) {
        constexpr auto max_doubles = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() / sizeof(double));
        if (count_ >= max_doubles)
            throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                "ordered return scratch offset exhausted");
        pending_[pending_count_++] = value;
        ++count_;
        if (pending_count_ == pending_.size()) flush();
    }

    template<class Emit>
    void replay(Emit emit) const {
        auto& stream = const_cast<PackedReturnStream&>(*this);
        stream.flush();
        if (stream.flushed_ != stream.count_)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                "ordered return scratch extent differs from its append count");
        std::uint64_t first = 0;
        while (first < stream.count_) {
            const auto remaining = stream.count_ - first;
            const auto batch = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, kBatchDoubles));
            stream.file_.read(stream.byte_offset(first), stream.pending_.data(),
                              batch * sizeof(double));
            for (std::size_t i = 0; i < batch; ++i)
                emit(stream.pending_[i]);
            first += batch;
        }
    }

    std::uint64_t count() const noexcept { return count_; }

private:
    static std::int64_t byte_offset(std::uint64_t first) {
        constexpr auto max_doubles = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() / sizeof(double));
        if (first > max_doubles)
            throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                "ordered return scratch offset exhausted");
        return static_cast<std::int64_t>(first * sizeof(double));
    }

    void flush() {
        if (pending_count_ == 0) return;
        file_.write(byte_offset(flushed_), pending_.data(),
                    pending_count_ * sizeof(double));
        flushed_ += pending_count_;
        pending_count_ = 0;
    }

    mutable report_sink::ScratchFile file_;
    mutable std::array<double, kBatchDoubles> pending_{};
    std::uint64_t count_ = 0;
    std::uint64_t flushed_ = 0;
    mutable std::size_t pending_count_ = 0;
};

struct Prefix {
    MetricsExtentV1 extent;
    std::uint64_t m = 0;
    double previous_equity = 0.0;
    std::uint64_t point_witness = 1469598103934665603ULL;
    PackedReturnStream returns;

    void fold(const pf_equity_point_t& point, int month) {
        if (extent.empty) {
            extent.empty = 0;
            extent.first_time_ms = point.time_ms;
        } else if (previous_equity > 0.0) {
            // This is deliberately the same expression and eligibility test
            // as for_each_bar_return. The bytes are the legacy r_i values.
            const double r = point.equity / previous_equity - 1.0;
            returns.append(r);
            ++m;
        }
        ++extent.count;
        extent.last_time_ms = point.time_ms;
        previous_equity = point.equity;
        fold_detail::witness_word(point_witness, static_cast<std::uint64_t>(point.time_ms));
        fold_detail::witness_double(point_witness, point.equity);
        fold_detail::witness_double(point_witness, point.open_profit);
        fold_detail::witness_word(point_witness, static_cast<std::uint64_t>(month));
    }

    template<class Emit>
    void generate(Emit emit) const {
        returns.replay(emit);
    }
};

class Fold final : public StreamingEquityRatiosV1 {
public:
    Fold(MetricsStreamingConfigV1 config, MetricsExtentPlanV1 plan,
         MetricsRatioTraceSinkV1 sink)
        : config_(config), plan_(plan), sink_(sink) {}
    MetricsStreamingStatusV1 append(const pf_equity_point_t& point, int month) override {
        if (have_held_) prefix_.fold(held_, held_month_);
        held_ = point;
        held_month_ = month;
        have_held_ = true;
        return MetricsStreamingStatusV1::Ok;
    }
    MetricsStreamingStatusV1 amend_held(
            std::int64_t index, const pf_equity_point_t& point, int month) override {
        // Deliberately not assert(): this invariant also binds Release.
        if (!have_held_ || index != prefix_.extent.count) std::abort();
        held_ = point;
        held_month_ = month;
        return MetricsStreamingStatusV1::Ok;
    }
    std::size_t state_size_bytes() const override { return sizeof(*this); }
    MetricsStreamingStatusV1 finish(
            MetricsStreamingResultV1& out, MetricsRatioCurveV1 curve) const override {
        auto actual = prefix_.extent;
        if (have_held_) {
            if (actual.empty) {
                actual.empty = 0;
                actual.first_time_ms = held_.time_ms;
            }
            ++actual.count;
            actual.last_time_ms = held_.time_ms;
        }
        double tail_return = 0.0;
        const bool tail_eligible = have_held_ && !prefix_.extent.empty
            && prefix_.previous_equity > 0.0;
        if (tail_eligible)
            tail_return = held_.equity / prefix_.previous_equity - 1.0;
        const std::uint64_t total_returns = prefix_.m
            + (tail_eligible ? 1U : 0U);
        const auto generate = [this, tail_eligible, tail_return](auto emit) {
            prefix_.generate(emit);
            if (tail_eligible) emit(tail_return);
        };
        out = {};
        auto& receipt = out.receipt;
        receipt.extent_plan = plan_;
        receipt.actual_extent = actual;
        receipt.actual_attempt = config_.attempt;
        receipt.actual_input_generation = config_.input_generation;
        receipt.actual_admitted_view = config_.admitted_view;
        receipt.actual_spec_identity = config_.spec_identity;
        receipt.authoritative_attempt = config_.attempt;
        receipt.eligible_returns = total_returns;
        receipt.metrics_needed = config_.metrics_needed;
        receipt.streaming_sharpe = config_.streaming_sharpe;
        receipt.mismatch_fields = fold_detail::extent_mismatch_v1(plan_, config_, actual);
        receipt.witness = !plan_.present ? MetricsExtentWitnessV1::NotEvaluated
            : receipt.mismatch_fields ? MetricsExtentWitnessV1::Mismatch
                                      : MetricsExtentWitnessV1::Match;
        const auto need = config_.metrics_needed;
        const bool sharpe = (need & kMetricsNeedSharpeBarV1) != 0;
        const bool sortino = (need & kMetricsNeedSortinoBarV1) != 0;
        auto reason = MetricsRouteReasonV1::None;
        if (need & kMetricsNeedCurveV1) reason = MetricsRouteReasonV1::CurveNeeded;
        else if (need & kMetricsNeedReturnsV1) reason = MetricsRouteReasonV1::ReturnsNeeded;
        else if (need & kMetricsNeedMonthlyRatiosV1) reason = MetricsRouteReasonV1::CurveNeeded;
        else if (sharpe && config_.streaming_sharpe == StreamingSharpeV1::Off)
            reason = MetricsRouteReasonV1::SharpeSwitchOff;
        else if (sortino && !plan_.present) reason = MetricsRouteReasonV1::NoExtentProof;
        else if (sortino && receipt.mismatch_fields) reason = MetricsRouteReasonV1::ExtentMismatch;
        else if (sortino && config_.deterministic_replay != 1)
            reason = MetricsRouteReasonV1::NoRecoveryProof;

        double rf = kNaN, ann = kNaN;
        const bool enabled = operands(actual, rf, ann) && total_returns >= 2;
        if (reason == MetricsRouteReasonV1::None && enabled && (sharpe || sortino)) {
            // The packed stream is the authoritative source. `legacy` makes
            // the same two sequential passes and the same left-fold
            // operations as the former vector generator.
            legacy(generate, actual, out, sink_);
        }
        if (reason == MetricsRouteReasonV1::None) {
            receipt.route = MetricsRatioRouteV1::Streaming;
            receipt.route_reason = MetricsRouteReasonV1::OrderedReplay;
            return MetricsStreamingStatusV1::Ok;
        }
        // Drop ALL speculative arithmetic, not just planned downside.
        out.sharpe_bar = out.sortino_bar = kNaN;
        out.centered_sum = out.downside_sum = 0.0;
        receipt.route_reason = reason;
        if (!config_.curve_available) {
            receipt.route = MetricsRatioRouteV1::LegacyRerun;
            // This is a request for the host's one authoritative replay,
            // not a claim that this pure fold executed strategy callbacks.
            receipt.replay_attempts = config_.deterministic_replay == 1 ? 1u : 0u;
            receipt.authoritative_attempt = 0; // supplied by the resolving host
            return MetricsStreamingStatusV1::Ok;
        }
        receipt.route = MetricsRatioRouteV1::LegacyActualCurve;
        if (curve.read && curve.count >= 0) {
            auto& actual = receipt.actual_extent;
            actual = {};
            actual.count = curve.count;
            actual.empty = curve.count == 0;
            if (curve.count > 0) {
                actual.first_time_ms = curve.read(curve.context, 0).time_ms;
                actual.last_time_ms = curve.read(curve.context, curve.count - 1).time_ms;
            }
            receipt.mismatch_fields = fold_detail::extent_mismatch_v1(plan_, config_, actual);
            receipt.witness = !plan_.present ? MetricsExtentWitnessV1::NotEvaluated
                : receipt.mismatch_fields ? MetricsExtentWitnessV1::Mismatch
                                          : MetricsExtentWitnessV1::Match;
            const auto generate = [&](auto emit) {
                if (curve.count <= 1) return;
                double previous = curve.read(curve.context, 0).equity;
                for (std::int64_t i = 1; i < curve.count; ++i) {
                    const double equity = curve.read(curve.context, i).equity;
                    if (previous > 0.0) emit(equity / previous - 1.0);
                    previous = equity;
                }
            };
            legacy(generate, actual, out, sink_);
        } else if (!(need & (kMetricsNeedCurveV1 | kMetricsNeedReturnsV1
                             | kMetricsNeedMonthlyRatiosV1))) {
            // The ordered stream is also an exact provider for a forced
            // legacy bar-ratio route when no equity curve was supplied.
            legacy(generate, actual, out, sink_);
        } else {
            // A caller promising a curve must supply its checked provider
            // when a curve/returns/monthly result was requested.
            return MetricsStreamingStatusV1::InvalidArgument;
        }
        return MetricsStreamingStatusV1::Ok;
    }
private:
    MetricsStreamingConfigV1 config_;
    MetricsExtentPlanV1 plan_;
    MetricsRatioTraceSinkV1 sink_;
    Prefix prefix_;
    pf_equity_point_t held_{};
    int held_month_ = 0;
    bool have_held_ = false;
};
static_assert(sizeof(Fold) <= report_sink::kScratchWindowBytes + 4096,
              "streaming-ratio state is one bounded scratch batch plus scalars");
} // namespace streaming_v1

}  // namespace

MetricsStreamingStatusV1 make_streaming_equity_ratios_v1(
        const MetricsStreamingConfigV1& config, const MetricsExtentPlanV1& plan,
        MetricsRatioTraceSinkV1 sink, std::unique_ptr<StreamingEquityRatiosV1>& out) {
    if ((config.streaming_sharpe != StreamingSharpeV1::Off
         && config.streaming_sharpe != StreamingSharpeV1::On)
        || (config.metrics_needed & ~kMetricsNeedAllV1))
        return MetricsStreamingStatusV1::InvalidArgument;
    out = std::make_unique<streaming_v1::Fold>(config, plan, sink);
    return MetricsStreamingStatusV1::Ok;
}

namespace detail {

// UTC civil-month key for the point's whole seconds, truncated toward zero.
// Every int64 millisecond stamp has a floor day within +/-106751991168:
// native_civil_date's intermediate arithmetic and year * 12 fit int64 here.
// External linkage lets the arithmetic test reach this internal helper.
int64_t month_key_utc(int64_t ts_ms) {
    const int64_t secs = ts_ms / 1000;
    const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
    const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(days);
    return date.year * 12 + date.month - 1;
}

// The walk below keys every equity point, and consecutive points
// share their month for weeks of bars, so the key is kept with the seconds of
// its month. The range is the civil month of the second's floor day,
// clamped to the cache span; it is kept only once both of its
// ends answer the key, and the key never decreases as the second increases (it
// is the civil order), so every second between the ends answers it too. A
// second outside the cache span is computed directly and is never kept.
int64_t keep_utc_month(UtcMonthMemo& memo, int64_t ts_ms) {
    const int64_t secs = ts_ms / 1000;
    const int64_t key = month_key_utc(ts_ms);
    constexpr int64_t kCivilSpan = int64_t{1} << 40;
    if (secs > -kCivilSpan && secs < kCivilSpan) {
        const int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
        // The civil month of the floor day, and the first day of the next.
        const native_calendar::NativeCivilDate date = native_calendar::native_civil_date(days);
        const int64_t first = native_calendar::native_civil_days(date.year, date.month, 1);
        const int64_t next = date.month < 12
            ? native_calendar::native_civil_days(date.year, date.month + 1, 1)
            : native_calendar::native_civil_days(date.year + 1, 1, 1);
        const int64_t lo = std::max(first * 86400, -kCivilSpan + 1);
        const int64_t hi = std::min(next * 86400, kCivilSpan);
        // Both probes are strictly inside +/-2^40 seconds, so * 1000 is safe.
        if (month_key_utc(lo * 1000) == key && month_key_utc((hi - 1) * 1000) == key) {
            memo.lo = lo;
            memo.hi = hi;
            memo.key = key;
        }
    }
    return key;
}

}  // namespace detail

std::int64_t equity_month_key(std::int64_t time_ms, const std::string& chart_timezone) {
    if (chart_timezone.empty() || chart_timezone == "UTC" || chart_timezone == "Etc/UTC")
        return detail::month_key_utc(time_ms);
    // This scope contains only the civil-month lookup, never engine dispatch.
    tz_util::ScopedTimezone guard(chart_timezone);
    return month_key_local(time_ms);
}

pf_trade_stats_t compute_trade_stats(const TradeC* trades, int n,
                                     TradeFilter filter, double initial_capital) {
    TradeStatsAccumulator accumulator(filter, initial_capital);
    for (int i = 0; i < n; ++i) accumulator.fold(trades[i]);
    return accumulator.finish();
}

// Report-only state, independent of broker decision counters. These are the
// old array loop's locals, retained across fold calls without regrouping sums.
struct TradeStatsAccumulator::State {
    TradeFilter filter;
    double initial_capital;
    pf_trade_stats_t stats{};
    double pct_sum = 0.0, win_pct_sum = 0.0, loss_pct_sum = 0.0;
    double bars_sum = 0.0, win_bars_sum = 0.0, loss_bars_sum = 0.0;
    std::int32_t streak_w = 0, streak_l = 0;
    double largest_win = 0.0, largest_win_pct = 0.0;
    double largest_loss = 0.0, largest_loss_pct = 0.0;  // magnitudes
    const char* nonfinite_field = nullptr;
    const char* nonfinite_prefix = nullptr;

    State(TradeFilter selected_filter, double capital)
        : filter(selected_filter), initial_capital(capital) {
        fold_check(initial_capital, "net_profit_pct");
    }

    const char* prefix() const {
        return filter == TradeFilter::ALL ? "metrics.all" :
               filter == TradeFilter::LONG ? "metrics.longs" : "metrics.shorts";
    }

    double check(double value, const char* field) const {
        return report::finite_value(value, field, prefix());
    }

    double defer(double value, const char* field, const char* field_prefix) {
        if (!std::isfinite(value) && nonfinite_field == nullptr) {
            nonfinite_field = field;
            nonfinite_prefix = field_prefix;
        }
        return value;
    }

    double fold_check(double value, const char* field) {
        return defer(value, field, prefix());
    }

    void fold(const TradeC& t) {
        if (!match(t, filter)) return;
        pf_trade_stats_t& s = stats;
        // Independent filters can be fed together, but the report finishes
        // ALL before LONG before SHORT. Retain each walk's first failure so
        // an eager LONG failure cannot overtake a later ALL failure.
#define PF_DEFER_TRADE(field) (void)defer(t.field, "trades." #field, nullptr)
        PF_DEFER_TRADE(entry_price);
        PF_DEFER_TRADE(exit_price);
        PF_DEFER_TRADE(qty);
        PF_DEFER_TRADE(pnl);
        PF_DEFER_TRADE(pnl_pct);
        PF_DEFER_TRADE(max_runup);
        PF_DEFER_TRADE(max_drawdown);
        PF_DEFER_TRADE(commission);
#undef PF_DEFER_TRADE
        ++s.num_trades;
        s.net_profit = fold_check(s.net_profit + t.pnl, "net_profit");
        s.commission_paid = fold_check(s.commission_paid + t.commission, "commission_paid");
        pct_sum = fold_check(pct_sum + t.pnl_pct, "avg_trade_pct");
        // TV counts bars-in-trade inclusively of the entry bar (TV=9 vs the
        // exclusive diff 8, all blocks; validated 2026-06-12 vs TV export).
        const double bars = (double)(static_cast<int64_t>(t.exit_bar_index)
                                     - static_cast<int64_t>(t.entry_bar_index)) + 1.0;
        bars_sum = fold_check(bars_sum + bars, "avg_bars_in_trade");
        if (t.pnl > 0.0) {
            ++s.num_wins;
            s.gross_profit = fold_check(s.gross_profit + t.pnl, "gross_profit");
            win_pct_sum = fold_check(win_pct_sum + t.pnl_pct, "avg_win_pct");
            win_bars_sum = fold_check(win_bars_sum + bars, "avg_bars_in_wins");
            streak_l = 0;
            if (++streak_w > s.max_consecutive_wins) s.max_consecutive_wins = streak_w;
            // TV "Largest profit %" is the independent max of per-trade %,
            // NOT the % of the largest-USD win (validated 2026-06-12 vs TV
            // export). Track the two maxima separately.
            if (t.pnl > largest_win) largest_win = t.pnl;
            if (t.pnl_pct > largest_win_pct) largest_win_pct = t.pnl_pct;
        } else if (t.pnl < 0.0) {
            ++s.num_losses;
            s.gross_loss = fold_check(s.gross_loss + -t.pnl, "gross_loss");  // positive magnitude
            loss_pct_sum = fold_check(loss_pct_sum + -t.pnl_pct, "avg_loss_pct");
            loss_bars_sum = fold_check(loss_bars_sum + bars, "avg_bars_in_losses");
            streak_w = 0;
            if (++streak_l > s.max_consecutive_losses) s.max_consecutive_losses = streak_l;
            // Independent maxima, mirroring the win side. pnl_pct is net
            // return-on-cost (sign == pnl sign), so -pnl_pct is a true
            // positive magnitude for losses.
            if (-t.pnl > largest_loss) largest_loss = -t.pnl;
            if (-t.pnl_pct > largest_loss_pct) largest_loss_pct = -t.pnl_pct;
        } else {
            ++s.num_even;                      // even trades break both streaks
            streak_w = 0;
            streak_l = 0;
        }
    }

    pf_trade_stats_t finish() const {
        // Finalize a value copy: a subsequent fold, copy or report read must
        // still see the unfinalized prefix and its original left-fold sums.
        if (nonfinite_field)
            report::finite_value(kNaN, nonfinite_field, nonfinite_prefix);
        pf_trade_stats_t s = stats;
        const bool cap_ok = initial_capital > 0.0;
        s.net_profit_pct   = cap_ok ? check(s.net_profit   / initial_capital * 100.0, "net_profit_pct") : kNaN;
        s.gross_profit_pct = cap_ok ? check(s.gross_profit / initial_capital * 100.0, "gross_profit_pct") : kNaN;
        s.gross_loss_pct   = cap_ok ? check(s.gross_loss   / initial_capital * 100.0, "gross_loss_pct") : kNaN;
        s.percent_profitable = s.num_trades > 0
            ? check((double)s.num_wins / (double)s.num_trades * 100.0, "percent_profitable") : kNaN;
        s.profit_factor = s.gross_loss > 0.0 ? check(s.gross_profit / s.gross_loss, "profit_factor") : kNaN;
        s.avg_trade     = s.num_trades > 0 ? check(s.net_profit / s.num_trades, "avg_trade") : kNaN;
        s.avg_trade_pct = s.num_trades > 0 ? check(pct_sum / s.num_trades, "avg_trade_pct") : kNaN;
        s.avg_win       = s.num_wins   > 0 ? check(s.gross_profit / s.num_wins, "avg_win") : kNaN;
        s.avg_win_pct   = s.num_wins   > 0 ? check(win_pct_sum / s.num_wins, "avg_win_pct") : kNaN;
        s.avg_loss      = s.num_losses > 0 ? check(s.gross_loss / s.num_losses, "avg_loss") : kNaN;
        s.avg_loss_pct  = s.num_losses > 0 ? check(loss_pct_sum / s.num_losses, "avg_loss_pct") : kNaN;
        s.ratio_avg_win_avg_loss =
            (s.num_wins > 0 && s.num_losses > 0 && s.avg_loss > 0.0)
                ? check(s.avg_win / s.avg_loss, "ratio_avg_win_avg_loss") : kNaN;
        s.largest_win      = s.num_wins   > 0 ? largest_win      : kNaN;
        s.largest_win_pct  = s.num_wins   > 0 ? largest_win_pct  : kNaN;
        s.largest_loss     = s.num_losses > 0 ? largest_loss     : kNaN;
        s.largest_loss_pct = s.num_losses > 0 ? largest_loss_pct : kNaN;
        s.expectancy = s.num_trades > 0
            ? check(((double)s.num_wins / s.num_trades) * (s.num_wins > 0 ? s.avg_win : 0.0)
              - ((double)s.num_losses / s.num_trades) * (s.num_losses > 0 ? s.avg_loss : 0.0), "expectancy")
            : kNaN;
        s.avg_bars_in_trade  = s.num_trades > 0 ? check(bars_sum / s.num_trades, "avg_bars_in_trade") : kNaN;
        s.avg_bars_in_wins   = s.num_wins   > 0 ? check(win_bars_sum / s.num_wins, "avg_bars_in_wins") : kNaN;
        s.avg_bars_in_losses = s.num_losses > 0 ? check(loss_bars_sum / s.num_losses, "avg_bars_in_losses") : kNaN;
        return s;
    }
};

TradeStatsAccumulator::TradeStatsAccumulator(TradeFilter filter, double initial_capital)
    : state_(std::make_unique<State>(filter, initial_capital)) {}

TradeStatsAccumulator::~TradeStatsAccumulator() = default;

TradeStatsAccumulator::TradeStatsAccumulator(const TradeStatsAccumulator& other)
    : state_(std::make_unique<State>(*other.state_)) {}

TradeStatsAccumulator& TradeStatsAccumulator::operator=(const TradeStatsAccumulator& other) {
    if (this != &other) *state_ = *other.state_;
    return *this;
}

void TradeStatsAccumulator::fold(const TradeC& trade) {
    state_->fold(trade);
}

pf_trade_stats_t TradeStatsAccumulator::finish() const {
    return state_->finish();
}

pf_equity_stats_t finalize_equity_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const EquityRatioResult* ratios) {
    const auto check = [](double value, const char* field) {
        return report::finite_value(value, field, "metrics.equity");
    };
    check(inputs.initial_capital, "buy_hold_return");
    report::finite_value(inputs.net_profit, "summary.net_pnl");
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    if (std::isinf(inputs.first_open)) check(inputs.first_open, "buy_hold_return_pct");
    if (std::isinf(inputs.last_close)) check(inputs.last_close, "buy_hold_return_pct");
    if (std::isfinite(inputs.first_open) && inputs.first_open > 0.0
        && std::isfinite(inputs.last_close)) {
        e.buy_hold_return_pct = check(
            (inputs.last_close / inputs.first_open - 1.0) * 100.0, "buy_hold_return_pct");
        e.buy_hold_return = check(
            inputs.initial_capital * (inputs.last_close / inputs.first_open - 1.0), "buy_hold_return");
    }
    if (first.point_count <= 0) return e;

    // Match the array producer's order on the COMPLETED curve. In particular,
    // a nonfinite final open profit precedes an earlier invalid equity point.
    // Prefix/replay walks only remember their first invalid field; no typed
    // exception is caught, converted to a sentinel, or lost on this path.
    e.open_pl = report::finite_value(first.last.open_profit, "equity_curve.open_profit");
    e.time_in_market_pct = first.observation_count > 0
        ? check(static_cast<double>(first.bars_in_market)
                / static_cast<double>(first.observation_count) * 100.0, "time_in_market_pct") : kNaN;
    if (first.nonfinite_field)
        report::finite_value(kNaN, first.nonfinite_field);
    e.max_equity_drawdown = first.max_drawdown;
    e.max_equity_drawdown_pct = first.max_drawdown_pct;
    e.max_equity_runup = first.max_runup;
    e.max_equity_runup_pct = first.max_runup_pct;

    const double span_ms = first.span_ms();
    const double span_years = check(first.span_years(), "cagr");
    if (span_years > 0.0 && inputs.initial_capital > 0.0 && first.last.equity > 0.0) {
        const double ratio = check(first.last.equity / inputs.initial_capital, "cagr");
        if (span_ms < static_cast<double>(detail::kMinAnnualizationSpanMs)) {
            e.cagr = check((ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        } else {
            const double compounded = (std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0;
            e.cagr = check(std::isfinite(compounded) ? compounded
                           : (ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        }
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = check(inputs.net_profit / e.max_equity_drawdown, "recovery_factor");
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = check(e.cagr / e.max_equity_drawdown_pct, "calmar");
    }
    if (ratios && ratios->matches(first)) {
        if (ratios->nonfinite_field)
            report::finite_value(kNaN, ratios->nonfinite_field);
        e.sharpe_monthly = ratios->sharpe_monthly;
        e.sortino_monthly = ratios->sortino_monthly;
        e.sharpe_bar = ratios->sharpe_bar;
        e.sortino_bar = ratios->sortino_bar;
    }
    return e;
}

template <class PointSource>
pf_equity_stats_t compute_equity_stats(const PointSource& curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit) {
    return compute_equity_stats<PointSource>(
        curve, n, initial_capital, chart_tz, first_open, last_close,
        bars_in_market, net_profit, n);
}

template <class PointSource>
pf_equity_stats_t compute_equity_stats(const PointSource& curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count) {
    const auto check = [](double value, const char* field) {
        return report::finite_value(value, field, "metrics.equity");
    };
    check(initial_capital, "buy_hold_return");
    report::finite_value(net_profit, "summary.net_pnl");
    pf_equity_stats_t e{};
    e.sharpe_monthly = kNaN; e.sortino_monthly = kNaN; e.sharpe_bar = kNaN; e.sortino_bar = kNaN;
    e.cagr = kNaN; e.calmar = kNaN; e.recovery_factor = kNaN;
    e.buy_hold_return = kNaN; e.buy_hold_return_pct = kNaN;
    e.time_in_market_pct = kNaN; e.open_pl = 0.0;

    // NaN is the existing missing-price sentinel. Infinity is never missing.
    if (std::isinf(first_open)) check(first_open, "buy_hold_return_pct");
    if (std::isinf(last_close)) check(last_close, "buy_hold_return_pct");
    if (std::isfinite(first_open) && first_open > 0.0 && std::isfinite(last_close)) {
        e.buy_hold_return_pct = check((last_close / first_open - 1.0) * 100.0, "buy_hold_return_pct");
        e.buy_hold_return = check(initial_capital * (last_close / first_open - 1.0), "buy_hold_return");
    }
    if (n <= 0) return e;

    e.open_pl = report::finite_value(curve[n - 1].open_profit, "equity_curve.open_profit");
    // Exposure is over observations, not curve points: a selected-window curve
    // carries its window anchor as point 0 (metrics.hpp), and the anchor is no
    // bar. Every other statistic below walks the n curve points.
    e.time_in_market_pct = observation_count > 0
        ? check((double)bars_in_market / (double)observation_count * 100.0, "time_in_market_pct") : kNaN;

    // --- Drawdown / runup walk. MUST mirror update_equity_extremes
    // (engine.hpp): trough resets to eq on every new peak. The walk over the
    // curve reproduces the engine's scalar extremes exactly (verified by the
    // engine-vs-walk integration test in test_metrics.cpp). Percent vs the
    // peak (dd) / trough (runup) in effect when the extreme was hit.
    double peak = curve[0].equity, trough = curve[0].equity;
    for (int64_t i = 0; i < n; ++i) {
        const double eq = report::finite_value(curve[i].equity, "equity_curve.equity");
        report::finite_value(curve[i].open_profit, "equity_curve.open_profit");
        if (eq > peak) { peak = eq; trough = eq; }
        if (eq < trough) trough = eq;
        const double dd = check(peak - eq, "max_equity_drawdown");
        if (dd > e.max_equity_drawdown) {
            e.max_equity_drawdown = dd;
            e.max_equity_drawdown_pct = peak > 0.0 ? check(dd / peak * 100.0, "max_equity_drawdown_pct") : kNaN;
        }
        const double ru = check(eq - trough, "max_equity_runup");
        if (ru > e.max_equity_runup) {
            e.max_equity_runup = ru;
            e.max_equity_runup_pct = trough > 0.0 ? check(ru / trough * 100.0, "max_equity_runup_pct") : kNaN;
        }
    }

    // --- Measured curve span on the UTC epoch-ms bar clock (ms -> years) ---
    // Unsigned differences avoid signed overflow even for the full int64
    // timestamp domain. For a representable old difference the conversion
    // and the floating-point operation order are unchanged.
    const int64_t first_time = curve[0].time_ms, last_time = curve[n - 1].time_ms;
    const double span_ms = last_time >= first_time
        ? (double)(static_cast<uint64_t>(last_time) - static_cast<uint64_t>(first_time))
        : -(double)(static_cast<uint64_t>(first_time) - static_cast<uint64_t>(last_time));
    const double span_years = check(span_ms / (365.25 * 86400.0 * 1000.0), "cagr");

    if (span_years > 0.0 && initial_capital > 0.0 && curve[n - 1].equity > 0.0) {
        const double ratio = check(curve[n - 1].equity / initial_capital, "cagr");
        if (span_ms < static_cast<double>(detail::kMinAnnualizationSpanMs)) {
            e.cagr = check((ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        } else {
            const double compounded = (std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0;
            // A nonfinite compounded value alone permits the proportional
            // fallback. The simple/compounded switch can cause a rare ranking
            // inversion across the one-day boundary (compounded > simple).
            e.cagr = check(std::isfinite(compounded) ? compounded
                           : (ratio - 1.0) * (1.0 / span_years) * 100.0, "cagr");
        }
    }
    if (e.max_equity_drawdown > 0.0) {
        e.recovery_factor = check(net_profit / e.max_equity_drawdown, "recovery_factor");
        if (!std::isnan(e.cagr) && e.max_equity_drawdown_pct > 0.0)
            e.calmar = check(e.cagr / e.max_equity_drawdown_pct, "calmar");
    }

    // --- Monthly-resampled Sharpe/Sortino (sharpe_monthly / sortino_monthly,
    // the TV-calibrated construction): last point of each chart-tz
    // (year,month) bucket; simple returns between consecutive month-ends.
    // A UTC chart decomposes only at month boundaries (the key is kept with
    // its month's seconds, detail::month_key_utc); a chart timezone still
    // calls localtime_r per point under the one guard.
    {
        const bool utc = chart_tz.empty() || chart_tz == "UTC" || chart_tz == "Etc/UTC";
        if (utc) {
            const auto month_returns = [&](auto emit) {
                detail::UtcMonthMemo memo;
                for_each_month_return(
                    curve, n,
                    [&memo](int64_t ts_ms) { return detail::month_key_utc(memo, ts_ms); },
                    emit);
            };
            sharpe_sortino_generated(month_returns, 0.02 / 12.0, std::sqrt(12.0),
                                     &e.sharpe_monthly, &e.sortino_monthly,
                                     "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
        } else {
            tz_util::ScopedTimezone guard(chart_tz);   // ONE guard for both generated passes
            const auto month_returns = [&](auto emit) {
                for_each_month_return(curve, n, month_key_local, emit);
            };
            sharpe_sortino_generated(month_returns, 0.02 / 12.0, std::sqrt(12.0),
                                     &e.sharpe_monthly, &e.sortino_monthly,
                                     "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
        }
    }

    // --- Per-bar variant: density-annualized (observed bars/year, NOT the
    // calendar tf formula — that inflates Sharpe ~sqrt(5) on non-24/7
    // sessions; see spec).
    if (n >= 3 && span_years > 0.0) {
        const double bars_per_year = check((double)(n - 1) / span_years, "sharpe_bar");
        const auto bar_returns = [&](auto emit) {
            for_each_bar_return(curve, n, emit);
        };
        sharpe_sortino_generated(bar_returns, 0.02 / bars_per_year, std::sqrt(bars_per_year),
                                 &e.sharpe_bar, &e.sortino_bar,
                                 "metrics.equity.sharpe_bar", "metrics.equity.sortino_bar");
    }
    return e;
}

// ArrayCurve and the L7c SpillCurve instantiations share this metric body.
template pf_equity_stats_t compute_equity_stats<ArrayCurve>(
    const ArrayCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double);

template pf_equity_stats_t compute_equity_stats<ArrayCurve>(
    const ArrayCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double, int64_t);

pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count) {
    // Preserve the old pointer overload's early return for a null curve with
    // a positive count.  PointSource itself has a valid-range precondition.
    if (curve == nullptr && n > 0) {
        const ArrayCurve empty(nullptr, 0);
        return compute_equity_stats<ArrayCurve>(
            empty, 0, initial_capital, chart_tz, first_open, last_close,
            bars_in_market, net_profit, observation_count);
    }
    const ArrayCurve source(curve, n);
    return compute_equity_stats<ArrayCurve>(
        source, n, initial_capital, chart_tz, first_open, last_close,
        bars_in_market, net_profit, observation_count);
}

// The ordinary entry: every point of the curve is an observation.
pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit) {
    return compute_equity_stats(curve, n, initial_capital, chart_tz, first_open, last_close,
                                bars_in_market, net_profit, n);
}

// L7c: emit both scratch-source entry points from the SAME metric body and
// floating-point operation order as ArrayCurve.
template pf_equity_stats_t compute_equity_stats<SpillCurve>(
    const SpillCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double);

template pf_equity_stats_t compute_equity_stats<SpillCurve>(
    const SpillCurve&, int64_t, double, const std::string&, double, double,
    int64_t, double, int64_t);

}  // namespace metrics

void BacktestEngine::fold_trade_percent_prefix(const Trade& trade) {
    trades_.invalidate_percent_fold();
    trade_percent_prefix_.all_sum += trade.pnl_pct;
    ++trade_percent_prefix_.all_count;
    if (trade.pnl > 0.0) {
        trade_percent_prefix_.win_sum += trade.pnl_pct;
        ++trade_percent_prefix_.win_count;
    } else if (trade.pnl < 0.0) {
        trade_percent_prefix_.loss_sum += trade.pnl_pct;
        ++trade_percent_prefix_.loss_count;
    }
}

void BacktestEngine::reset_trade_percent_prefix() {
    trades_.invalidate_percent_fold();
    trade_percent_prefix_ = {};
    // Bind the empty report seeds before publication; the first read after
    // a finished ordinary run must not discover its closed history. A ledger
    // that still has a sealed base has no empty prefix to bind.
    if (trades_.base() == 0) (void)trades_.report_fold(initial_capital_, {});
}

TradeLedger::PercentFold BacktestEngine::current_trade_percent_fold() const {
    const auto& prefix = trade_percent_prefix_;
    return trades_.percent_fold({prefix.all_sum, prefix.win_sum, prefix.loss_sum,
                                prefix.all_count, prefix.win_count, prefix.loss_count});
}

}  // namespace pineforge

// Read-side binding. Keep the existing metric implementation and
// its array instantiations above unchanged; L7c owns the public SpillCurve
// instantiations. The internal window can exercise the real scratch reader
// before that downstream adapter is merged.
#include "report_scratch.hpp"

namespace pineforge {
namespace metrics {

template pf_equity_stats_t compute_equity_stats<report_sink::ScratchCurveWindow>(
    const report_sink::ScratchCurveWindow&, int64_t, double, const std::string&,
    double, double, int64_t, double);

template pf_equity_stats_t compute_equity_stats<report_sink::ScratchCurveWindow>(
    const report_sink::ScratchCurveWindow&, int64_t, double, const std::string&,
    double, double, int64_t, double, int64_t);

} // namespace metrics
} // namespace pineforge

#include <cstring>
#include <type_traits>

namespace pineforge::metrics {

void TradeStatsAccumulator::reset(TradeFilter filter, double initial_capital) noexcept {
    static_assert(std::is_trivially_copyable_v<State>, "report accumulator must stay scalar");
    *state_ = State(filter, initial_capital);
}

std::size_t TradeStatsAccumulator::state_bytes() noexcept {
    return sizeof(State);
}

bool TradeStatsAccumulator::same_state(const TradeStatsAccumulator& other) const noexcept {
    const auto& a = *state_;
    const auto& b = *other.state_;
    const auto same_double = [](const double& left, const double& right) {
        return std::memcmp(&left, &right, sizeof(double)) == 0;
    };
    if (a.filter != b.filter || !same_double(a.initial_capital, b.initial_capital)
        || a.nonfinite_field != b.nonfinite_field || a.nonfinite_prefix != b.nonfinite_prefix)
        return false;
#define PF_SAME_INTEGER(field) if (a.stats.field != b.stats.field) return false
    PF_SAME_INTEGER(num_trades);
    PF_SAME_INTEGER(num_wins);
    PF_SAME_INTEGER(num_losses);
    PF_SAME_INTEGER(num_even);
    PF_SAME_INTEGER(max_consecutive_wins);
    PF_SAME_INTEGER(max_consecutive_losses);
#undef PF_SAME_INTEGER
#define PF_SAME_STAT(field) if (!same_double(a.stats.field, b.stats.field)) return false
    PF_SAME_STAT(percent_profitable);
    PF_SAME_STAT(net_profit);
    PF_SAME_STAT(net_profit_pct);
    PF_SAME_STAT(gross_profit);
    PF_SAME_STAT(gross_profit_pct);
    PF_SAME_STAT(gross_loss);
    PF_SAME_STAT(gross_loss_pct);
    PF_SAME_STAT(profit_factor);
    PF_SAME_STAT(avg_trade);
    PF_SAME_STAT(avg_trade_pct);
    PF_SAME_STAT(avg_win);
    PF_SAME_STAT(avg_win_pct);
    PF_SAME_STAT(avg_loss);
    PF_SAME_STAT(avg_loss_pct);
    PF_SAME_STAT(ratio_avg_win_avg_loss);
    PF_SAME_STAT(largest_win);
    PF_SAME_STAT(largest_win_pct);
    PF_SAME_STAT(largest_loss);
    PF_SAME_STAT(largest_loss_pct);
    PF_SAME_STAT(commission_paid);
    PF_SAME_STAT(expectancy);
    PF_SAME_STAT(avg_bars_in_trade);
    PF_SAME_STAT(avg_bars_in_wins);
    PF_SAME_STAT(avg_bars_in_losses);
#undef PF_SAME_STAT
#define PF_SAME_LOCAL(field) if (!same_double(a.field, b.field)) return false
    PF_SAME_LOCAL(pct_sum);
    PF_SAME_LOCAL(win_pct_sum);
    PF_SAME_LOCAL(loss_pct_sum);
    PF_SAME_LOCAL(bars_sum);
    PF_SAME_LOCAL(win_bars_sum);
    PF_SAME_LOCAL(loss_bars_sum);
    PF_SAME_LOCAL(largest_win);
    PF_SAME_LOCAL(largest_win_pct);
    PF_SAME_LOCAL(largest_loss);
    PF_SAME_LOCAL(largest_loss_pct);
#undef PF_SAME_LOCAL
    return a.streak_w == b.streak_w && a.streak_l == b.streak_l;
}

} // namespace pineforge::metrics
