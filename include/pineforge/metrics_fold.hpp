#pragma once

// Pure report folds retain no engine or point source. Streaming-ratio folds
// own a bounded RAM batch and the run's positional scratch stream.
// The caller supplies chart-civil month keys after dispatch, without holding
// a timezone guard across an engine callback.
#include <pineforge/pineforge.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace pineforge {
namespace metrics {

// Same whole-second truncation and civil-month convention as the array path.
// Non-UTC calls take and release the timezone lock within this call; callers
// must not already hold that lock. The pure folds below also accept keys from
// an independently verified calendar provider.
int equity_month_key(std::int64_t time_ms, const std::string& chart_timezone);

// Outside this domain the legacy libc fallback computes its month key in
// int, with possible signed overflow. Monthly replay never claims that
// domain or changes that expression; the engine keeps/recovers the curve.
inline bool monthly_replay_time_supported_v1(std::int64_t time_ms) {
    constexpr std::int64_t span_seconds = std::int64_t{1} << 40;
    const auto seconds = time_ms / 1000; // same truncation as the legacy key
    return seconds > -span_seconds && seconds < span_seconds;
}

// Streaming ratios. A fold owns a fixed-size prefix, one editable held point,
// and an ordered packed return replay. Its fallback result is an internal
// routing decision, never a run failure; scratch I/O failures remain typed.
enum class MetricsStreamingStatusV1 : std::int32_t {
    Ok = 0,
    NotImplemented = -1,
    InvalidArgument = -2,
    Busy = -3,
};
enum class StreamingSharpeV1 : std::uint32_t { Off = 0, On = 1 };
using MetricsNeedMaskV1 = std::uint64_t;
constexpr MetricsNeedMaskV1 kMetricsNeedSharpeBarV1 = 1ULL << 0;
constexpr MetricsNeedMaskV1 kMetricsNeedSortinoBarV1 = 1ULL << 1;
constexpr MetricsNeedMaskV1 kMetricsNeedMonthlyRatiosV1 = 1ULL << 2;
constexpr MetricsNeedMaskV1 kMetricsNeedReturnsV1 = 1ULL << 3;
constexpr MetricsNeedMaskV1 kMetricsNeedEquitySummaryV1 = 1ULL << 4;
constexpr MetricsNeedMaskV1 kMetricsNeedTradeStatsV1 = 1ULL << 5;
constexpr MetricsNeedMaskV1 kMetricsNeedCurveV1 = 1ULL << 6;
constexpr MetricsNeedMaskV1 kMetricsNeedAllV1 = (1ULL << 7) - 1;

// Closed month ends only; the current bucket and editable tail are scalars.
constexpr std::size_t kMetricsMonthEndLimitV1 = 4096;

// Fixed-width, versioned diagnostic PODs; no change to pf_report_t.
// Empty extents have empty=1 and count=0; endpoint words are not observations.
struct MetricsExtentV1 {
    std::int64_t count = 0;
    std::int64_t first_time_ms = 0;
    std::int64_t last_time_ms = 0;
    std::uint32_t empty = 1;
    std::uint32_t reserved = 0;
};
struct MetricsExtentPlanV1 {
    MetricsExtentV1 extent;
    std::uint64_t attempt = 0;
    std::uint64_t input_generation = 0;
    std::uint64_t admitted_view = 0;
    std::uint64_t spec_identity = 0;
    std::uint32_t present = 0;
    std::uint32_t reserved = 0;
};
enum class MetricsRatioRouteV1 : std::uint32_t {
    Unavailable = 0, Streaming = 1, LegacyActualCurve = 2, LegacyRerun = 3,
};
enum class MetricsExtentWitnessV1 : std::uint32_t {
    NotEvaluated = 0, Match = 1, Mismatch = 2,
};
enum class MetricsRouteReasonV1 : std::uint32_t {
    None = 0, SharpeSwitchOff = 1, ReturnsNeeded = 2, CurveNeeded = 3,
    PlainNo = 4, NoExtentProof = 5, NoRecoveryProof = 6, NumericGuard = 7,
    ExtentMismatch = 8, LiveSnapshot = 9, ExistingFailure = 10,
    OrderedReplay = 11, ScratchUnavailable = 12,
};
enum class MetricsStopBoundaryKindV1 : std::uint32_t {
    None = 0, CompletedMark = 1, IntraBar = 2, MarginCallback = 3,
};
struct MetricsStopBoundaryV1 {
    std::int64_t completed_marks = 0;
    std::int64_t input_index = -1;
    MetricsStopBoundaryKindV1 kind = MetricsStopBoundaryKindV1::None;
    std::uint32_t reserved = 0;
};
// mismatch_fields: bit 0 empty, 1 count, 2 first, 3 last, 4 attempt,
// 5 input_generation, 6 admitted_view, 7 spec_identity.
struct MetricsExtentReceiptV1 {
    std::uint32_t struct_size = sizeof(MetricsExtentReceiptV1);
    std::uint32_t version = 1;
    MetricsExtentPlanV1 extent_plan;
    MetricsExtentV1 actual_extent;
    std::uint64_t actual_attempt = 0;
    std::uint64_t actual_input_generation = 0;
    std::uint64_t actual_admitted_view = 0;
    std::uint64_t actual_spec_identity = 0;
    std::uint64_t eligible_returns = 0;
    std::uint64_t mismatch_fields = 0;
    std::uint64_t failed_plain_inputs = 0;
    MetricsNeedMaskV1 metrics_needed = kMetricsNeedAllV1;
    MetricsStopBoundaryV1 stop_boundary;
    MetricsExtentWitnessV1 witness = MetricsExtentWitnessV1::NotEvaluated;
    MetricsRatioRouteV1 route = MetricsRatioRouteV1::Unavailable;
    MetricsRouteReasonV1 route_reason = MetricsRouteReasonV1::None;
    StreamingSharpeV1 streaming_sharpe = StreamingSharpeV1::On;
    std::uint32_t plain_subset = 0;
    std::uint32_t replay_attempts = 0;
    std::uint64_t authoritative_attempt = 0;
};
static_assert(sizeof(MetricsExtentV1) == 32, "streaming-ratio extent POD");
static_assert(sizeof(MetricsExtentPlanV1) == 72, "streaming-ratio plan POD");
static_assert(sizeof(MetricsStopBoundaryV1) == 24, "streaming-ratio stop POD");
static_assert(sizeof(MetricsExtentReceiptV1) == 232, "streaming-ratio receipt POD");

// The engine uses actual extent for Sharpe; only Sortino consumes the plan.
// On is the default; explicit Off plus a Sharpe need selects the exact curve
// route. A mask without SharpeBar permits skipping Sharpe. Returns/Curve
// needs retain the curve under either switch setting. A mismatch or
// failed recovery proof chooses the exact curve route, never a new run
// failure or a changed report domain. An admitted no-curve route uses the
// ordered scratch replay and reports OrderedReplay.
struct MetricsStreamingConfigV1 {
    StreamingSharpeV1 streaming_sharpe = StreamingSharpeV1::On;
    MetricsNeedMaskV1 metrics_needed = kMetricsNeedAllV1;
    std::uint64_t attempt = 0;
    std::uint64_t input_generation = 0;
    std::uint64_t admitted_view = 0;
    std::uint64_t spec_identity = 0;
    std::uint32_t deterministic_replay = 0;
    std::uint32_t curve_available = 1;
};
namespace fold_detail {
inline std::uint64_t extent_mismatch_v1(const MetricsExtentPlanV1& plan,
        const MetricsStreamingConfigV1& config, const MetricsExtentV1& actual) {
    if (!plan.present) return 0;
    std::uint64_t fields = 0;
    if (plan.extent.empty != actual.empty) fields |= 1ULL << 0;
    if (plan.extent.count != actual.count) fields |= 1ULL << 1;
    if (!plan.extent.empty && !actual.empty) {
        if (plan.extent.first_time_ms != actual.first_time_ms) fields |= 1ULL << 2;
        if (plan.extent.last_time_ms != actual.last_time_ms) fields |= 1ULL << 3;
    }
    if (plan.attempt != config.attempt) fields |= 1ULL << 4;
    if (plan.input_generation != config.input_generation) fields |= 1ULL << 5;
    if (plan.admitted_view != config.admitted_view) fields |= 1ULL << 6;
    if (plan.spec_identity != config.spec_identity) fields |= 1ULL << 7;
    return fields;
}
} // namespace fold_detail
struct MetricsRatioTraceV1 {
    const char* series_stage;
    std::int64_t return_index;
    const char* operation;
    const char* code;
    const char* field;
    const char* reason;
};
using MetricsRatioTraceCallbackV1 = void (*)(void*, const MetricsRatioTraceV1&);
struct MetricsRatioTraceSinkV1 {
    void* context = nullptr;
    MetricsRatioTraceCallbackV1 emit = nullptr;
};
struct MetricsStreamingResultV1 {
    MetricsExtentReceiptV1 receipt;
    double sharpe_bar = std::numeric_limits<double>::quiet_NaN();
    double sortino_bar = std::numeric_limits<double>::quiet_NaN();
    double downside_sum = 0.0;
    double centered_sum = 0.0;
    const char* failure_code = nullptr;
    const char* failure_field = nullptr;
    const char* failure_reason = nullptr;
};
// Optional checked, final metric-point provider for the exact curve route.
// A Streaming result must not read it. It is absent after a no-curve attempt;
// LegacyRerun then asks the eligible host for its authoritative second run.
struct MetricsRatioCurveV1 {
    void* context = nullptr;
    std::int64_t count = 0;
    pf_equity_point_t (*read)(void*, std::int64_t) = nullptr;
};
class StreamingEquityRatiosV1 {
public:
    virtual ~StreamingEquityRatiosV1() = default;
    virtual MetricsStreamingStatusV1 append(const pf_equity_point_t&, int month_key) = 0;
    // zero-based logical index must be the CURRENT held point. A released
    // point cannot be amended after its successor exists. Violation is an
    // internal assertion (SIGABRT, also in Release), never a new run failure.
    // Repeated amendments before release are legal.
    virtual MetricsStreamingStatusV1 amend_held(
        std::int64_t logical_index, const pf_equity_point_t&, int month_key) = 0;
    virtual MetricsStreamingStatusV1 finish(
        MetricsStreamingResultV1&, MetricsRatioCurveV1 = {}) const = 0;
    virtual std::size_t state_size_bytes() const = 0;
};
MetricsStreamingStatusV1 make_streaming_equity_ratios_v1(
    const MetricsStreamingConfigV1&, const MetricsExtentPlanV1&,
    MetricsRatioTraceSinkV1, std::unique_ptr<StreamingEquityRatiosV1>&);

// Separate companion, preserving the existing result/receipt and virtual
// interface layouts. Explicit monthly needs enable it at construction for
// no-curve folds. The engine also enables it for bar-only needs because its
// completed equity summary includes monthly fields under every scalar mask.
// Enable before the first append; foreign fold implementations are refused.
struct MetricsMonthlyResultV1 {
    double sharpe_monthly = std::numeric_limits<double>::quiet_NaN();
    double sortino_monthly = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t complete = 0;
    std::uint32_t reserved = 0;
};
MetricsStreamingStatusV1 enable_streaming_monthly_replay_v1(StreamingEquityRatiosV1&);
MetricsStreamingStatusV1 streaming_monthly_result_v1(
    const StreamingEquityRatiosV1&, MetricsMonthlyResultV1&);

struct EquityReturnSummary {
    std::size_t count = 0;
    double sum = 0.0;
    void fold(double value) { sum += value; ++count; }
};

struct EquityFoldSnapshot {
    std::int64_t point_count = 0;
    std::int64_t observation_count = 0;
    std::int64_t bars_in_market = 0;
    pf_equity_point_t first{};
    pf_equity_point_t last{};
    double max_drawdown = 0.0, max_drawdown_pct = 0.0;
    double max_runup = 0.0, max_runup_pct = 0.0;
    EquityReturnSummary bar_returns, month_returns;
    // Report-only ordered witness, independent of the broker-state hash.
    std::uint64_t point_witness = 1469598103934665603ULL;
    // First failure in the ordered point walk, deferred until finalization:
    // the array producer checks inputs and the FINAL open profit first.
    const char* nonfinite_field = nullptr;

    double span_ms() const {
        return last.time_ms >= first.time_ms
            ? static_cast<double>(static_cast<std::uint64_t>(last.time_ms)
                                  - static_cast<std::uint64_t>(first.time_ms))
            : -static_cast<double>(static_cast<std::uint64_t>(first.time_ms)
                                   - static_cast<std::uint64_t>(last.time_ms));
    }
    double span_years() const { return span_ms() / (365.25 * 86400.0 * 1000.0); }
    bool needs_second_pass() const {
        return month_returns.count >= 2
            || (point_count >= 3 && span_years() > 0.0 && bar_returns.count >= 2);
    }
    bool same_input(const EquityFoldSnapshot& other) const {
        return point_count == other.point_count
            && observation_count == other.observation_count
            && bars_in_market == other.bars_in_market
            && point_witness == other.point_witness
            && ((nonfinite_field == nullptr && other.nonfinite_field == nullptr)
                || (nonfinite_field && other.nonfinite_field
                    && std::strcmp(nonfinite_field, other.nonfinite_field) == 0))
            && bar_returns.count == other.bar_returns.count
            && month_returns.count == other.month_returns.count
            && std::memcmp(&bar_returns.sum, &other.bar_returns.sum, sizeof(double)) == 0
            && std::memcmp(&month_returns.sum, &other.month_returns.sum, sizeof(double)) == 0
            && std::memcmp(&first, &other.first, sizeof(first)) == 0
            && std::memcmp(&last, &other.last, sizeof(last)) == 0;
    }
};

namespace fold_detail {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

// Do not throw while accumulating a prefix. A terminal replacement can remove
// its held defect, and a completed report has checks preceding this walk.
// Keep only the first field in each ordered stage; the producer's existing
// report_result_invalid helper raises it at that stage during finalization.
inline double defer_nonfinite(double value, const char* field, const char*& first) {
    if (!std::isfinite(value) && first == nullptr) first = field;
    return value;
}

// The state of the array path's two return generators, in their original
// order. A bucket closes on a KEY CHANGE, including nonmonotonic timestamps.
struct ReturnSequence {
    bool have_point = false;
    bool have_previous_month = false;
    int current_key = 0;
    double previous_equity = 0.0;
    double current_month_end = 0.0, previous_month_end = 0.0;

    template<class BarEmit, class MonthEmit>
    void fold(const pf_equity_point_t& point, int key, BarEmit bar, MonthEmit month) {
        if (!have_point) {
            have_point = true;
            current_key = key;
            previous_equity = current_month_end = point.equity;
            return;
        }
        if (previous_equity > 0.0) bar(point.equity / previous_equity - 1.0);
        previous_equity = point.equity;
        if (key != current_key) {
            finish_month(month);
            previous_month_end = current_month_end;
            have_previous_month = true;
            current_key = key;
        }
        current_month_end = point.equity;
    }
    template<class MonthEmit>
    void finish_month(MonthEmit month) const {
        if (have_previous_month && previous_month_end > 0.0)
            month(current_month_end / previous_month_end - 1.0);
    }
};

inline void witness_word(std::uint64_t& hash, std::uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) {
        hash ^= (word >> (8 * i)) & 255u;
        hash *= 1099511628211ULL;
    }
}
inline void witness_double(std::uint64_t& hash, double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    witness_word(hash, bits);
}

struct FirstPrefix {
    EquityFoldSnapshot values;
    ReturnSequence returns;
    double peak = 0.0, trough = 0.0;

    void fold(const pf_equity_point_t& point, int key, bool observation, bool exposed) {
        auto& s = values;
        if (s.point_count == 0) {
            s.first = point;
            peak = trough = point.equity; // FIRST point, never initial capital
        }
        ++s.point_count;
        if (observation) ++s.observation_count;
        if (observation && exposed) ++s.bars_in_market;
        s.last = point;
        const auto check = [&s](double value, const char* field) {
            return defer_nonfinite(value, field, s.nonfinite_field);
        };
        const double eq = check(point.equity, "equity_curve.equity");
        check(point.open_profit, "equity_curve.open_profit");
        if (eq > peak) { peak = eq; trough = eq; }
        if (eq < trough) trough = eq;
        const double dd = check(peak - eq, "metrics.equity.max_equity_drawdown");
        if (dd > s.max_drawdown) {
            s.max_drawdown = dd;
            s.max_drawdown_pct = peak > 0.0
                ? check(dd / peak * 100.0, "metrics.equity.max_equity_drawdown_pct") : nan;
        }
        const double ru = check(eq - trough, "metrics.equity.max_equity_runup");
        if (ru > s.max_runup) {
            s.max_runup = ru;
            s.max_runup_pct = trough > 0.0
                ? check(ru / trough * 100.0, "metrics.equity.max_equity_runup_pct") : nan;
        }
        returns.fold(point, key,
            [&](double r) { s.bar_returns.fold(r); },
            [&](double r) { s.month_returns.fold(r); });
        witness_word(s.point_witness, static_cast<std::uint64_t>(point.time_ms));
        witness_double(s.point_witness, point.equity);
        witness_double(s.point_witness, point.open_profit);
        witness_word(s.point_witness, static_cast<std::uint64_t>(key));
        witness_word(s.point_witness, observation ? 1u : 0u);
        witness_word(s.point_witness, observation && exposed ? 1u : 0u);
    }
};
} // namespace fold_detail

// Prefix excludes the held point. Replacing that point therefore requires no
// subtraction, inverse sum, or regrouping, even across a civil-month boundary.
// first_pass() closes the held month in a value copy and is repeatable.
class EquityMetricsFold {
public:
    void fold(const pf_equity_point_t& point, int month_key,
              bool observation = true, bool exposed = false) {
        if (have_held_)
            prefix_.fold(held_, held_key_, held_observation_, held_exposed_);
        held_ = point;
        held_key_ = month_key;
        held_observation_ = observation;
        held_exposed_ = exposed;
        have_held_ = true;
    }
    // Returns false on an empty fold; it never invents an observation.
    bool replace_last(const pf_equity_point_t& point, int month_key) {
        if (!have_held_) return false;
        held_ = point;
        held_key_ = month_key;
        return true;
    }
    const pf_equity_point_t* held_point() const { return have_held_ ? &held_ : nullptr; }
    int held_month_key() const { return held_key_; }
    bool held_observation() const { return held_observation_; }
    bool held_exposed() const { return held_exposed_; }
    EquityFoldSnapshot first_pass() const {
        auto result = prefix_;
        if (have_held_)
            result.fold(held_, held_key_, held_observation_, held_exposed_);
        result.returns.finish_month(
            [&](double r) { result.values.month_returns.fold(r); });
        return result.values;
    }

private:
    fold_detail::FirstPrefix prefix_;
    pf_equity_point_t held_{};
    int held_key_ = 0;
    bool held_observation_ = false, held_exposed_ = false, have_held_ = false;
};

struct EquityMetricInputs {
    double initial_capital;
    double first_open;
    double last_close;
    double net_profit; // Report row-order sum, including presented terminal rows.
};

struct EquityRatioResult {
    EquityFoldSnapshot expected, replayed;
    double sharpe_monthly = fold_detail::nan, sortino_monthly = fold_detail::nan;
    double sharpe_bar = fold_detail::nan, sortino_bar = fold_detail::nan;
    const char* nonfinite_field = nullptr;
    bool complete = false;
    bool matches(const EquityFoldSnapshot& first) const {
        return complete && expected.same_input(first) && replayed.same_input(first);
    }
};

namespace fold_detail {
struct ReturnVariance {
    std::size_t expected_count = 0, count = 0;
    double mean = 0.0, rf = 0.0, ann = 0.0;
    double var = 0.0, down = 0.0;
    const char* sharpe_field = nullptr;
    const char* sortino_field = nullptr;
    const char* nonfinite_field = nullptr;
    bool enabled = false;
    double check(double value, const char* field) {
        return defer_nonfinite(value, field, nonfinite_field);
    }
    void prepare(const EquityReturnSummary& first, double risk_free, double annualization,
                 bool allowed, const char* sharpe_name, const char* sortino_name) {
        sharpe_field = sharpe_name;
        sortino_field = sortino_name;
        expected_count = first.count;
        enabled = allowed && first.count >= 2;
        if (!enabled) return;
        rf = check(risk_free, sharpe_field);
        ann = check(annualization, sharpe_field);
        check(first.sum, sharpe_field);
        mean = check(first.sum / static_cast<double>(first.count), sharpe_field);
    }
    void fold(double value) {
        if (!enabled) return;
        check(value, sharpe_field);
        const double delta = check(value - mean, sharpe_field);
        var = check(var + delta * delta, sharpe_field);
        const double d = std::min(0.0, check(value - rf, sortino_field));
        down = check(down + d * d, sortino_field);
        ++count;
    }
    bool complete() const { return !enabled || count == expected_count; }
    void finish(double& sharpe, double& sortino) {
        sharpe = sortino = nan;
        if (!enabled || !complete()) return;
        const double sd = check(std::sqrt(var / static_cast<double>(expected_count - 1)),
                                sharpe_field);
        const double dd = check(std::sqrt(down / static_cast<double>(expected_count)),
                                sortino_field);
        if (sd > 0.0) sharpe = check((mean - rf) / sd * ann, sharpe_field);
        if (dd > 0.0) sortino = check((mean - rf) / dd * ann, sortino_field);
    }
};
struct SecondPrefix {
    ReturnSequence returns;
    ReturnVariance bars, months;
    void fold(const pf_equity_point_t& point, int key) {
        returns.fold(point, key, [&](double r) { bars.fold(r); },
                     [&](double r) { months.fold(r); });
    }
};
} // namespace fold_detail

// A deterministic replay feeds finalized points in their original order.
// This class does not execute an engine, retain an input, or invoke observers.
// It also holds its last point so a replay can apply terminal/presentation
// rewrites before finish(). Both return series share this single walk.
class EquityMetricsSecondPass {
public:
    explicit EquityMetricsSecondPass(const EquityFoldSnapshot& first) : expected_(first) {
        prefix_.months.prepare(first.month_returns, 0.02 / 12.0, std::sqrt(12.0), true,
                               "metrics.equity.sharpe_monthly", "metrics.equity.sortino_monthly");
        const double span = first.span_years();
        if (first.point_count >= 3 && span > 0.0) {
            const double bars_per_year = prefix_.bars.check(
                static_cast<double>(first.point_count - 1) / span, "metrics.equity.sharpe_bar");
            prefix_.bars.prepare(first.bar_returns, 0.02 / bars_per_year,
                                 std::sqrt(bars_per_year), true,
                                 "metrics.equity.sharpe_bar", "metrics.equity.sortino_bar");
        }
    }
    void fold(const pf_equity_point_t& point, int month_key,
              bool observation = true, bool exposed = false) {
        if (const auto* held = replay_.held_point())
            prefix_.fold(*held, replay_.held_month_key());
        replay_.fold(point, month_key, observation, exposed);
    }
    bool replace_last(const pf_equity_point_t& point, int month_key) {
        return replay_.replace_last(point, month_key);
    }
    EquityRatioResult finish() const {
        auto final = prefix_;
        if (const auto* held = replay_.held_point())
            final.fold(*held, replay_.held_month_key());
        final.returns.finish_month([&](double r) { final.months.fold(r); });
        EquityRatioResult result;
        result.expected = expected_;
        result.replayed = replay_.first_pass();
        result.complete = expected_.same_input(result.replayed)
            && final.bars.complete() && final.months.complete();
        if (result.complete) {
            final.months.finish(result.sharpe_monthly, result.sortino_monthly);
            final.bars.finish(result.sharpe_bar, result.sortino_bar);
            // Monthly ratios precede per-bar ratios in the array producer,
            // regardless of when either defect was encountered during replay.
            result.nonfinite_field = final.months.nonfinite_field
                ? final.months.nonfinite_field : final.bars.nonfinite_field;
        }
        return result;
    }

private:
    EquityFoldSnapshot expected_;
    EquityMetricsFold replay_;
    fold_detail::SecondPrefix prefix_;
};

pf_equity_stats_t finalize_equity_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const EquityRatioResult* ratios = nullptr);

// The trade blocks come from the existing fixed accumulators. Passing no
// second pass explicitly produces first-pass fields only; callers publish
// readiness separately (needs_second_pass), never describe pending ratios as
// a completed summary.
inline pf_metrics_t finalize_metrics(
        const EquityFoldSnapshot& first, const EquityMetricInputs& inputs,
        const pf_trade_stats_t& all, const pf_trade_stats_t& longs,
        const pf_trade_stats_t& shorts, const EquityRatioResult* ratios = nullptr) {
    pf_metrics_t result{};
    result.all = all;
    result.longs = longs;
    result.shorts = shorts;
    result.equity = finalize_equity_metrics(first, inputs, ratios);
    return result;
}

} // namespace metrics
} // namespace pineforge
