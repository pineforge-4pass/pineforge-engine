// streaming-ratio first stage. Against feae3369's frozen headers the first missing feature
// is an streaming-ratio contract type (the supervisor records the actual diagnostic).
// With the declarations/stubs applied these compile and FAIL at the streaming-ratio API
// status assertion. No skip, expected-failure property, or stub catch exists.
// Resolve metrics_stream's enclosing pineforge::detail scratch declaration
// before equity_stats_reference.hpp introduces metrics::detail.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace pineforge::source {
// The existing friend seam (pine_strategy_host.hpp:873) also permits the
// loaded module to call Pine's private presenter (:1024). Qualify the call
// to avoid redispatch, as in tests/test_report_sink_equity.cpp:35.
struct ReportSinkEquityTestAccess {
    static void present(const PineStrategyHost& host, ReportC* out) {
        host.PineStrategyHost::present_report(out);
    }
    static void record_range_end(PineStrategyHost& host) {
        host.record_applied_range_end();
    }
};
} // namespace pineforge::source

#if defined(STREAMING_MODULE_OLD) || defined(STREAMING_MODULE_CUSTOM)
// The loaded-library rows compile THIS unit into two small real modules on
// the verification host. OLD exports no PLAIN declaration. CUSTOM models a
// hand-edited generated module and declares its presenter override honestly.
class GeneratedStrategy final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {}
#ifdef STREAMING_MODULE_CUSTOM
    void present_report(pineforge::ReportC* out) const override {
        pineforge::source::ReportSinkEquityTestAccess::present(*this, out);
        if (out->equity_curve_len >= 3) {
            out->equity_curve[1].equity *= .97;
            out->equity_curve[out->equity_curve_len - 1].equity *= 1.01;
        }
    }
#endif
};
extern "C" PF_API pineforge::NativeStrategyHost* streaming_module_create() {
    return new GeneratedStrategy;
}
extern "C" PF_API void streaming_module_destroy(pineforge::NativeStrategyHost* host) {
    delete host;
}
#ifdef STREAMING_MODULE_CUSTOM
extern "C" PF_API const pineforge::MetricsPlainDeclarationV1* streaming_module_plain() {
    static const pineforge::MetricsPlainDeclarationV1 declaration{
        sizeof(pineforge::MetricsPlainDeclarationV1), 1, 0, 1, 1, 0, 0, 0, 0, 0};
    return &declaration;
}
#endif
#else
#include "metrics_streaming_stop_fixture.hpp"
#include <pineforge/metrics.hpp>
#include <pineforge/selected_window.h>

#include <dlfcn.h>
#include <array>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace streaming_suite {
namespace mt = metrics_streaming_test;
using namespace pineforge;
using namespace pineforge::metrics;
int failures = 0, checks = 0;
std::string current_row, filter, mutant;
bool list_only = false;
struct RatioTrial {
    MetricsStreamingStatusV1 status = MetricsStreamingStatusV1::NotImplemented;
    const char* stage = "factory";
    MetricsExtentPlanV1 planned;
    mt::LegacyRatios old;
    std::optional<MetricsStreamingResultV1> result;
    std::vector<mt::Trace> trace;
    std::size_t provider_reads = 0, state_bytes = 0, final_state_bytes = 0;
};
struct Artifact {
    std::string name, source_identity = "independent_generated_points";
    MetricsExtentPlanV1 planned;
    MetricsExtentV1 actual_extent;
    mt::LegacyRatios old;
    std::optional<MetricsStreamingResultV1> candidate;
    std::optional<MetricsExtentReceiptV1> replay_receipt;
    std::optional<mt::RunResult> replay_report;
    std::optional<mt::LegacyRatios> mutant_result;
    std::optional<pf_equity_stats_t> curve_control;
    std::optional<std::pair<double, double>> old_monthly, new_monthly;
    std::vector<std::pair<std::string, RatioTrial>> trials;
    std::vector<mt::Trace> candidate_trace;
    std::string original_code, original_args = "{}", original_message;
    MetricsStopBoundaryV1 boundary;
    std::string plain = "not_applicable", predicate_inputs, exact, mutation;
    bool legacy_control = false;
    bool reference_available = false;
    bool unavailable = false;
    int failures_before = 0;
    int executions = 0, publications = 0;
    std::size_t provider_reads = 0, state_bytes = 0;
    double kappa = mt::nan;
};
std::vector<Artifact> artifacts;
Artifact* active = nullptr;
#define STREAM_CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", current_row.c_str(), \
                 __FILE__, __LINE__, #expr); } } while (0)

bool row(const std::string& name) {
    if (list_only) { std::puts(name.c_str()); return false; }
    if (!filter.empty() && name.find(filter) == std::string::npos) return false;
    current_row = name;
    artifacts.emplace_back();
    active = &artifacts.back();
    active->name = name;
    active->failures_before = failures;
    return true;
}
bool implemented(MetricsStreamingStatusV1 status) {
    STREAM_CHECK(status == MetricsStreamingStatusV1::Ok);
    if (status != MetricsStreamingStatusV1::Ok) {
        if (active) active->unavailable = true;
        std::fprintf(stderr, "[%s] streaming-ratio not implemented (feature assertion)\n",
                     current_row.c_str());
        return false;
    }
    return true;
}
struct Provider {
    const mt::PointSource& points;
    std::size_t reads = 0;
    static pf_equity_point_t read(void* context, std::int64_t i) {
        auto& self = *static_cast<Provider*>(context);
        ++self.reads;
        return self.points[i];
    }
    MetricsRatioCurveV1 view() { return {this, points.count, &read}; }
};
void compare_result(const mt::LegacyRatios& want, const MetricsStreamingResultV1& got,
                    MetricsNeedMaskV1 needed) {
    const std::string code = got.failure_code ? got.failure_code : "";
    STREAM_CHECK(code == want.failure_code);
    STREAM_CHECK(std::string(got.failure_field ? got.failure_field : "") == want.failure_field);
    STREAM_CHECK(std::string(got.failure_reason ? got.failure_reason : "") == want.failure_reason);
    if (!want.failure_code.empty()) return;
    // The Streaming route is the ordered replay: the legacy fold over the same
    // returns in the same order, so every route compares bit for bit.
    if (needed & kMetricsNeedSortinoBarV1)
        STREAM_CHECK(mt::same(got.sortino_bar, want.sortino));
    // A caller without SharpeBar may skip Sharpe altogether, under either
    // switch value. Do not turn an unread objective into a curve requirement.
    if (!(needed & kMetricsNeedSharpeBarV1)) return;
    STREAM_CHECK(std::isnan(got.sharpe_bar) == std::isnan(want.sharpe));
    STREAM_CHECK(mt::same(got.sharpe_bar, want.sharpe));
}
bool candidate(const mt::PointSource& points, MetricsExtentPlanV1 planned,
               MetricsStreamingConfigV1 cfg, MetricsRatioRouteV1 required,
               const mt::HeldScript* held = nullptr) {
    auto& a = *active;
    a.planned = planned;
    a.actual_extent = mt::extent(points);
    a.old = mt::legacy(points, points.count <= 64);
    a.reference_available = true;
    std::unique_ptr<StreamingEquityRatiosV1> fold;
    MetricsRatioTraceSinkV1 trace;
    if (points.count <= 64) trace = {&a.candidate_trace, &mt::collect_trace};
    if (!implemented(make_streaming_equity_ratios_v1(cfg, planned, trace, fold))) return false;
    STREAM_CHECK(fold != nullptr);
    if (!fold) return false;
    a.state_bytes = fold->state_size_bytes();
    STREAM_CHECK(a.state_bytes > 0 && a.state_bytes <= 64 * 1024 + 4096);
    if (held) {
        for (const auto& event : held->events) {
            const int key = equity_stats_reference::month_key_utc(event.point.time_ms);
            const auto status = event.amendment
                ? fold->amend_held(event.index, event.point, key) : fold->append(event.point, key);
            if (!implemented(status)) return false;
        }
    } else {
        for (std::int64_t i = 0; i < points.count; ++i) {
            const auto point = points[i];
            if (!implemented(fold->append(
                    point, equity_stats_reference::month_key_utc(point.time_ms)))) return false;
        }
    }
    STREAM_CHECK(fold->state_size_bytes() == a.state_bytes);
    Provider provider{points};
    MetricsStreamingResultV1 got;
    if (!implemented(fold->finish(got, cfg.curve_available ? provider.view()
                                                        : MetricsRatioCurveV1{}))) return false;
    a.candidate = got;
    a.provider_reads = provider.reads;
    STREAM_CHECK(got.receipt.route == required);
    STREAM_CHECK(mt::same_extent(got.receipt.actual_extent, a.actual_extent));
    STREAM_CHECK(got.receipt.eligible_returns == a.old.m);
    const bool matching = planned.present && mt::same_extent(planned.extent, a.actual_extent)
        && planned.attempt == cfg.attempt
        && planned.input_generation == cfg.input_generation
        && planned.admitted_view == cfg.admitted_view
        && planned.spec_identity == cfg.spec_identity;
    const auto witness = !planned.present ? MetricsExtentWitnessV1::NotEvaluated
        : matching ? MetricsExtentWitnessV1::Match : MetricsExtentWitnessV1::Mismatch;
    STREAM_CHECK(got.receipt.witness == witness);
    if (required == MetricsRatioRouteV1::Streaming) STREAM_CHECK(provider.reads == 0);
    if (required != MetricsRatioRouteV1::LegacyRerun)
        compare_result(a.old, got, cfg.metrics_needed);
    if (!a.old.failure_code.empty()) {
        const auto failure = std::find_if(a.candidate_trace.begin(), a.candidate_trace.end(),
            [](const mt::Trace& t) { return !t.code.empty(); });
        STREAM_CHECK(failure != a.candidate_trace.end());
        if (failure != a.candidate_trace.end()) {
            STREAM_CHECK(failure->stage == "bar");
            STREAM_CHECK(failure->index == a.old.failure_index);
            STREAM_CHECK(failure->operation == a.old.failure_operation);
            STREAM_CHECK(failure->field == a.old.failure_field);
            STREAM_CHECK(failure->reason == a.old.failure_reason);
        }
    }
    MetricsStreamingResultV1 again;
    if (!implemented(fold->finish(again, cfg.curve_available ? provider.view()
                                                           : MetricsRatioCurveV1{}))) return false;
    STREAM_CHECK(mt::same(got.sharpe_bar, again.sharpe_bar));
    STREAM_CHECK(mt::same(got.sortino_bar, again.sortino_bar));
    STREAM_CHECK(mt::same(got.downside_sum, again.downside_sum));
    STREAM_CHECK(got.receipt.eligible_returns == again.receipt.eligible_returns);
    STREAM_CHECK(got.receipt.route == again.receipt.route);
    return true;
}

struct ConcurrentTrials {
    std::atomic<std::int64_t> ready{0}, factories{0}, appends{0}, finishes{0};
    static void meet(std::atomic<std::int64_t>& arrivals, std::int64_t target = 2) {
        arrivals.fetch_add(1, std::memory_order_acq_rel);
        while (arrivals.load(std::memory_order_acquire) < target)
            std::this_thread::yield();
    }
};

// A fresh factory, fold, provider and trace for EVERY trial. This helper
// never touches the suite's global checks/artifact state, even on failure;
// only the joining thread records assertions. It is not repeated finish().
RatioTrial run_ratio_trial(const mt::PointSource& points, ConcurrentTrials* concurrent = nullptr) {
    RatioTrial out;
    out.planned = mt::plan(points);
    out.old = mt::legacy(points);
    const auto cfg = mt::config();
    std::unique_ptr<StreamingEquityRatiosV1> fold;
    if (concurrent) ConcurrentTrials::meet(concurrent->ready);
    out.status = make_streaming_equity_ratios_v1(
        cfg, out.planned, {&out.trace, &mt::collect_trace}, fold);
    // Both factory calls finish before either trial appends. Successful
    // folds therefore coexist on different threads, exposing shared state.
    // Meet even on NotImplemented so the red stub cannot strand a worker.
    if (concurrent) ConcurrentTrials::meet(concurrent->factories);
    if (out.status == MetricsStreamingStatusV1::Ok && fold) {
        out.state_bytes = fold->state_size_bytes();
        out.stage = "append";
    }
    for (std::int64_t i = 0; i < points.count; ++i) {
        if (out.status == MetricsStreamingStatusV1::Ok && fold) {
            const auto point = points[i];
            out.status = fold->append(
                point, equity_stats_reference::month_key_utc(point.time_ms));
        }
        // Neither trial can consume its next point until both have attempted
        // this one. A failed trial keeps meeting the gates without more API
        // calls, so a missing/broken implementation cannot strand its peer.
        if (concurrent) ConcurrentTrials::meet(concurrent->appends, 2 * (i + 1));
    }
    if (out.status == MetricsStreamingStatusV1::Ok && fold) {
        out.final_state_bytes = fold->state_size_bytes();
        Provider provider{points};
        MetricsStreamingResultV1 result;
        out.stage = "finish";
        out.status = fold->finish(result, provider.view());
        out.provider_reads = provider.reads;
        if (out.status == MetricsStreamingStatusV1::Ok) out.result = result;
    }
    // Keep both folds alive until both finish calls have completed.
    if (concurrent) ConcurrentTrials::meet(concurrent->finishes);
    return out;
}

bool record_trial(const std::string& name, const RatioTrial& trial) {
    auto& a = *active;
    if (a.trials.empty()) {
        a.old = trial.old;
        a.reference_available = true;
        a.planned = trial.planned;
        a.actual_extent = trial.planned.extent;
        a.candidate = trial.result;
        a.candidate_trace = trial.trace;
        a.provider_reads = trial.provider_reads;
        a.state_bytes = trial.state_bytes;
    }
    a.trials.emplace_back(name, trial);
    STREAM_CHECK(trial.old.failure_code.empty());
    STREAM_CHECK(std::isfinite(trial.old.sharpe));
    STREAM_CHECK(std::isfinite(trial.old.sortino));
    if (!implemented(trial.status)) return false;
    STREAM_CHECK(trial.result.has_value());
    if (!trial.result) return false;
    const auto& got = *trial.result;
    STREAM_CHECK(trial.state_bytes > 0 && trial.state_bytes <= 64 * 1024 + 4096);
    STREAM_CHECK(trial.final_state_bytes == trial.state_bytes);
    STREAM_CHECK(trial.provider_reads == 0);
    STREAM_CHECK(got.receipt.route == MetricsRatioRouteV1::Streaming);
    STREAM_CHECK(got.receipt.streaming_sharpe == StreamingSharpeV1::On);
    STREAM_CHECK(got.receipt.metrics_needed == mt::config().metrics_needed);
    STREAM_CHECK(got.receipt.witness == MetricsExtentWitnessV1::Match);
    STREAM_CHECK(mt::same_extent(got.receipt.actual_extent, trial.planned.extent));
    STREAM_CHECK(got.receipt.eligible_returns == trial.old.m);
    compare_result(trial.old, got, mt::config().metrics_needed);
    STREAM_CHECK(mt::bits(got.downside_sum) == mt::bits(trial.old.down));
    return true;
}

void same_trial_ratio_bits(const RatioTrial& first, const RatioTrial& next) {
    STREAM_CHECK(mt::bits(first.result->sharpe_bar) == mt::bits(next.result->sharpe_bar));
    STREAM_CHECK(mt::bits(first.result->sortino_bar) == mt::bits(next.result->sortino_bar));
}

void determinism_and_tie_rows() {
    const auto points = mt::equities({100., 97., 101., 96., 103., 98.});
    if (row("determinism-repeat-same-process")) {
        // Two complete trials, with the first fold destroyed before the
        // second factory call. RED on the missing streaming-ratio factory, not a control.
        const auto first = run_ratio_trial(points);
        const auto second = run_ratio_trial(points);
        const bool first_ok = record_trial("first", first);
        const bool second_ok = record_trial("second", second);
        if (first_ok && second_ok) same_trial_ratio_bits(first, second);
    }
    if (row("determinism-two-threads")) {
        // Each worker owns its fold and trace; per-point gates interleave
        // trials, and neither fold is destroyed before both finish. Compare
        // both workers to each other AND a separate serial trial. No
        // thread-dependent summation order is allowed per trial.
        const auto single = run_ratio_trial(points);
        ConcurrentTrials concurrent;
        std::array<RatioTrial, 2> parallel;
        std::thread left([&] { parallel[0] = run_ratio_trial(points, &concurrent); });
        std::thread right([&] { parallel[1] = run_ratio_trial(points, &concurrent); });
        left.join();
        right.join();
        const bool single_ok = record_trial("single", single);
        const bool left_ok = record_trial("thread-left", parallel[0]);
        const bool right_ok = record_trial("thread-right", parallel[1]);
        if (left_ok && right_ok) same_trial_ratio_bits(parallel[0], parallel[1]);
        if (single_ok && left_ok) same_trial_ratio_bits(single, parallel[0]);
        if (single_ok && right_ok) same_trial_ratio_bits(single, parallel[1]);
    }
    if (row("determinism-intervening-different-input")) {
        const auto different = mt::equities({256., 384., 480., 240., 180.}, 2 * mt::year);
        const auto first = run_ratio_trial(points);
        const auto between = run_ratio_trial(different);
        const auto last = run_ratio_trial(points);
        STREAM_CHECK(points.count != different.count);
        STREAM_CHECK(mt::bits(first.old.sharpe) != mt::bits(between.old.sharpe));
        const bool first_ok = record_trial("same-before", first);
        record_trial("different-between", between);
        const bool last_ok = record_trial("same-after", last);
        if (first_ok && last_ok) same_trial_ratio_bits(first, last);
    }
    if (row("tie-legacy-bit-equal-different-series")) {
        // Emitted returns are (.5,.25,-.5,-.25) and (.25,-.25,.5,-.5).
        // In both orders the legacy sum/mean is exactly zero and every
        // square/add in the centered second pass is exact. Equal extents
        // give equal rf/ann. Confirm the tie with BOTH independent frozen
        // walks below; no final ratio literal supplies an expected value.
        const auto a = mt::equities({256., 384., 480., 240., 180.});
        const auto b = mt::equities({256., 320., 240., 360., 180.});
        bool different_returns = false;
        for (std::int64_t i = 1; i < a.count; ++i)
            different_returns |= mt::bits(a[i].equity / a[i - 1].equity - 1.)
                != mt::bits(b[i].equity / b[i - 1].equity - 1.);
        STREAM_CHECK(different_returns);
        const auto exact_a = mt::exact_reference(a);
        const auto exact_b = mt::exact_reference(b);
        const auto old_a = mt::legacy(a), old_b = mt::legacy(b);
        STREAM_CHECK(mt::hex_bits(old_a.sharpe) == exact_a.at("legacy_sharpe_bits"));
        STREAM_CHECK(mt::hex_bits(old_b.sharpe) == exact_b.at("legacy_sharpe_bits"));
        STREAM_CHECK(exact_a.at("legacy_sharpe_bits") == exact_b.at("legacy_sharpe_bits"));
        STREAM_CHECK(mt::bits(old_a.sharpe) == mt::bits(old_b.sharpe));
        STREAM_CHECK(exact_a.at("exact_mean") == exact_b.at("exact_mean"));
        STREAM_CHECK(exact_a.at("exact_centered_sum") == exact_b.at("exact_centered_sum"));
        active->exact = "series_a: " + exact_a.at("exact_mean") + " "
            + exact_a.at("exact_centered_sum") + "; series_b: "
            + exact_b.at("exact_mean") + " " + exact_b.at("exact_centered_sum");
        const auto first = run_ratio_trial(a);
        const auto second = run_ratio_trial(b);
        const bool first_ok = record_trial("series-a", first);
        const bool second_ok = record_trial("series-b", second);
        if (first_ok && second_ok) {
            const double x = first.result->sharpe_bar, y = second.result->sharpe_bar;
            STREAM_CHECK(mt::same(x, old_a.sharpe));
            STREAM_CHECK(mt::same(y, old_b.sharpe));
            STREAM_CHECK(mt::same(x, y));
        }
        // HPO resolves equal objectives by the lower trial_id. Its tie-break
        // does not read objective bits beyond equality; last-bit changes
        // that split this legacy tie are constrained by the streaming-ratio ceiling.
    }
}

void numeric(const std::string& name, const mt::PointSource& points,
             MetricsRatioRouteV1 route = MetricsRatioRouteV1::Streaming) {
    if (!row(name)) return;
    candidate(points, mt::plan(points), mt::config(), route);
}

void domains() {
    numeric("n-0-empty", mt::equities({}));
    numeric("n-1", mt::equities({100.}));
    numeric("n-2", mt::equities({100., 97.}));
    numeric("n-3", mt::equities({100., 97., 101.}));
    numeric("n-3-filtered-returns", mt::equities({0., -1., 100.}));
    numeric("nonpositive-span", mt::equities({100., 97., 101.}, 0));
    numeric("negative-span", mt::equities({100., 97., 101.}, -mt::year));
    numeric("constant-zero", mt::equities({1., 1., 1.}));
    numeric("constant-negative", mt::equities({1., .5, .25}));
    numeric("constant-positive", mt::equities({1., 2., 4.}));
    numeric("return-filter-keeps-point-count", mt::equities({100., 0., -1., 98., 102.}));
    numeric("positive-threshold-straddle", mt::equities({1., 1.001, 1.1, 1.1001, 1.3}));
    numeric("alternating", {1000, [](std::int64_t i) {
        return pf_equity_point_t{mt::epoch + i * 60000, i % 2 ? 97. : 101., 0.};
    }});
    numeric("long-tail", {1000, [](std::int64_t i) {
        return pf_equity_point_t{mt::epoch + i * 60000,
                                i == 999 ? 100000. : 100. + (i % 7), 0.};
    }});
    // Separate rows keep the million-point walks bounded in elapsed time.
    numeric("flat-1000", {1000, [](std::int64_t i) {
        return pf_equity_point_t{mt::epoch + i * 60000, 100., 0.};
    }});
    numeric("flat-1000000", {1000000, [](std::int64_t i) {
        return pf_equity_point_t{mt::epoch + i * 60000, 100., 0.};
    }});
    // The legacy variance pass is itself inexact here: over the 999,997 exact
    // zero returns its accumulator sits in one binade (spacing 2^-63) and each
    // addition of mean^2 (about 32244.135 spacings) advances it by exactly
    // 32244 spacings, so it loses about 1.464e-14 against a centered sum of
    // about 1.748e-3. An exact-variance Sharpe therefore differs from the
    // legacy bits by about 4.19e-12 relative, so the ordered replay is required
    // (the legacy fold over the stored returns, in the legacy order), and an
    // ideal centered fold would not be bit-identical. The oracle values
    // below are checked at run time; the mean screen itself passes.
    if (row("flat-between-nonflat-ends-1000000")) {
        const mt::PointSource points{1000000, [](std::int64_t i) {
            return pf_equity_point_t{mt::epoch + i * 60000,
                                    i == 0 ? 103. : i == 999999 ? 97. : 100., 0.};
        }};
        const auto exact = mt::exact_reference(points);
        const double legacy = std::stod(exact.at("legacy_sharpe"));
        const double ideal = std::stod(exact.at("legacy_numerator_exact_variance_sharpe_rounded"));
        STREAM_CHECK(exact.at("mean_screen_pass") == "1");
        STREAM_CHECK(std::abs((ideal - legacy) / legacy) > 1e-12);
        active->exact = exact.at("exact_mean") + " " + exact.at("exact_sample_variance");
        candidate(points, mt::plan(points), mt::config(), MetricsRatioRouteV1::Streaming);
    }
    numeric("timestamp-conversion-2pow53", mt::source({
        {9007199254740991LL, 100., 0.}, {9007199254740993LL, 97., 0.},
        {9007199254740995LL, 101., 0.}}));
    numeric("timestamp-full-int64-span", mt::source({
        {std::numeric_limits<std::int64_t>::min(), 100., 0.}, {0, 97., 0.},
        {std::numeric_limits<std::int64_t>::max(), 101., 0.}}));
}
double raw_moment_mutant(const mt::PointSource& points) {
    const auto ref = mt::legacy(points, false);
    double sum = 0., squared = 0.;
    std::size_t m = 0;
    for (std::int64_t i = 1; i < points.count; ++i) {
        if (!(points[i - 1].equity > 0.)) continue;
        const double r = points[i].equity / points[i - 1].equity - 1.;
        sum += r;
        squared += r * r;
        ++m;
    }
    const double var = squared - sum * sum / static_cast<double>(m);
    return var > 0. ? (sum / static_cast<double>(m) - ref.rf)
        / std::sqrt(var / static_cast<double>(m - 1)) * ref.ann : mt::nan;
}
void exact_rows() {
    // A=2^20, h=2^-20. r=(A-1,A-1+h), mean=A-1+h/2,
    // deltas=(-2^-21,+2^-21), centered sum=2^-41. Each operation
    // is exactly representable. The raw-moment mutant cancels to zero.
    const auto huge = mt::equities({1., 0x1p20, 0x1p40 + 1.});
    if (row("huge-mean-tiny-variance")) {
        const auto exact = mt::exact_reference(huge);
        const auto want = mt::legacy(huge);
        active->exact = exact.at("exact_centered_sum");
        STREAM_CHECK(mt::hex_bits(want.sharpe) == exact.at("legacy_sharpe_bits"));
        STREAM_CHECK(mt::hex_bits(want.sortino) == exact.at("legacy_sortino_bits"));
        STREAM_CHECK(std::isfinite(want.sharpe));
        STREAM_CHECK(std::isnan(raw_moment_mutant(huge)));
        if (mutant == "raw") {
            active->mutation = "raw_moments";
            active->old = want;
            active->reference_available = true;
            active->planned = mt::plan(huge);
            active->actual_extent = mt::extent(huge);
            active->mutant_result = want;
            active->mutant_result->sharpe = raw_moment_mutant(huge);
            // Deliberately RED numerical mutation, even with the base library.
            STREAM_CHECK(mt::same(raw_moment_mutant(huge), std::stod(exact.at("legacy_sharpe"))));
        } else candidate(huge, mt::plan(huge), mt::config(), MetricsRatioRouteV1::Streaming);
    }
    // R=2^20-1, h=2^-32; r=(R,R,R+h). Exact mu=R+h/3,
    // sample variance=h^2/3. Legacy sum ties to 3R, mean=R, var=h^2/2.
    // Even an ideal centered fold differs from the legacy bits by more than 1e-12:
    // the ordered replay (the legacy fold itself) is required.
    const auto high = mt::equities({1., 0x1p20, 0x1p40, 0x1p60 + 0x1p8});
    if (row("kappa-nonrepresentable-mean")) {
        const auto exact = mt::exact_reference(high);
        const double legacy = std::stod(exact.at("legacy_sharpe"));
        const double ideal = std::stod(exact.at("legacy_numerator_exact_variance_sharpe_rounded"));
        STREAM_CHECK(std::abs((ideal - legacy) / legacy) > 1e-12);
        active->kappa = std::stod(exact.at("kappa_rounded"));
        active->exact = exact.at("exact_mean") + " " + exact.at("exact_sample_variance");
        candidate(high, mt::plan(high), mt::config(), MetricsRatioRouteV1::Streaming);
    }
    for (double target : {1e3, 1e6, 1e9, 1e12, 1e15}) {
        for (int order = 0; order < 3; ++order) {
            const std::string name = "kappa-sweep-" + mt::printed(target, 1)
                + "-order-" + std::to_string(order);
            if (!row(name)) continue;
            std::vector<double> values{1.};
            // Realizable equities, not supplied return literals. Different
            // multiplication/division rounding is decoded by the Python oracle.
            for (int i = 0; i < 7; ++i) {
                const int sign = (i + order) % 3 - 1;
                values.push_back(values.back() * (2. + sign / target));
            }
            const auto points = mt::equities(std::move(values));
            const auto exact = mt::exact_reference(points);
            STREAM_CHECK(mt::hex_bits(mt::legacy(points).sharpe) == exact.at("legacy_sharpe_bits"));
            if (exact.count("kappa_rounded"))
                active->kappa = std::stod(exact.at("kappa_rounded"));
            active->exact = exact.at("exact_mean");
            candidate(points, mt::plan(points), mt::config(),
                MetricsRatioRouteV1::Streaming);
        }
    }
}
void wrong_plans() {
    const auto points = mt::equities({100., 97., 101., 96., 103., 98.});
    for (const auto& kind : {"count-small", "count-large", "first-only", "last-only",
                             "empty", "attempt", "generation", "view", "spec"}) {
        if (!row(std::string("wrong-plan-") + kind)) continue;
        auto p = mt::plan(points);
        const std::string k = kind;
        if (k == "count-small") --p.extent.count;
        if (k == "count-large") ++p.extent.count;
        if (k == "first-only") ++p.extent.first_time_ms;
        if (k == "last-only") --p.extent.last_time_ms;
        if (k == "empty") { p.extent = {}; }
        if (k == "attempt") ++p.attempt;
        if (k == "generation") ++p.input_generation;
        if (k == "view") ++p.admitted_view;
        if (k == "spec") ++p.spec_identity;
        candidate(points, p, mt::config(), MetricsRatioRouteV1::LegacyActualCurve);
    }
    if (row("wrong-plan-same-rounded-density")) {
        const auto points2 = mt::source({
            {0, 100., 0.}, {4503599627370496LL, 97., 0.},
            {9007199254740992LL, 101., 0.}});
        auto p = mt::plan(points2);
        ++p.extent.last_time_ms; // 2^53+1 converts to the same binary64.
        STREAM_CHECK(static_cast<double>(p.extent.last_time_ms)
                 == static_cast<double>(points2[2].time_ms));
        candidate(points2, p, mt::config(), MetricsRatioRouteV1::LegacyActualCurve);
    }
    if (row("wrong-plan-nonempty-to-empty"))
        candidate(mt::equities({}), mt::plan(points), mt::config(),
                  MetricsRatioRouteV1::LegacyActualCurve);
    if (row("missing-plan-sortino")) {
        auto cfg = mt::config(); cfg.metrics_needed = kMetricsNeedSortinoBarV1;
        candidate(points, {}, cfg, MetricsRatioRouteV1::LegacyActualCurve);
    }
}
void switches_and_needs() {
    const auto points = mt::equities({100., 97., 101., 96.});
    if (row("switch-OFF-exact-legacy-control")) {
        // BASELINE PASS BY CONSTRUCTION on feae3369: explicitly use the
        // preserved curve producer whose bits OFF must retain. This control
        // isolates numeric identity; switch-OFF-sharpe/both below exercise
        // the still-missing streaming-ratio switch plumbing and must remain red.
        std::vector<pf_equity_point_t> curve;
        for (std::int64_t i = 0; i < points.count; ++i) curve.push_back(points[i]);
        const auto want = mt::legacy(points);
        // A const pointer selects the library's pointer overload; a non-const one
        // would deduce the unexported PointSource template (link error).
        const auto got = compute_equity_stats(
            static_cast<const pf_equity_point_t*>(curve.data()), points.count,
            curve.front().equity, "UTC", 1., 1., 0, 0.);
        active->legacy_control = true;
        active->old = want;
        active->reference_available = true;
        active->actual_extent = mt::extent(points);
        active->curve_control = got;
        STREAM_CHECK(want.failure_code.empty());
        STREAM_CHECK(std::isfinite(want.sharpe) && std::isfinite(want.sortino));
        STREAM_CHECK(mt::bits(got.sharpe_bar) == mt::bits(want.sharpe));
        STREAM_CHECK(mt::bits(got.sortino_bar) == mt::bits(want.sortino));
    }
    for (auto mode : {StreamingSharpeV1::Off, StreamingSharpeV1::On}) {
        for (const auto& need : std::vector<std::pair<std::string, MetricsNeedMaskV1>>{
                {"sharpe", kMetricsNeedSharpeBarV1}, {"sortino", kMetricsNeedSortinoBarV1},
                {"both", kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1},
                {"returns", kMetricsNeedReturnsV1 | kMetricsNeedSortinoBarV1},
                {"returns-sharpe", kMetricsNeedReturnsV1 | kMetricsNeedSharpeBarV1},
                {"curve", kMetricsNeedCurveV1}, {"trades", kMetricsNeedTradeStatsV1},
                {"summary", kMetricsNeedEquitySummaryV1}, {"monthly", kMetricsNeedMonthlyRatiosV1},
                {"all", kMetricsNeedAllV1}, {"none", 0}}) {
            const std::string name = std::string("switch-")
                + (mode == StreamingSharpeV1::Off ? "OFF-" : "ON-") + need.first;
            if (!row(name)) continue;
            auto cfg = mt::config();
            cfg.streaming_sharpe = mode;
            cfg.metrics_needed = need.second;
            // Returns (including returns.* objectives) retains the curve
            // with either switch. Without SharpeBar, OFF alone cannot
            // require it; compare_result permits an unread Sharpe to be skipped.
            const bool curve = (need.second & (kMetricsNeedReturnsV1 | kMetricsNeedCurveV1
                                               | kMetricsNeedMonthlyRatiosV1))
                || (mode == StreamingSharpeV1::Off
                    && (need.second & kMetricsNeedSharpeBarV1));
            if (candidate(points, mt::plan(points), cfg, curve
                    ? MetricsRatioRouteV1::LegacyActualCurve : MetricsRatioRouteV1::Streaming)) {
                STREAM_CHECK(active->candidate->receipt.streaming_sharpe == mode);
                STREAM_CHECK(active->candidate->receipt.metrics_needed == need.second);
            }
        }
    }
    if (row("switch-default-ON")) {
        MetricsStreamingConfigV1 cfg; // Deliberately no mode assignment.
        STREAM_CHECK(cfg.streaming_sharpe == StreamingSharpeV1::On);
        cfg.metrics_needed = kMetricsNeedSharpeBarV1;
        // Sharpe uses actual extents and needs no advance Sortino plan.
        candidate(points, {}, cfg, MetricsRatioRouteV1::Streaming);
    }
    for (bool c_export : {false, true}) {
        if (!row(c_export ? "switch-default-C-export-ON" : "switch-default-engine-ON")) continue;
        mt::CompletedMarkHost host({}); // No configure, run or switch setter.
        MetricsExtentReceiptV1 receipt;
        // Poison the output so an untouched default-ON POD cannot prove the
        // engine/export default. first-stage is RED at the NotImplemented getter.
        receipt.streaming_sharpe = StreamingSharpeV1::Off;
        receipt.metrics_needed = 0;
        const auto status = c_export
            ? static_cast<MetricsStreamingStatusV1>(strategy_metrics_extent_receipt_v1(&host, &receipt))
            : host.metrics_extent_receipt_v1(&receipt);
        if (implemented(status)) {
            STREAM_CHECK(receipt.streaming_sharpe == StreamingSharpeV1::On);
            STREAM_CHECK(receipt.metrics_needed == kMetricsNeedAllV1);
        }
    }
    for (auto mode : {StreamingSharpeV1::Off, StreamingSharpeV1::On}) {
        if (!row(std::string("need-mask-C-export-")
                 + (mode == StreamingSharpeV1::Off ? "OFF" : "ON"))) continue;
        mt::CompletedMarkHost host({});
        STREAM_CHECK(host.configure_native(mt::native_spec()).status == NativeSetupStatus::Applied);
        const auto mask = kMetricsNeedSharpeBarV1 | kMetricsNeedReturnsV1;
        implemented(static_cast<MetricsStreamingStatusV1>(strategy_set_metrics_streaming_v1(
            &host, static_cast<std::uint32_t>(mode), mask)));
        const auto bars = mt::stop_feed(true);
        host.run(bars.data(), static_cast<int>(bars.size()));
        MetricsExtentReceiptV1 receipt;
        const auto status = static_cast<MetricsStreamingStatusV1>(
            strategy_metrics_extent_receipt_v1(&host, &receipt));
        if (implemented(status)) {
            STREAM_CHECK(receipt.metrics_needed == mask);
            STREAM_CHECK(receipt.streaming_sharpe == mode);
            STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
            STREAM_CHECK(receipt.actual_extent.count == static_cast<std::int64_t>(bars.size()));
        }
    }
}

void stop_rows() {
    for (bool margin : {false, true}) {
        for (const auto& stop : std::vector<std::pair<const char*, int>>{
                {"3", 3}, {"half", 6}, {"last", 11}}) {
            if (!row(std::string(margin ? "M-" : "E-") + stop.first)) continue;
            const mt::StopSpec injection{stop.second, mt::Injection::CompletedMark, margin, false};
            const auto old = mt::run_stop(injection);
            active->source_identity = "actual_stopped_presented_metric_curve";
            active->original_code = old.code; active->original_args = old.args;
            active->original_message = old.message; active->boundary = old.boundary;
            STREAM_CHECK(old.curve.size() == static_cast<std::size_t>(stop.second));
            STREAM_CHECK(old.boundary.completed_marks == stop.second);
            STREAM_CHECK(old.boundary.kind == MetricsStopBoundaryKindV1::CompletedMark);
            STREAM_CHECK(old.message.find("streaming-ratio injected host stop") != std::string::npos);
            STREAM_CHECK(!old.code.empty());
            STREAM_CHECK(!margin || old.margins == 1);
            const auto actual = old.points();
            const auto reference = mt::legacy(actual);
            STREAM_CHECK(std::isfinite(reference.sharpe) && std::isfinite(reference.sortino));
            STREAM_CHECK(mt::same(old.metrics.equity.sharpe_bar, reference.sharpe));
            STREAM_CHECK(mt::same(old.metrics.equity.sortino_bar, reference.sortino));
            const auto full_bars = mt::stop_feed();
            std::vector<pf_equity_point_t> planned_points;
            for (const auto& bar : full_bars) planned_points.push_back({bar.timestamp, 1., 0.});
            const auto planned = mt::plan(mt::source(std::move(planned_points)));
            STREAM_CHECK(!mt::same_extent(planned.extent, mt::extent(actual)));
            // Stored curve: both ratios must be the frozen actual-prefix bits.
            candidate(actual, planned, mt::config(), MetricsRatioRouteV1::LegacyActualCurve);
            // Separate executed adapter; no hardcoded execution/publication
            // receipt is accepted in place of invoking the host.
            const auto replay = mt::run_replay_adapter(injection, planned);
            active->executions = replay.executions;
            active->publications = replay.publications;
            if (implemented(replay.status)) {
                active->replay_receipt = replay.receipt;
                active->replay_report = replay.authoritative;
                STREAM_CHECK(replay.executions == 2 && replay.publications == 1);
                STREAM_CHECK(replay.first_curve_capacity <= 1);
                STREAM_CHECK(replay.receipt.route == MetricsRatioRouteV1::LegacyRerun);
                STREAM_CHECK(replay.receipt.replay_attempts == 1);
                STREAM_CHECK(mt::same_run(old, replay.authoritative));
                STREAM_CHECK(replay.authoritative.curve.size() == old.curve.size());
                STREAM_CHECK(replay.receipt.stop_boundary.completed_marks == stop.second);
            }
        }
    }
    for (const auto& shape : {"E-intra-bar", "M-inside-margin-callback",
                              "margin-continues", "regular-spacing-prefix-witness",
                              "unreplayable-host-curve", "cooperative-cancel-prefix"}) {
        if (!row(shape)) continue;
        const std::string name = shape;
        const bool margin = name == "M-inside-margin-callback" || name == "margin-continues";
        mt::StopSpec stop{6, mt::Injection::CompletedMark, margin,
                          name == "regular-spacing-prefix-witness"};
        if (name == "E-intra-bar") stop.injection = mt::Injection::IntraBar;
        if (name == "M-inside-margin-callback") stop.injection = mt::Injection::MarginCallback;
        if (name == "margin-continues") stop.injection = mt::Injection::None;
        if (name == "cooperative-cancel-prefix") stop.injection = mt::Injection::Cooperative;
        const auto result = mt::run_stop(stop);
        const auto actual = result.points();
        active->boundary = result.boundary;
        active->original_code = result.code; active->original_args = result.args;
        if (stop.injection == mt::Injection::IntraBar
            || stop.injection == mt::Injection::MarginCallback)
            STREAM_CHECK(result.curve.size() == 5);
        if (name == "margin-continues") {
            STREAM_CHECK(result.margins == 1);
            STREAM_CHECK(result.curve.size() == 12);
            STREAM_CHECK(result.lifecycle == NativeLifecycleKind::Completed && result.status == 0);
        }
        auto p = mt::plan(actual);
        // unreplayable-host-curve: the pure-fold call keeps the matching plan,
        // so only deterministic_replay = 0 can force the legacy route there.
        const auto p_match = p;
        if (name != "margin-continues" && name != "regular-spacing-prefix-witness") {
            // A stopped run's plan is the planned full extent (as in the
            // CompletedMark loop above), not the stopped prefix.
            std::vector<pf_equity_point_t> planned_points;
            for (const auto& bar : mt::stop_feed()) planned_points.push_back({bar.timestamp, 1., 0.});
            p = mt::plan(mt::source(std::move(planned_points)));
            STREAM_CHECK(!mt::same_extent(p.extent, mt::extent(actual)));
        }
        if (name == "regular-spacing-prefix-witness") {
            p.extent.count = 12;
            p.extent.last_time_ms = mt::epoch + 11 * 60000;
            // Regular minute prefixes can have equal OR adjacent rounded rf
            // operands. The exact equal-rf control below uses integer years.
            STREAM_CHECK(p.extent.count != actual.count);
        }
        auto cfg = mt::config();
        if (name == "unreplayable-host-curve") cfg.deterministic_replay = 0;
        candidate(actual, name == "unreplayable-host-curve" ? p_match : p, cfg,
                  name == "margin-continues"
            ? MetricsRatioRouteV1::Streaming : MetricsRatioRouteV1::LegacyActualCurve);
        if (name == "unreplayable-host-curve") {
            const auto unreplayable = mt::run_replay_adapter(stop, p, false);
            active->executions = unreplayable.executions;
            active->publications = unreplayable.publications;
            if (implemented(unreplayable.status)) {
                STREAM_CHECK(unreplayable.executions == 1 && unreplayable.publications == 1);
                STREAM_CHECK(unreplayable.receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
                STREAM_CHECK(mt::same_run(result, unreplayable.authoritative));
            }
        }
        if (name == "cooperative-cancel-prefix") {
            STREAM_CHECK(result.boundary.completed_marks == 6 && result.curve.size() == 6);
            STREAM_CHECK(result.status != 0 && !result.code.empty());
        }
    }
    if (row("regular-spacing-exact-equal-rf-witness")) {
        mt::PointSource points{6, [](std::int64_t i) {
            return pf_equity_point_t{mt::epoch + i * mt::year, i % 2 ? 97. : 101., 0.};
        }};
        auto p = mt::plan(points);
        p.extent.count = 12; p.extent.last_time_ms = mt::epoch + 11 * mt::year;
        STREAM_CHECK(mt::same(mt::legacy(points).rf, .02 / (11. / 11.)));
        candidate(points, p, mt::config(), MetricsRatioRouteV1::LegacyActualCurve);
    }
    // SIGTERM / timeout that kills a trial => NO REPORT AT ALL, as today.
    // It has no ratio row, no route total, no finalize and no replay result.
}

void overflow_rows() {
    for (bool return_overflow : {true, false}) {
        if (!row(return_overflow ? "O-RETURN" : "O-ORDER")) continue;
        const std::vector<double> values = return_overflow
            ? std::vector<double>{1., -1., 0x1p-600, 0x1p600, 1.}
            : std::vector<double>{1., -0x1p600, 1., 2.};
        std::vector<pf_equity_point_t> curve;
        for (std::size_t i = 0; i < values.size(); ++i)
            curve.push_back({static_cast<std::int64_t>(i) * mt::day, values[i], 0.});
        const auto points = mt::source(std::move(curve));
        STREAM_CHECK(mt::overflow_prefix_is_finite(points));
        const auto want = mt::legacy(points);
        STREAM_CHECK(want.failure_code == "report_result_invalid");
        STREAM_CHECK(want.failure_field == "metrics.equity.sharpe_bar");
        STREAM_CHECK(want.failure_reason == "nonfinite");
        STREAM_CHECK(want.failure_index == (return_overflow ? 1 : 0));
        STREAM_CHECK(want.failure_operation == (return_overflow
            ? "promote.return.first_pass" : "variance.add_square"));
        // Matching extent is mandatory here: a downside check moved into
        // the first pass must not outrank the old variance failure.
        candidate(points, mt::plan(points), mt::config(), MetricsRatioRouteV1::Streaming);
    }
}

MetricsPlainDeclarationV1 plain_declaration() {
    MetricsPlainDeclarationV1 d;
    d.builtin_presenter = d.builtin_marks = d.builtin_scheduler = 1;
    return d;
}
MetricsPlainInputsV1 plain_inputs(const MetricsPlainDeclarationV1* declaration) {
    MetricsPlainInputsV1 p;
    p.declaration = declaration;
    p.module_contract_bound = p.historical_batch = p.equal_resolved_timeframes = 1;
    p.intrabar_none = p.builtin_host_marks = p.feed_tolerant_passthrough = 1;
    p.final_admitted_view_bound = 1;
    return p;
}
void plain_rows() {
    const auto points = mt::equities({100., 97., 101.});
    for (const auto& kind : {"plain", "old-library", "overridden-presenter",
            "overridden-marks", "security", "lower-tf", "declared-magnifier",
            "begin-magnifier", "intrabar", "every-point", "history-tick",
            "stream", "unequal-tf", "undetected", "pending-selection",
            "admitted-selection", "realtime-tail", "probe-tail", "unbound-view"}) {
        if (!row(std::string("PLAIN-") + kind)) continue;
        auto d = plain_declaration();
        auto p = plain_inputs(&d);
        const std::string k = kind;
        if (k == "old-library") p.declaration = nullptr;
        if (k == "overridden-presenter") d.builtin_presenter = 0;
        if (k == "overridden-marks") d.builtin_marks = 0;
        if (k == "security") d.security_sites = 1;
        if (k == "lower-tf") d.lower_tf_sites = 1;
        if (k == "declared-magnifier") d.magnifier_declared = 1;
        if (k == "begin-magnifier") p.begin_bar_magnifier = 1;
        if (k == "intrabar") p.intrabar_none = 0;
        if (k == "every-point") p.calculation_trigger = 2;
        if (k == "history-tick") d.calc_on_every_history_tick = 1;
        if (k == "stream") p.historical_batch = 0;
        if (k == "unequal-tf") p.equal_resolved_timeframes = 0;
        if (k == "undetected") p.timeframe_undetected = 1;
        if (k == "pending-selection") p.pending_selected_window = 1;
        if (k == "admitted-selection") p.admitted_selected_window = 1;
        if (k == "realtime-tail") p.realtime_tail = 1;
        if (k == "probe-tail") p.probe_suppress_tail_logic = 1;
        if (k == "unbound-view") p.final_admitted_view_bound = 0;
        active->plain = k == "plain" ? "yes" : "no";
        active->predicate_inputs = k;
        active->planned = mt::plan(points);
        mt::CompletedMarkHost host({});
        MetricsExtentReceiptV1 receipt;
        if (!implemented(host.admit_metrics_plain_v1(p, active->planned, &receipt))) continue;
        STREAM_CHECK(receipt.plain_subset == (k == "plain" ? 1u : 0u));
        STREAM_CHECK(receipt.failed_plain_inputs == 0 || k != "plain");
        if (k != "plain") {
            STREAM_CHECK(receipt.failed_plain_inputs != 0);
            STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        }
    }
}

// P-OLD controls run all three entry families with two invalid fields, before
// and after a begun run. The candidate config call is before the invalid run,
// but admission may not read the plan/feed before today's option/bar checks.
void preflight_rows() {
    for (int entry = 0; entry < 3; ++entry) {
        for (bool reused : {false, true}) {
            if (!row("P-OLD-entry-" + std::to_string(entry)
                     + (reused ? "-reused" : "-ready"))) continue;
            mt::CompletedMarkHost old({}), next({});
            const auto spec = mt::native_spec();
            STREAM_CHECK(old.configure_native(spec).status == NativeSetupStatus::Applied);
            STREAM_CHECK(next.configure_native(spec).status == NativeSetupStatus::Applied);
            auto bars = mt::stop_feed(true);
            if (reused) {
                old.run(bars.data(), static_cast<int>(bars.size()));
                next.run(bars.data(), static_cast<int>(bars.size()));
                auto fresh = spec;
                fresh.identity.run_number = 2;
                STREAM_CHECK(old.configure_native(fresh).status == NativeSetupStatus::Applied);
                STREAM_CHECK(next.configure_native(fresh).status == NativeSetupStatus::Applied);
            }
            const auto before = next.begun;
            const bool ready = implemented(next.set_metrics_streaming_v1(
                StreamingSharpeV1::On, kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1));
            bars[0].open = std::numeric_limits<double>::infinity();
            bars[0].low = bars[0].high + 1.; // Independent second invalid field.
            const auto drive = [&](mt::CompletedMarkHost& host) {
                if (entry == 0) host.run(bars.data(), static_cast<int>(bars.size()));
                if (entry == 1) host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
                if (entry == 2) host.run(bars.data(), static_cast<int>(bars.size()),
                    "1", "1", InputsMap{}, SymInfo{});
            };
            drive(old); drive(next);
            const auto control = mt::capture(old), got = mt::capture(next);
            active->original_code = control.code; active->original_args = control.args;
            STREAM_CHECK(control.code == "chart_bars_rejected");
            STREAM_CHECK(control.args.find("0") != std::string::npos);
            STREAM_CHECK(next.begun == before);
            STREAM_CHECK(control.curve.empty() && got.curve.empty());
            STREAM_CHECK(mt::same_run(control, got));
            if (ready) {
                MetricsExtentReceiptV1 receipt;
                STREAM_CHECK(next.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
            }
        }
    }
    if (row("P-OLD-options-before-bars")) {
        mt::CompletedMarkHost host({});
        STREAM_CHECK(host.configure_native(mt::native_spec()).status == NativeSetupStatus::Applied);
        auto bars = mt::stop_feed();
        bars[0].open = std::numeric_limits<double>::infinity();
        implemented(host.set_metrics_streaming_v1(StreamingSharpeV1::On, kMetricsNeedSortinoBarV1));
        host.run(bars.data(), static_cast<int>(bars.size()), "5", "1");
        const auto got = mt::capture(host);
        STREAM_CHECK(got.code == "run_options_rejected");
        STREAM_CHECK(got.args == "{\"option\":\"timeframes\"}");
        STREAM_CHECK(host.begun == 0 && got.curve.empty());
    }
}

void held_row(const std::string& name, const mt::HeldScript& script) {
    if (!row(name)) return;
    const auto points = mt::source(script.final);
    const auto want = mt::legacy(points);
    const auto wrong = mt::legacy(mt::append_time_mutant(script));
    active->source_identity = "final_amended_recorded_curve_F";
    active->planned = mt::plan(points);
    active->actual_extent = mt::extent(points);
    STREAM_CHECK(mt::same_extent(mt::extent(mt::append_time_mutant(script)), mt::extent(points)));
    STREAM_CHECK(!mt::same(want.down, wrong.down));
    STREAM_CHECK(!mt::same(want.sortino, wrong.sortino));
    if (mutant == "append") {
        active->old = want;
        active->reference_available = true;
        active->mutation = "append_time_fold_R";
        active->mutant_result = wrong;
        // Numerical red receipt independent of missing production streaming-ratio.
        STREAM_CHECK(mt::same(wrong.down, want.down));
        STREAM_CHECK(mt::same(wrong.sortino, want.sortino));
        return;
    }
    auto cfg = mt::config();
    cfg.metrics_needed = kMetricsNeedSortinoBarV1;
    if (candidate(points, mt::plan(points), cfg, MetricsRatioRouteV1::Streaming, &script))
        STREAM_CHECK(mt::same(active->candidate->downside_sum, want.down));
}
void held_synthetic_rows() {
    // Include an amended *preceding* held slot before the final mark, then
    // two amendments of the last held point. The final return uses AMENDED
    // eq_last/eq_prev-1; the first one is also affected by the earlier edit.
    mt::HeldScript script;
    script.append({mt::epoch, 100000., 0.});
    script.append({mt::epoch + mt::day, 100800., 800.});
    script.amend(1, {mt::epoch + mt::day, 100600., 0.});
    script.amend(1, {mt::epoch + mt::day, 100500., 0.});
    script.append({mt::epoch + 2 * mt::day, 100100., 0.});
    held_row("held-twice-before-next-mark", script);
    mt::HeldScript provisional;
    provisional.append({mt::epoch, 100., 0.});
    provisional.append({mt::epoch + mt::day, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::infinity()});
    provisional.amend(1, {mt::epoch + mt::day, 97., 0.});
    provisional.append({mt::epoch + 2 * mt::day, 101., 0.});
    held_row("held-provisional-nonfinite-cleared", provisional);
    if (row("held-release-invariant")) {
        const pid_t child = fork();
        STREAM_CHECK(child >= 0);
        if (child == 0) {
            mt::HeldScript illegal;
            illegal.append({0, 100., 0.}); illegal.append({1, 101., 0.});
            illegal.amend(0, {0, 99., 0.}); // Must assert; never catches.
            _exit(0);
        }
        if (child > 0) {
            int status = 0;
            STREAM_CHECK(waitpid(child, &status, 0) == child);
            STREAM_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        }
        // A production debug invariant is also required, not just a fixture
        // assert. The absent factory makes this row red in first stage.
        std::unique_ptr<StreamingEquityRatiosV1> fold;
        if (implemented(make_streaming_equity_ratios_v1(
                mt::config(), mt::plan(mt::source(script.final)), {}, fold))) {
            const pid_t production = fork();
            STREAM_CHECK(production >= 0);
            if (production == 0) {
                fold->append(script.final[0], 0); fold->append(script.final[1], 0);
                fold->amend_held(0, script.final[0], 0);
                _exit(0);
            }
            if (production > 0) {
                int status = 0;
                STREAM_CHECK(waitpid(production, &status, 0) == production);
                STREAM_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
            }
        }
    }
}

// These two executables are registered only with the source layer enabled.
// The existing test_metrics_fold/test_metrics_overflow retain kernel rows.
class CommissionHost : public source::PineStrategyHost {
public:
    explicit CommissionHost(bool coof = false) : coof_(coof) {
        source::PineStrategyConfig c;
        c.initial_capital = 100000.; c.default_qty_value = 100.;
        c.pyramiding = 2; c.process_orders_on_close = true;
        c.commission_value = .1; c.calc_on_order_fills = coof;
        configure_pine_strategy(c);
    }
    mt::HeldScript script;
    int terminal_amendments = 0;
    void reconcile(bool forced_amend = false) {
        for (std::size_t i = script.final.size(); i < equity_curve_.size(); ++i)
            script.append(equity_curve_[i]);
        if (!script.final.empty() && !equity_curve_.empty()) {
            const auto& last = equity_curve_.back();
            if (forced_amend || !mt::same(last.equity, script.final.back().equity)
                || !mt::same(last.open_profit, script.final.back().open_profit))
                script.amend(static_cast<std::int64_t>(script.final.size()) - 1, last);
        }
    }
    void on_source_bar(const Bar&) override {
        reconcile();
        if (pine_bar_index() == 0) strategy_entry("L1", true);
        if (pine_bar_index() == 2) strategy_entry("L2", true);
        if (coof_ && pine_bar_index() == 4 && !script.final.empty()) {
            // Two real M06 -> M05 calls BEFORE this source slot is marked.
            // The second write is deliberately idempotent: finalize/reentry
            // must not add the held return twice.
            const auto count = equity_curve_.size();
            source::ReportSinkEquityTestAccess::record_range_end(*this);
            reconcile(true); ++terminal_amendments;
            source::ReportSinkEquityTestAccess::record_range_end(*this);
            reconcile(true); ++terminal_amendments;
            assert(equity_curve_.size() == count);
        }
    }
    void complete_script(const std::vector<Bar>& bars) {
        reconcile();
        if (!coof_ && !script.final.empty()) {
            // Independently reconstruct the final gross append-time mark:
            // 100 units at 100, 100 at 102, terminal close 105. Net M05
            // equity is 100758.8; R's equity is 100800.
            auto final = script.final;
            script = {};
            for (std::size_t i = 0; i < final.size(); ++i) {
                auto p = final[i];
                if (i + 1 == final.size()) {
                    p.open_profit = (bars.back().close - bars[0].close) * 100.
                        + (bars.back().close - bars[2].close) * 100.;
                    p.equity = 100000. + p.open_profit;
                }
                script.append(p);
            }
            script.amend(static_cast<std::int64_t>(final.size()) - 1, final.back());
        }
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("streaming-commission/v1");
        sink.i(coof_); sink.i(terminal_amendments);
        // The event log is observation-only; strategy decisions use coof_
        // and source bar index. Hash it nevertheless for test replay audits.
        sink.i(script.events.size());
        for (const auto& e : script.events) {
            sink.i(e.amendment); sink.i(e.index); sink.i(e.point.time_ms);
            sink.d(e.point.equity); sink.d(e.point.open_profit);
        }
    }
    bool coof_;
};
void amended_pine_rows() {
    std::vector<Bar> bars;
    const double prices[] = {100., 90., 102., 108., 105.};
    for (int i = 0; i < 5; ++i)
        bars.push_back({prices[i], prices[i], prices[i], prices[i], 1.,
                        mt::epoch + i * 3600000LL});
    for (bool coof : {false, true}) {
        const std::string name = coof ? "amended-held-COOF-twice" : "amended-held-commission";
        if (list_only || (!filter.empty() && name.find(filter) == std::string::npos)) {
            if (list_only) row(name);
            continue;
        }
        CommissionHost host(coof);
        host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
        host.complete_script(bars);
        held_row(name, host.script);
        STREAM_CHECK(host.last_error().empty());
        const auto report = mt::capture(host);
        STREAM_CHECK(report.curve.size() == bars.size());
        const double net = (105. - 100.) * 100. - (100. + 105.) * 100. * .1 / 100.
                         + (105. - 102.) * 100. - (102. + 105.) * 100. * .1 / 100.;
        STREAM_CHECK(mt::same(report.curve.back().equity, 100000. + net));
        if (coof) STREAM_CHECK(host.terminal_amendments >= 2);
    }
}

class CurveHost final : public NativeStrategyHost {
public:
    int callbacks = 0, secondary = 0, subbars = 0, fills = 0;
    bool edit_earlier = false, hide_suffix = false, amend_time = false;
    bool prepare_failure = false;
    mutable std::vector<pf_equity_point_t> authoritative;
    void prepare_native_begin(const NativeBeginArgs&) override {
        if (prepare_failure) throw std::runtime_error("streaming-ratio preparation error");
    }
    void on_native_run_begin() override { callbacks = secondary = subbars = fills = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++callbacks; }
    void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) override {
        ++secondary;
    }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { ++subbars; }
    void present_report(ReportC* out) const override {
        if (out->equity_curve_len > 2) {
            if (edit_earlier) out->equity_curve[1].equity *= .97;
            if (amend_time) out->equity_curve[out->equity_curve_len - 1].time_ms += 60000;
        }
        authoritative.clear();
        if (out->equity_curve_len)
            authoritative.assign(out->equity_curve, out->equity_curve + out->equity_curve_len);
        if (hide_suffix && out->equity_curve_len > 2) --out->equity_curve_len;
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        sink.s("streaming-curve-host/v1");
        sink.i(callbacks); sink.i(secondary); sink.i(subbars); sink.i(fills);
        sink.i(edit_earlier); sink.i(hide_suffix); sink.i(amend_time);
        sink.i(prepare_failure);
        // authoritative is a report observation, never an execution input.
    }
};
void curve_route_rows() {
    for (const auto& kind : {"trim-after", "plan-before-trim", "tail-appended", "gaps",
            "lazy-seal", "EOF-complete", "EOF-incomplete", "warmup-no-orders",
            "selection-anchor-preroll", "selection-hidden-metric-suffix",
            "presenter-earlier-edit", "presenter-time-edit", "magnifier", "security"}) {
        if (!row(std::string("curve-route-") + kind)) continue;
        const std::string k = kind;
        auto bars = mt::stop_feed(true);
        const auto feed_plan = [](const std::vector<Bar>& feed) {
            std::vector<pf_equity_point_t> points;
            for (const auto& bar : feed) points.push_back({bar.timestamp, 1., 0.});
            return mt::plan(mt::source(std::move(points)));
        };
        const auto before_transform = feed_plan(bars);
        if (k == "trim-after" || k == "plan-before-trim")
            bars.erase(bars.begin(), bars.begin() + 3);
        if (k == "tail-appended") {
            auto tail = bars.back(); tail.timestamp += 60000; bars.push_back(tail);
        }
        if (k == "gaps") bars.erase(bars.begin() + 3);
        if (k == "lazy-seal") bars.erase(bars.begin() + 2);
        if (k == "EOF-incomplete") bars.pop_back();
        // Freeze the plan BEFORE either host is pumped. Negative rows keep
        // the intentionally stale pre-trim/pre-tail view; the actual witness
        // below comes independently from the presented metric source.
        const auto admitted_plan = feed_plan(bars);
        const auto frozen_plan = k == "plan-before-trim" || k == "tail-appended"
            || k == "gaps" ? before_transform : admitted_plan;
        auto spec = mt::native_spec();
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        if (k == "lazy-seal" || k == "EOF-complete" || k == "EOF-incomplete") {
            spec.script_tf = "3";
            spec.slot_label_policy = NativeSlotLabelPolicy::Canonical;
        }
        if (k == "magnifier") {
            IntrabarPath::synthesized path;
            path.samples = 4;
            spec.intrabar.value = path;
        }
        if (k == "security") {
            NativeTimeframeSubscription secondary;
            secondary.tf = "3";
            spec.subscriptions.push_back(secondary);
        }
        CurveHost old, next;
        old.edit_earlier = next.edit_earlier = k == "presenter-earlier-edit"
            || k == "selection-hidden-metric-suffix";
        old.hide_suffix = next.hide_suffix = k == "selection-hidden-metric-suffix";
        old.amend_time = next.amend_time = k == "presenter-time-edit";
        STREAM_CHECK(old.configure_native(spec).status == NativeSetupStatus::Applied);
        STREAM_CHECK(next.configure_native(spec).status == NativeSetupStatus::Applied);
        if (k.find("selection-") == 0) {
            pf_selected_window_config_v1 window{
                sizeof(pf_selected_window_config_v1), 1, bars[3].timestamp,
                bars.back().timestamp + 60000};
            STREAM_CHECK(strategy_set_selected_window_v1(&old, &window) == 0);
            STREAM_CHECK(strategy_set_selected_window_v1(&next, &window) == 0);
        }
        // These are feature rows: absence of streaming-ratio is RED, even while the
        // existing run/report itself still has today's exact behavior.
        const bool configured = implemented(next.set_metrics_streaming_v1(
            StreamingSharpeV1::On, kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1));
        old.run(bars.data(), static_cast<int>(bars.size()), "1", spec.script_tf);
        next.run(bars.data(), static_cast<int>(bars.size()), "1", spec.script_tf);
        const auto control = mt::capture(old), got = mt::capture(next);
        STREAM_CHECK(mt::same_run(control, got));
        STREAM_CHECK(control.message.empty());
        if (k == "EOF-complete") STREAM_CHECK(control.curve.size() == 4);
        if (k == "EOF-incomplete") STREAM_CHECK(control.curve.size() == 3);
        if (k == "warmup-no-orders") STREAM_CHECK(control.curve.size() == bars.size());
        if (k == "security") STREAM_CHECK(old.secondary > 0 && old.callbacks == 12);
        if (k.find("selection-") == 0) {
            STREAM_CHECK(!control.curve.empty() && control.curve[0].time_ms == bars[3].timestamp);
            STREAM_CHECK(control.curve[0].equity == spec.initial_capital);
        }
        const auto metric_points = mt::source(old.authoritative);
        active->source_identity = "presenter_authoritative_metric_sequence";
        active->old = mt::legacy(metric_points);
        active->reference_available = true;
        active->actual_extent = mt::extent(metric_points);
        if (k == "selection-hidden-metric-suffix")
            STREAM_CHECK(old.authoritative.size() == control.curve.size() + 1);
        STREAM_CHECK(mt::same(control.metrics.equity.sharpe_bar, active->old.sharpe));
        STREAM_CHECK(mt::same(control.metrics.equity.sortino_bar, active->old.sortino));
        auto cfg = mt::config();
        cfg.metrics_needed |= kMetricsNeedCurveV1;
        candidate(metric_points, frozen_plan, cfg, MetricsRatioRouteV1::LegacyActualCurve);
        if (configured) {
            MetricsExtentReceiptV1 receipt;
            if (implemented(next.metrics_extent_receipt_v1(&receipt))) {
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
                STREAM_CHECK(mt::same_extent(receipt.actual_extent, active->actual_extent));
            }
        }
    }
}

void live_rows() {
    for (bool partial_end : {false, true}) {
        if (!row(partial_end ? "LIVE-partial-end-on" : "LIVE-partial-end-off")) continue;
        auto spec = mt::native_spec(); spec.report_policy = NativeReportPolicy::KernelRecorded;
        CurveHost old, next;
        STREAM_CHECK(old.configure_native(spec).status == NativeSetupStatus::Applied);
        STREAM_CHECK(next.configure_native(spec).status == NativeSetupStatus::Applied);
        implemented(next.set_metrics_streaming_v1(StreamingSharpeV1::On, kMetricsNeedSortinoBarV1));
        const auto bars = mt::stop_feed(true);
        STREAM_CHECK(old.stream_begin(bars.data(), 3, "1", "1"));
        STREAM_CHECK(next.stream_begin(bars.data(), 3, "1", "1"));
        for (std::size_t i = 3; i < 7; ++i) {
            STREAM_CHECK(old.stream_push_bar(bars[i]));
            STREAM_CHECK(next.stream_push_bar(bars[i]));
            pf_report_t a{}, b{};
            STREAM_CHECK(strategy_stream_fill_report(&old, &a) == 0);
            STREAM_CHECK(strategy_stream_fill_report(&next, &b) == 0);
            STREAM_CHECK(a.equity_curve_len == b.equity_curve_len);
            STREAM_CHECK(std::memcmp(&a.metrics, &b.metrics, sizeof a.metrics) == 0);
            BacktestEngine::free_report(reinterpret_cast<ReportC*>(&a));
            BacktestEngine::free_report(reinterpret_cast<ReportC*>(&b));
            MetricsExtentReceiptV1 receipt;
            if (implemented(next.metrics_extent_receipt_v1(&receipt)))
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        }
        STREAM_CHECK(old.stream_end(partial_end)); STREAM_CHECK(next.stream_end(partial_end));
    }
    for (bool partial_end : {false, true}) {
        if (!row(partial_end ? "LIVE-ticks-carried-partial-on" : "LIVE-ticks-carried-partial-off"))
            continue;
        auto spec = mt::native_spec(); spec.report_policy = NativeReportPolicy::KernelRecorded;
        CurveHost old, next;
        STREAM_CHECK(old.configure_native(spec).status == NativeSetupStatus::Applied);
        STREAM_CHECK(next.configure_native(spec).status == NativeSetupStatus::Applied);
        implemented(next.set_metrics_streaming_v1(StreamingSharpeV1::On, kMetricsNeedSortinoBarV1));
        const auto bars = mt::stop_feed(true);
        STREAM_CHECK(old.stream_begin(bars.data(), 3, "1", "1"));
        STREAM_CHECK(next.stream_begin(bars.data(), 3, "1", "1"));
        const TradeTick ticks[] = {
            {mt::epoch + 3 * 60000, 1, 101., 1.},
            {mt::epoch + 5 * 60000 + 15000, 2, 97., 1.},
        };
        for (const auto& tick : ticks) {
            STREAM_CHECK(old.stream_push_tick(tick)); STREAM_CHECK(next.stream_push_tick(tick));
            pf_report_t a{}, b{};
            STREAM_CHECK(strategy_stream_fill_report(&old, &a) == 0);
            STREAM_CHECK(strategy_stream_fill_report(&next, &b) == 0);
            STREAM_CHECK(a.equity_curve_len == b.equity_curve_len);
            STREAM_CHECK(std::memcmp(&a.metrics, &b.metrics, sizeof a.metrics) == 0);
            BacktestEngine::free_report(reinterpret_cast<ReportC*>(&a));
            BacktestEngine::free_report(reinterpret_cast<ReportC*>(&b));
        }
        STREAM_CHECK(old.stream_end(partial_end)); STREAM_CHECK(next.stream_end(partial_end));
        pf_report_t a{}, b{};
        STREAM_CHECK(strategy_stream_fill_report(&old, &a) == 0);
        STREAM_CHECK(strategy_stream_fill_report(&next, &b) == 0);
        // finalize_elapsed_slots records the slots before the latest tick's
        // slot (src/native_execution_consumer.cpp:11452-11485). Partial end
        // invokes only the callback (:11400-11445), without the confirmed
        // calculation's record_script_report_point (:9528), so adds no point.
        const auto confirmed_slots = (ticks[1].timestamp - bars[0].timestamp) / 60000;
        STREAM_CHECK(a.equity_curve_len == confirmed_slots);
        STREAM_CHECK(a.equity_curve_len == b.equity_curve_len);
        STREAM_CHECK(std::memcmp(&a.metrics, &b.metrics, sizeof a.metrics) == 0);
        BacktestEngine::free_report(reinterpret_cast<ReportC*>(&a));
        BacktestEngine::free_report(reinterpret_cast<ReportC*>(&b));
    }
}

class QuoteHost final : public source::PineStrategyHost {
public:
    explicit QuoteHost(bool flat) : flat_(flat) {
        source::PineStrategyConfig c;
        c.initial_capital = 10000.; c.default_qty_value = 2.; c.pyramiding = 2;
        c.commission_value = .1;
        configure_pine_strategy(c);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0 || pine_bar_index() == 1) strategy_entry("open", true);
        if (flat_ && pine_bar_index() == 2) strategy_close("open");
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("streaming-quote/v1"); sink.i(flat_);
    }
    bool flat_;
};
void terminal_quote_rows() {
    for (bool flat : {false, true}) {
        if (!row(flat ? "quote-not-applied-flat" : "quote-applied-open-position")) continue;
        QuoteHost old(flat), next(flat);
        std::vector<Bar> bars;
        for (int i = 0; i <= 60; ++i) {
            const double price = 100. + i * .125;
            bars.push_back({price, price + .25, price - .25, price + .125, 1.,
                            mt::epoch + i * 60000LL});
        }
        STREAM_CHECK(old.set_report_terminal_quote(bars.back().timestamp, 112.));
        STREAM_CHECK(next.set_report_terminal_quote(bars.back().timestamp, 112.));
        const bool configured = implemented(next.set_metrics_streaming_v1(
            StreamingSharpeV1::On, kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1));
        old.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
        next.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
        ReportC report{};
        old.fill_report(&report);
        STREAM_CHECK(old.report_terminal_quote_applied(report) == !flat);
        BacktestEngine::free_report(&report);
        const auto control = mt::capture(old), got = mt::capture(next);
        STREAM_CHECK(control.message.empty());
        STREAM_CHECK(mt::same_run(control, got));
        active->source_identity = "actual_post_quote_presented_metric_curve";
        active->actual_extent = mt::extent(control.points());
        active->old = mt::legacy(control.points());
        active->reference_available = true;
        STREAM_CHECK(mt::same(got.metrics.equity.sharpe_bar, active->old.sharpe));
        STREAM_CHECK(mt::same(got.metrics.equity.sortino_bar, active->old.sortino));
        if (configured) {
            MetricsExtentReceiptV1 receipt;
            if (implemented(next.metrics_extent_receipt_v1(&receipt)))
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        }
    }
}
class RecalculationErrorHost final : public source::PineStrategyHost {
public:
    RecalculationErrorHost() {
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true; c.default_qty_value = 1.;
        configure_pine_strategy(c);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("coof", true);
        if (pine_bar_index() == 1) throw std::runtime_error("streaming-ratio mid-recalculation error");
    }
};
void failure_and_monthly_rows() {
    if (row("COOF-mid-recalculation-error")) {
        RecalculationErrorHost old, next;
        implemented(next.set_metrics_streaming_v1(StreamingSharpeV1::On, kMetricsNeedSortinoBarV1));
        const auto bars = mt::stop_feed(true);
        old.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        next.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        const auto control = mt::capture(old), got = mt::capture(next);
        STREAM_CHECK(old.native_recalculation_count() > 0);
        STREAM_CHECK(control.message.find("streaming-ratio mid-recalculation error") != std::string::npos);
        STREAM_CHECK(mt::same_run(control, got));
        active->original_code = control.code; active->original_args = control.args;
        active->old = mt::legacy(control.points()); active->reference_available = true;
        active->actual_extent = mt::extent(control.points());
    }
    for (const auto& kind : {"preparation", "pending-summary", "monthly-exact"}) {
        if (!row(std::string("existing-") + kind)) continue;
        const std::string k = kind;
        CurveHost old, next;
        auto spec = mt::native_spec(); spec.report_policy = NativeReportPolicy::KernelRecorded;
        STREAM_CHECK(old.configure_native(spec).status == NativeSetupStatus::Applied);
        STREAM_CHECK(next.configure_native(spec).status == NativeSetupStatus::Applied);
        old.prepare_failure = next.prepare_failure = k == "preparation";
        if (k == "pending-summary") {
            STREAM_CHECK(old.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == 0);
            STREAM_CHECK(next.set_report_mode_v1(ReportMode::SUMMARY_ONLY) == 0);
            STREAM_CHECK(old.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0);
            STREAM_CHECK(next.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0);
        }
        auto bars = mt::stop_feed(true);
        if (k == "monthly-exact") {
            for (std::size_t i = 0; i < bars.size(); ++i)
                bars[i].timestamp = mt::epoch + static_cast<std::int64_t>(i) * 32 * mt::day;
            old.edit_earlier = next.edit_earlier = true;
        }
        implemented(next.set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1));
        old.run(bars.data(), static_cast<int>(bars.size()));
        next.run(bars.data(), static_cast<int>(bars.size()));
        const auto control = mt::capture(old), got = mt::capture(next);
        STREAM_CHECK(mt::same_run(control, got));
        active->original_code = control.code; active->original_args = control.args;
        if (k == "preparation") {
            STREAM_CHECK(old.callbacks == 0 && next.callbacks == 0);
            STREAM_CHECK(control.curve.empty() && !control.message.empty());
        }
        if (k == "pending-summary") {
            STREAM_CHECK(!old.equity_summary_ratios_ready() && !next.equity_summary_ratios_ready());
            STREAM_CHECK(std::isnan(got.metrics.equity.sharpe_bar));
            STREAM_CHECK(std::isnan(got.metrics.equity.sortino_bar));
            ReportEnvelopeV1 envelope{};
            envelope.metadata.struct_size = sizeof(envelope.metadata);
            envelope.metadata.version = 1;
            STREAM_CHECK(next.fill_report_envelope_v1(&envelope) == 0);
            STREAM_CHECK(envelope.metadata.complete == 0);
            BacktestEngine::free_report(&envelope.report);
        }
        if (k == "monthly-exact") {
            const auto want = equity_stats_reference::compute_equity_stats(
                control.curve.data(), static_cast<std::int64_t>(control.curve.size()),
                spec.initial_capital, "UTC", bars.front().open, bars.back().close, 0, 0.);
            STREAM_CHECK(mt::same(got.metrics.equity.sharpe_monthly, want.sharpe_monthly));
            STREAM_CHECK(mt::same(got.metrics.equity.sortino_monthly, want.sortino_monthly));
        }
    }
    if (row("existing-unreadable-scratch")) {
        // A valid empty file has an unreadable three-point region. Keep the
        // fd open so a candidate's own allocations cannot recycle its number.
        char path[] = "/tmp/pineforge-streaming-unreadable-XXXXXX";
        const int fd = mkstemp(path);
        STREAM_CHECK(fd >= 0);
        if (fd >= 0) unlink(path);
        metrics::SpillCurve unreadable({fd, 0, 3});
        try {
            (void)metrics::compute_equity_stats(unreadable, 3, 1., "UTC", 1., 1., 0, 0.);
            STREAM_CHECK(false);
        } catch (const std::exception& error) {
            const auto failure = classify_run_failure(error);
            STREAM_CHECK(failure.code == RunFailureCode::report_sink_failed);
            active->original_code = run_failure_code_name(failure.code);
            active->original_args = failure.args ? *failure.args : "{}";
        }
        const auto points = mt::equities({100., 97., 101.});
        std::unique_ptr<StreamingEquityRatiosV1> fold;
        // Outside the expected legacy-I/O catch: a missing streaming-ratio is always RED.
        auto cfg = mt::config();
        cfg.metrics_needed |= kMetricsNeedReturnsV1;
        if (implemented(make_streaming_equity_ratios_v1(cfg, mt::plan(points), {}, fold))) {
            for (std::int64_t i = 0; i < points.count; ++i)
                STREAM_CHECK(fold->append(points[i], 0) == MetricsStreamingStatusV1::Ok);
            const MetricsRatioCurveV1 provider{&unreadable, 3,
                [](void* context, std::int64_t i) {
                    return (*static_cast<metrics::SpillCurve*>(context))[i];
                }};
            try {
                MetricsStreamingResultV1 result;
                const auto status = fold->finish(result, provider);
                STREAM_CHECK(status == MetricsStreamingStatusV1::Ok);
                STREAM_CHECK(false); // Existing unreadable-source failure was lost.
            } catch (const std::exception& error) {
                const auto failure = classify_run_failure(error);
                STREAM_CHECK(failure.code == RunFailureCode::report_sink_failed);
                STREAM_CHECK((failure.args ? *failure.args : "{}") == active->original_args);
            }
        }
        if (fd >= 0) close(fd);
    }
}
void prefix_threshold_rows() {
    for (bool regular : {true, false}) {
        if (!row(regular ? "C-fixed-spacing-prefix-thresholds" : "C-irregular-distinct-thresholds"))
            continue;
        std::set<std::uint64_t> thresholds;
        for (int n = 3; n <= 24; ++n) {
            mt::PointSource points{n, [regular](std::int64_t i) {
                return pf_equity_point_t{
                    mt::epoch + mt::year * (regular ? i : i * (i + 1)),
                    i % 2 ? 97. : 101., 0.};
            }};
            const auto ref = mt::legacy(points);
            thresholds.insert(mt::bits(ref.rf));
            auto cfg = mt::config(); cfg.metrics_needed = kMetricsNeedSortinoBarV1;
            if (candidate(points, mt::plan(points), cfg, MetricsRatioRouteV1::Streaming))
                STREAM_CHECK(mt::same(active->candidate->downside_sum, ref.down));
        }
        STREAM_CHECK(regular ? thresholds.size() == 1 : thresholds.size() == 22);
    }
}

// Real module loading is kept in the allowed test unit, without adding
// unowned CMake targets. Compilation occurs ONLY when the supervisor runs
// this row on the verification host. No repository script is invoked.
void loaded_module_rows() {
    namespace fs = std::filesystem;
    for (bool overridden : {false, true}) {
        const std::string name = overridden
            ? "curve-route-loaded-overridden-presenter-identical"
            : "curve-route-loaded-old-library-identical";
        const std::string admission_name = overridden
            ? "loaded-module-admission-overridden-presenter"
            : "loaded-module-admission-old-library";
        if (list_only) { row(name); row(admission_name); continue; }
        const bool run_control = filter.empty() || name.find(filter) != std::string::npos;
        const bool run_admission = filter.empty() || admission_name.find(filter) != std::string::npos;
        if (!run_control && !run_admission) continue;
        row(run_control ? name : admission_name);
        char buffer[4096];
#ifdef __APPLE__
        std::uint32_t capacity = sizeof buffer;
        const bool located = _NSGetExecutablePath(buffer, &capacity) == 0;
        STREAM_CHECK(located);
        if (!located) continue;
        const std::string linker_kind = "-dynamiclib";
#else
        const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
        STREAM_CHECK(length > 0);
        if (length <= 0) continue;
        buffer[length] = '\0';
        const std::string linker_kind = "-shared";
#endif
        const fs::path build = fs::weakly_canonical(buffer).parent_path().parent_path();
        const fs::path root = fs::path(mt::tests_directory()).parent_path();
        char temporary[] = "/tmp/pineforge-streaming-module-XXXXXX";
        char* directory = mkdtemp(temporary);
        STREAM_CHECK(directory != nullptr);
        if (!directory) continue;
        const fs::path module = fs::path(directory) / "strategy.so";
        const std::string command = "c++ -std=c++17 " + linker_kind + " -fPIC -ffp-contract=off "
            "-fvisibility=hidden -fvisibility-inlines-hidden "
            + std::string(overridden ? "-DSTREAMING_MODULE_CUSTOM=1 " : "-DSTREAMING_MODULE_OLD=1 ")
            + "-I" + mt::shell_quote((root / "include").string()) + " "
            + "-I" + mt::shell_quote((build / "include").string()) + " "
            + mt::shell_quote((root / "tests/test_metrics_streaming_ratios.cpp").string()) + " "
            + mt::shell_quote((build / "lib/libpineforge.a").string())
            + " -pthread -o " + mt::shell_quote(module.string());
        const int compiled = std::system(command.c_str());
        STREAM_CHECK(compiled == 0);
        if (compiled != 0) { fs::remove_all(directory); continue; }
        void* library = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
        STREAM_CHECK(library != nullptr);
        if (!library) { std::fprintf(stderr, "%s\n", dlerror()); fs::remove_all(directory); continue; }
        using Create = NativeStrategyHost* (*)();
        using Destroy = void (*)(NativeStrategyHost*);
        using Declaration = const MetricsPlainDeclarationV1* (*)();
        const auto create = reinterpret_cast<Create>(dlsym(library, "streaming_module_create"));
        const auto destroy = reinterpret_cast<Destroy>(dlsym(library, "streaming_module_destroy"));
        const auto declaration = reinterpret_cast<Declaration>(dlsym(library, "streaming_module_plain"));
        STREAM_CHECK(create && destroy);
        STREAM_CHECK(overridden ? declaration != nullptr : declaration == nullptr);
        if (create && destroy && run_control) {
            auto* old = create(); auto* next = create();
            const auto bars = mt::stop_feed(true);
            old->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            next->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            const auto control = mt::capture(*old), got = mt::capture(*next);
            active->legacy_control = true; // PASS ON BASE BY CONSTRUCTION.
            active->plain = "no";
            active->predicate_inputs = overridden ? "builtin_presenter=0" : "declaration=missing";
            active->source_identity = "loaded_module_actual_presented_metric_curve";
            active->old = mt::legacy(control.points());
            active->reference_available = true;
            active->actual_extent = mt::extent(control.points());
            STREAM_CHECK(mt::same_run(control, got));
            STREAM_CHECK(control.message.empty() && control.curve.size() == bars.size());
            if (overridden)
                STREAM_CHECK(control.curve[1].equity != control.curve[0].equity);
            destroy(old); destroy(next);
        }
        if (create && destroy && run_admission) {
            if (run_control) row(admission_name);
            auto* old = create(); auto* next = create();
            const auto bars = mt::stop_feed(true);
            std::vector<pf_equity_point_t> input_points;
            for (const auto& bar : bars) input_points.push_back({bar.timestamp, 1., 0.});
            const auto planned = mt::plan(mt::source(std::move(input_points)));
            auto inputs = plain_inputs(declaration ? declaration() : nullptr);
            // Loading the edited module invalidates the implementation binding
            // in addition to its explicit builtin_presenter=0 declaration.
            if (overridden) inputs.module_contract_bound = 0;
            active->plain = "no";
            active->predicate_inputs = overridden
                ? "module_contract_bound=0,builtin_presenter=0" : "declaration=missing";
            active->planned = planned;
            const bool configured = implemented(next->set_metrics_streaming_v1(
                StreamingSharpeV1::On, kMetricsNeedSortinoBarV1));
            MetricsExtentReceiptV1 receipt;
            const bool admitted = implemented(next->admit_metrics_plain_v1(inputs, planned, &receipt));
            if (admitted) {
                STREAM_CHECK(receipt.plain_subset == 0 && receipt.failed_plain_inputs != 0);
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
            }
            old->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            next->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            const auto control = mt::capture(*old), got = mt::capture(*next);
            STREAM_CHECK(mt::same_run(control, got));
            active->old = mt::legacy(control.points()); active->reference_available = true;
            active->actual_extent = mt::extent(control.points());
            active->source_identity = "loaded_module_actual_presented_metric_curve";
            if (configured && admitted
                && implemented(next->metrics_extent_receipt_v1(&receipt)))
                STREAM_CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
            destroy(old); destroy(next);
        }
        dlclose(library);
        fs::remove_all(directory);
    }
}

void run_suite() {
    domains();
    exact_rows();
    wrong_plans();
    switches_and_needs();
    determinism_and_tie_rows();
    stop_rows();
    overflow_rows();
    plain_rows();
    preflight_rows();
    held_synthetic_rows();
    amended_pine_rows();
    curve_route_rows();
    live_rows();
    terminal_quote_rows();
    failure_and_monthly_rows();
    prefix_threshold_rows();
    loaded_module_rows();
}
} // namespace streaming_suite

#ifndef METRICS_STREAMING_COMPARE_TOOL
int main(int argc, char** argv) {
    using namespace streaming_suite;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") list_only = true;
        else if (arg == "--row" && i + 1 < argc) filter = argv[++i];
        else if (arg == "--mutant" && i + 1 < argc) mutant = argv[++i];
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); return 2; }
    }
    run_suite();
    if (!list_only && artifacts.empty()) {
        std::fprintf(stderr, "no matching streaming-ratio rows\n"); return 2;
    }
    std::printf("streaming-ratio streaming ratios: %zu rows, %d checks, %d failures\n",
                artifacts.size(), checks, failures);
    return failures ? 1 : 0;
}
#endif
#endif // Real-module fixture versus executable.
