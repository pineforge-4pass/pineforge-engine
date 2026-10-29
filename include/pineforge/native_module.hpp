#pragma once

/// @file native_module.hpp
/// One-line C ABI export for a hand-written `NativeStrategyHost`.
///
/// A loadable PineForge strategy is a shared object that exports a small,
/// fixed set of `extern "C"` entry points (`strategy_create`, `run_backtest`,
/// …) declared in `<pineforge/pineforge.h>`. Codegen emits them for a
/// PineScript strategy; a native C++ host had to hand-write them. This header
/// generates exactly that surface from one macro:
///
/// @code
/// #include <pineforge/native_module.hpp>
///
/// class MyHost : public pineforge::NativeStrategyHost {
///     void on_native_bar(const pineforge::Bar&,
///                        const pineforge::NativeDecisionContext&) override { … }
/// };
///
/// PINEFORGE_EXPORT_NATIVE_STRATEGY(MyHost);
/// @endcode
///
/// The ordinary entry points are declared in `<pineforge/pineforge.h>`.
/// The optional metrics-only capability below belongs to each module, not
/// the static runtime. The remaining runtime exports
/// (`strategy_configure_native_v1`, the `strategy_stream_*` family,
/// `strategy_get_last_error`, …) come from the engine's own `c_abi.cpp`; the
/// generated `strategy_create` references `pf_abi_version()` so the linker
/// keeps that object when the module links `libpineforge.a` statically.
///
/// The host class must be a `NativeStrategyHost` subclass and must not be
/// `final`: the module wraps it in one derived class, which is what makes the
/// engine's protected presentation-error string (read back through
/// `strategy_get_last_error`) writable from the C boundary.

#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

/// Optional per-module probe. Call it, rather than testing symbol presence:
/// Ok (0) certifies this module's metrics-only adapter; NotImplemented (-1)
/// keeps the actual curve. Callers must resolve LegacyRerun before publishing
/// any report, using a fresh strategy with the same settings and frozen bars.
extern "C" PF_API std::int32_t strategy_metrics_only_capability_v1(void) noexcept;

namespace pineforge {
namespace native_module {

/// Specialize for the EXACT host passed to PINEFORGE_EXPORT_NATIVE_STRATEGY.
/// Return its versioned declaration, or nullopt when it cannot certify itself.
/// No specialization means no certification; a subclass does not inherit one.
///
/// In addition to the declaration's presenter/marks/scheduler facts, opting in
/// certifies one final metric point per input bar, with no extra anchor, on the
/// admitted passthrough schedule, and deterministic whole-run replay on a fresh
/// instance with identical settings and frozen input. Source hosts with private
/// tail/probe modes need their own adapter: this wrapper certifies only the
/// native defaults of those setters. A modified presenter or mark implementation
/// needs a new, truthful specialization, never a caller-provided declaration.
template <typename Host>
struct MetricsPlainHostV1 {
    static std::optional<MetricsPlainDeclarationV1> declaration() {
        return std::nullopt;
    }
};

template <typename Host>
bool metrics_plain_declaration_v1(MetricsPlainDeclarationV1& out) noexcept {
    try {
        const auto declaration = MetricsPlainHostV1<Host>::declaration();
        if (!declaration) return false;
        const auto& d = *declaration;
        if (d.struct_size != sizeof(d) || d.version != 1
            || d.builtin_presenter != 1 || d.builtin_marks != 1
            || d.builtin_scheduler != 1 || d.security_sites != 0
            || d.lower_tf_sites != 0 || d.magnifier_declared != 0
            || d.calculation_trigger > 1 || d.calc_on_every_history_tick != 0)
            return false;
        out = d;
        return true;
    } catch (...) {
        return false;
    }
}

template <typename Host>
std::int32_t metrics_only_capability_v1() noexcept {
    MetricsPlainDeclarationV1 declaration;
    return static_cast<std::int32_t>(metrics_plain_declaration_v1<Host>(declaration)
        ? metrics::MetricsStreamingStatusV1::Ok
        : metrics::MetricsStreamingStatusV1::NotImplemented);
}

// Report-only invocation identities. They never enter broker decisions or
// continuation hashes. Exhaustion disables the optimization instead of wrapping
// an attempt identity. Fresh handles used for recovery receive fresh attempts.
inline std::uint64_t next_metrics_attempt_v1() noexcept {
    static std::atomic<std::uint64_t> last{0};
    auto previous = last.load(std::memory_order_relaxed);
    while (previous != std::numeric_limits<std::uint64_t>::max()) {
        if (last.compare_exchange_weak(previous, previous + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return previous + 1;
    }
    return 0;
}

// Hash scalar words explicitly: neither Bar padding nor host byte order is an
// input. NaN payloads and signed zero remain distinct input generations.
class MetricsBindingHashV1 {
    std::uint64_t value_ = 14695981039346656037ULL;
public:
    void word(std::uint64_t value) noexcept {
        for (unsigned i = 0; i < 8; ++i) {
            value_ ^= value & 0xffu;
            value_ *= 1099511628211ULL;
            value >>= 8;
        }
    }
    void number(double value) noexcept {
        std::uint64_t bits;
        static_assert(sizeof(bits) == sizeof(value), "binary64 input binding");
        std::memcpy(&bits, &value, sizeof(bits));
        word(bits);
    }
    std::uint64_t value() const noexcept { return value_; }
};

// Bars are borrowed through the C mirror and reports are memcpy'd, so
// their layouts must agree exactly.
static_assert(sizeof(pf_bar_t) == sizeof(Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");

static_assert(sizeof(pf_report_t) == sizeof(ReportC),
              "pf_report_t / pineforge::ReportC size mismatch");
static_assert(offsetof(pf_report_t, total_trades) == offsetof(ReportC, total_trades),
              "pf_report_t::total_trades offset mismatch");
static_assert(offsetof(pf_report_t, trades) == offsetof(ReportC, trades),
              "pf_report_t::trades offset mismatch");
static_assert(offsetof(pf_report_t, net_profit) == offsetof(ReportC, net_profit),
              "pf_report_t::net_profit offset mismatch");
static_assert(offsetof(pf_report_t, security_diag) == offsetof(ReportC, security_diag),
              "pf_report_t::security_diag offset mismatch");
static_assert(offsetof(pf_report_t, metrics) == offsetof(ReportC, metrics),
              "pf_report_t::metrics offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve) == offsetof(ReportC, equity_curve),
              "pf_report_t::equity_curve offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve_len) == offsetof(ReportC, equity_curve_len),
              "pf_report_t::equity_curve_len offset mismatch");
static_assert(offsetof(pf_report_t, broker_state_hash) == offsetof(ReportC, broker_state_hash),
              "pf_report_t::broker_state_hash offset mismatch");

/// The one class the macro instantiates: the host plus the module boundary.
/// Deriving is deliberate — `last_error_` is protected engine state, written
/// here only through note_run_failure (run_failure.hpp), with its code.
template <typename Host>
class Module final : public Host {
    static_assert(std::is_base_of<NativeStrategyHost, Host>::value,
                  "PINEFORGE_EXPORT_NATIVE_STRATEGY requires a NativeStrategyHost subclass");

public:
    /// This is a speculative report optimization only. Failure to establish
    /// any proof leaves the existing run and its refusal ordering in charge.
    void prepare_metrics_only_v1(const Bar* bars, int count,
                                 const char* input_tf, const char* script_tf,
                                 bool with_timeframes) noexcept {
        using namespace metrics;
        if (this->metrics_streaming_.configuration.streaming_sharpe
                != StreamingSharpeV1::On)
            return;
        // Existing/default requests retain their sequence and do not acquire
        // an admission or recovery proof just by using this wrapper.
        const auto needed = this->metrics_streaming_.configuration.metrics_needed;
        if (needed & (kMetricsNeedReturnsV1 | kMetricsNeedCurveV1)) return;
        MetricsPlainDeclarationV1 declaration;
        if (!metrics_plain_declaration_v1<Host>(declaration)) return;
        try {
            const auto state = this->native_state();
            if (state.kind != NativeLifecycleKind::Ready || !state.spec
                || count < 0 || (count > 0 && !bars))
                return;
            const auto& spec = *state.spec;
            // Empty timeframe arguments are forwarded unchanged by run_batch.
            // They cannot certify a different staged view before native begin
            // has resolved/refused them.
            const bool bound_timeframes = !with_timeframes
                || (input_tf && script_tf && spec.input_tf == input_tf
                    && spec.script_tf == script_tf);
            MetricsExtentPlanV1 plan;
            plan.present = 1;
            plan.extent.count = count;
            plan.extent.empty = count == 0;
            if (count > 0) {
                plan.extent.first_time_ms = bars[0].timestamp;
                plan.extent.last_time_ms = bars[count - 1].timestamp;
            }
            plan.attempt = next_metrics_attempt_v1();
            if (plan.attempt == 0) return;
            // The caller freezes these borrowed bars through synchronous run
            // and recovery. Bind their actual words, not their storage address.
            MetricsBindingHashV1 generation;
            generation.word(static_cast<std::uint64_t>(count));
            for (int i = 0; i < count; ++i) {
                const auto& bar = bars[i];
                generation.word(static_cast<std::uint64_t>(bar.timestamp));
                generation.number(bar.open);
                generation.number(bar.high);
                generation.number(bar.low);
                generation.number(bar.close);
                generation.number(bar.volume);
            }
            plan.input_generation = generation.value();
            plan.spec_identity = native_run_spec_digest(spec);
            // Bind only the planned input extent. Final equity is a receipt
            // observation produced after execution and never part of this hash.
            MetricsBindingHashV1 view;
            view.word(plan.input_generation);
            view.word(plan.spec_identity);
            view.word(static_cast<std::uint64_t>(plan.extent.count));
            view.word(static_cast<std::uint64_t>(plan.extent.first_time_ms));
            view.word(static_cast<std::uint64_t>(plan.extent.last_time_ms));
            view.word(plan.extent.empty);
            plan.admitted_view = view.value();

            MetricsPlainInputsV1 inputs;
            inputs.declaration = &declaration;
            inputs.module_contract_bound = declaration.calculation_trigger
                == static_cast<std::uint32_t>(spec.calculation);
            inputs.historical_batch = 1;
            inputs.equal_resolved_timeframes = bound_timeframes
                && !spec.input_tf.empty() && spec.input_tf == spec.script_tf;
            inputs.timeframe_undetected = spec.timeframe_undetected;
            inputs.intrabar_none = spec.intrabar.is_none();
            inputs.builtin_host_marks =
                spec.report_policy == NativeReportPolicy::KernelRecorded
                || spec.report_policy == NativeReportPolicy::KernelRecordedAtHostMarks;
            inputs.feed_tolerant_passthrough =
                spec.slot_label_policy == NativeSlotLabelPolicy::FeedTolerant
                && spec.subscriptions.empty() && !spec.auxiliary_feed
                && spec.instrument_feeds.empty();
            // An override may keep private state this wrapper cannot inspect.
            // Unknown is PLAIN=no, even if a module supplied a declaration.
            const bool native_tails = this->template native_tail_defaults<Host>(0);
            inputs.realtime_tail = native_tails ? 0u : 1u;
            inputs.probe_suppress_tail_logic = native_tails ? 0u : 1u;
            inputs.calculation_trigger = static_cast<std::uint32_t>(spec.calculation);
            inputs.final_admitted_view_bound = bound_timeframes;
            // Selection is owned by native begin, which rechecks pending AND
            // admitted selection after all legacy refusals, before consuming
            // this candidate proof. No selected run can use the input extent.
            MetricsExtentReceiptV1 receipt;
            if (this->admit_metrics_plain_v1(inputs, plan, &receipt)
                    != MetricsStreamingStatusV1::Ok
                || receipt.plain_subset != 1 || receipt.failed_plain_inputs != 0)
                return;
            MetricsStreamingConfigV1 config;
            config.streaming_sharpe = StreamingSharpeV1::On;
            config.metrics_needed = needed;
            config.attempt = plan.attempt;
            config.input_generation = plan.input_generation;
            config.admitted_view = plan.admitted_view;
            config.spec_identity = plan.spec_identity;
            config.deterministic_replay = 1; // the exact-host certification above
            config.curve_available = 0;
            if (this->prepare_metrics_streaming_v1(config, plan)
                    == MetricsStreamingStatusV1::Ok)
                return;
        } catch (...) {
            // Admission is optional, including allocation in a module's hook.
            // The ordinary run still owns every execution failure.
        }
        this->metrics_streaming_.pending_plain = false;
        this->metrics_streaming_.prepared = false;
        this->metrics_streaming_.pending_plan = {};
        this->metrics_streaming_.pending_failed_plain_inputs = 0;
    }

    /// Presentation text for `strategy_get_last_error`, written with its run
    /// failure code (`strategy_get_last_error_code`) so the two never
    /// disagree. Never throws: it runs on the C boundary, often from a catch
    /// handler.
    void note_error(const char* text, const RunFailureValue& value) noexcept {
        try {
            note_run_failure(*this, std::string(text ? text : ""), value);
        } catch (...) {
        }
    }
    /// The text alone, as before run-failure codes: engine_unclassified_error.
    void note_error(const char* text) noexcept {
        note_error(text, RunFailureValue{RunFailureCode::engine_unclassified_error, nullptr});
    }

private:
    // An overload or inaccessible setter is unknown, rather than a new compile
    // requirement on arbitrary uncertified hosts using the existing macro.
    template <typename H>
    static auto native_tail_defaults(int) noexcept
        -> decltype(&H::set_realtime_tail, &H::set_probe_suppress_tail_logic, bool{}) {
        return std::is_same<decltype(&H::set_realtime_tail),
                            decltype(&BacktestEngine::set_realtime_tail)>::value
            && std::is_same<decltype(&H::set_probe_suppress_tail_logic),
                            decltype(&BacktestEngine::set_probe_suppress_tail_logic)>::value;
    }
    template <typename H>
    static bool native_tail_defaults(...) noexcept { return false; }
};

inline BacktestEngine* as_engine(pf_strategy_t strategy) {
    return static_cast<BacktestEngine*>(strategy);
}

template <typename Host>
Module<Host>* as_module(pf_strategy_t strategy) {
    return strategy ? dynamic_cast<Module<Host>*>(as_engine(strategy)) : nullptr;
}

template <typename Host>
void note_error(pf_strategy_t strategy, const char* text, const RunFailureValue& value) {
    if (auto* module = as_module<Host>(strategy)) module->note_error(text, value);
}

/// The code of an exception no coded site raised: engine_unclassified_error.
inline RunFailureValue unclassified_failure() noexcept {
    return RunFailureValue{RunFailureCode::engine_unclassified_error, nullptr};
}

/// The text alone, as before run-failure codes: engine_unclassified_error.
template <typename Host>
void note_error(pf_strategy_t strategy, const char* text) {
    note_error<Host>(strategy, text, unclassified_failure());
}

/// Runs `fn` and converts any escaping exception into presentation text and
/// its code. C callers have no exception channel, so nothing may propagate
/// past here.
template <typename Host, typename Fn>
void guarded(pf_strategy_t strategy, Fn&& fn) {
    if (!strategy) return;
    try {
        fn();
    } catch (const std::exception& error) {
        note_error<Host>(strategy, error.what(), classify_run_failure(error));
    } catch (...) {
        note_error<Host>(strategy, "native host refuses source mutation",
                         unclassified_failure());
    }
}

inline Bar copy_bar(const pf_bar_t& in) {
    Bar out;
    out.open = in.open;
    out.high = in.high;
    out.low = in.low;
    out.close = in.close;
    out.volume = in.volume;
    out.timestamp = in.timestamp;
    return out;
}

/// A native host owns its own intrabar path through `NativeRunSpec::intrabar`,
/// so the C magnifier arguments have no meaning here. Only the defaults are
/// accepted; anything else is refused before the run starts.
inline bool magnifier_unsupported(int bar_magnifier, int magnifier_samples,
                                  pf_magnifier_distribution_t distribution) {
    return bar_magnifier != 0 || magnifier_samples != 4
        || distribution != PF_MAGNIFIER_ENDPOINTS;
}

/// Fills a local report first: the caller's struct is written only once the
/// engine has produced a complete, owned value.
inline void publish_report(BacktestEngine* engine, pf_report_t* out) {
    if (!engine || !out) return;
    ReportC local{};
    try {
        engine->fill_report(&local);
        std::memcpy(out, &local, sizeof(local));
    } catch (...) {
        BacktestEngine::free_report(&local);
        throw;
    }
}

template <typename Host>
pf_strategy_t create() {
    try {
        // Also the link-time anchor: referencing one PF_API symbol pulls the
        // engine's C ABI object (and with it every other runtime export) out
        // of libpineforge.a.
        (void)pf_abi_version();
        BacktestEngine* engine = new Module<Host>();
        return static_cast<pf_strategy_t>(engine);
    } catch (...) {
        return nullptr;
    }
}

inline void destroy(pf_strategy_t strategy) {
    delete as_engine(strategy);
}

template <typename Host>
void set_input(pf_strategy_t strategy, const char* key, const char* value) {
    guarded<Host>(strategy, [&] {
        as_engine(strategy)->set_input(key ? key : "", value ? value : "");
    });
}

template <typename Host>
void set_override(pf_strategy_t strategy, const char* key, const char* value) {
    guarded<Host>(strategy, [&] {
        // No BacktestEngine override mutator exists. Forward to the public
        // guarded source-configuration entry; a host that refuses source
        // mutation latches before inputs_ is written.
        as_engine(strategy)->set_input(key ? key : "strategy_set_override",
                                       value ? value : "");
    });
}

template <typename Host>
void set_magnifier_volume_weighted(pf_strategy_t strategy, int on) {
    guarded<Host>(strategy, [&] {
        as_engine(strategy)->set_magnifier_volume_weighted(on != 0);
    });
}

template <typename Host>
void run_batch(pf_strategy_t strategy, pf_bar_t* bars, int count,
               const char* input_tf, const char* script_tf, bool with_timeframes,
               pf_report_t* out) {
    if (!strategy) return;
    auto* engine = as_engine(strategy);
    try {
        const Bar* source = reinterpret_cast<const Bar*>(bars);
        // The size and every field offset are pinned above, matching the
        // generated strategy wrapper's borrowed Bar array. The engine reads
        // these bars only during run(); making a temporary vector here would
        // charge another complete chart array to every native-module call.
        // Keep malformed pointer/count pairs intact so the engine's common
        // begin preflight still owns their diagnostic and lifecycle result.
        // The caller keeps this array alive through the synchronous run.
        // Reports own their arrays separately, as publish_report requires.
        if (auto* module = as_module<Host>(strategy))
            module->prepare_metrics_only_v1(source, count, input_tf, script_tf,
                                            with_timeframes);
        if (with_timeframes) {
            engine->run(source, count, input_tf ? input_tf : "",
                        script_tf ? script_tf : "");
        } else {
            engine->run(source, count);
        }
        publish_report(engine, out);
    } catch (const std::exception& error) {
        note_error<Host>(strategy, error.what(), classify_run_failure(error));
    } catch (...) {
        note_error<Host>(strategy, "native module C batch failed", unclassified_failure());
    }
}

template <typename Host>
void run_backtest(pf_strategy_t strategy, pf_bar_t* bars, int count, pf_report_t* out) {
    run_batch<Host>(strategy, bars, count, nullptr, nullptr, false, out);
}

template <typename Host>
void run_backtest_full(pf_strategy_t strategy, pf_bar_t* bars, int count,
                       const char* input_tf, const char* script_tf,
                       int bar_magnifier, int magnifier_samples,
                       pf_magnifier_distribution_t magnifier_distribution,
                       pf_report_t* out) {
    if (!strategy) return;
    if (magnifier_unsupported(bar_magnifier, magnifier_samples, magnifier_distribution)) {
        // Wrapper preflight only: the output report is left untouched and the
        // host stays Ready, so the caller may retry with supported arguments.
        try {
            note_error<Host>(strategy, "native module refuses unsupported magnifier arguments",
                             make_run_failure(RunFailureCode::run_options_rejected,
                                              {{"option", "magnifier"}}));
        } catch (...) {
        }
        return;
    }
    run_batch<Host>(strategy, bars, count, input_tf, script_tf, true, out);
}

inline void report_free(pf_report_t* report) {
    if (!report) return;
    ReportC local{};
    std::memcpy(&local, report, sizeof(local));
    BacktestEngine::free_report(&local);
    std::memcpy(report, &local, sizeof(local));
}

}  // namespace native_module
}  // namespace pineforge

/// Define the loadable-module C ABI for one `NativeStrategyHost` subclass.
/// Use it once, at namespace scope, in the module's translation unit.
#define PINEFORGE_EXPORT_NATIVE_STRATEGY(Class)                                     \
    extern "C" {                                                                    \
    PF_API std::int32_t strategy_metrics_only_capability_v1(void) noexcept {        \
        return ::pineforge::native_module::metrics_only_capability_v1<Class>();     \
    }                                                                               \
    PF_API pf_strategy_t strategy_create(const char*) {                             \
        return ::pineforge::native_module::create<Class>();                         \
    }                                                                               \
    PF_API void strategy_free(pf_strategy_t s) {                                    \
        ::pineforge::native_module::destroy(s);                                     \
    }                                                                               \
    PF_API void strategy_set_input(pf_strategy_t s, const char* key,                \
                                   const char* value) {                             \
        ::pineforge::native_module::set_input<Class>(s, key, value);                \
    }                                                                               \
    PF_API void strategy_set_override(pf_strategy_t s, const char* key,             \
                                      const char* value) {                          \
        ::pineforge::native_module::set_override<Class>(s, key, value);             \
    }                                                                               \
    PF_API void strategy_set_magnifier_volume_weighted(pf_strategy_t s, int on) {   \
        ::pineforge::native_module::set_magnifier_volume_weighted<Class>(s, on);    \
    }                                                                               \
    PF_API void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n,                \
                             pf_report_t* out) {                                    \
        ::pineforge::native_module::run_backtest<Class>(s, bars, n, out);           \
    }                                                                               \
    PF_API void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n,           \
                                  const char* input_tf, const char* script_tf,      \
                                  int bar_magnifier, int magnifier_samples,         \
                                  pf_magnifier_distribution_t magnifier_dist,       \
                                  pf_report_t* out) {                               \
        ::pineforge::native_module::run_backtest_full<Class>(                       \
            s, bars, n, input_tf, script_tf, bar_magnifier, magnifier_samples,      \
            magnifier_dist, out);                                                   \
    }                                                                               \
    PF_API void report_free(pf_report_t* report) {                                  \
        ::pineforge::native_module::report_free(report);                            \
    }                                                                               \
    }                                                                               \
    static_assert(true, "PINEFORGE_EXPORT_NATIVE_STRATEGY expects a trailing ';'")
