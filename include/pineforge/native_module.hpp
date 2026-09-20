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
/// The macro adds no new C symbol: every name it defines is already declared
/// in `<pineforge/pineforge.h>`. The remaining runtime exports
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

#include <cstddef>
#include <cstring>
#include <exception>
#include <type_traits>
#include <vector>

namespace pineforge {
namespace native_module {

// The C mirrors are copied field by field (bars) and memcpy'd (report), so
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
/// Deriving is deliberate — `last_error_` is protected engine state, and only
/// a derived class may write it.
template <typename Host>
class Module final : public Host {
    static_assert(std::is_base_of<NativeStrategyHost, Host>::value,
                  "PINEFORGE_EXPORT_NATIVE_STRATEGY requires a NativeStrategyHost subclass");

public:
    /// Presentation text for `strategy_get_last_error`. Never throws: it runs
    /// on the C boundary, often from a catch handler.
    void note_error(const char* text) noexcept {
        try {
            this->last_error_ = text ? text : "";
        } catch (...) {
        }
    }
};

inline BacktestEngine* as_engine(pf_strategy_t strategy) {
    return static_cast<BacktestEngine*>(strategy);
}

template <typename Host>
Module<Host>* as_module(pf_strategy_t strategy) {
    return strategy ? dynamic_cast<Module<Host>*>(as_engine(strategy)) : nullptr;
}

template <typename Host>
void note_error(pf_strategy_t strategy, const char* text) {
    if (auto* module = as_module<Host>(strategy)) module->note_error(text);
}

/// Runs `fn` and converts any escaping exception into presentation text. C
/// callers have no exception channel, so nothing may propagate past here.
template <typename Host, typename Fn>
void guarded(pf_strategy_t strategy, Fn&& fn) {
    if (!strategy) return;
    try {
        fn();
    } catch (const std::exception& error) {
        note_error<Host>(strategy, error.what());
    } catch (...) {
        note_error<Host>(strategy, "native host refuses source mutation");
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
        std::vector<Bar> copied;
        const Bar* source = nullptr;
        if (count > 0 && bars != nullptr) {
            copied.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                copied[static_cast<std::size_t>(i)] = copy_bar(bars[i]);
            }
            source = copied.data();
        }
        if (with_timeframes) {
            engine->run(source, count, input_tf ? input_tf : "",
                        script_tf ? script_tf : "");
        } else {
            engine->run(source, count);
        }
        publish_report(engine, out);
    } catch (const std::exception& error) {
        note_error<Host>(strategy, error.what());
    } catch (...) {
        note_error<Host>(strategy, "native module C batch failed");
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
        note_error<Host>(strategy, "native module refuses unsupported magnifier arguments");
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
