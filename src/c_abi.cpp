/*
 * c_abi.cpp — runtime-side implementation of the public C ABI declared
 * in <pineforge/pineforge.h>.
 *
 * Contains:
 *   - Layout-compatibility static_asserts between the public C PODs
 *     and the internal C++ types they mirror. If any of these trip,
 *     the C ABI has drifted from the internal representation — fix
 *     BEFORE shipping a .so that consumers depend on.
 *   - The runtime-library-side `extern "C"` symbols (the setters,
 *     strategy_get_last_error, the strategy_stream_* lifecycle,
 *     pf_version_get/pf_version_string, pf_abi_version — the authoritative list is EXPECTED_RUNTIME in
 *     scripts/check_c_abi_runtime.py, enforced by CI). The other
 *     `extern "C"` symbols listed in pineforge.h (strategy_create,
 *     run_backtest, etc.) are emitted per-compiled-strategy by the
 *     codegen, not here.
 */

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible so definitions here are prototype-checked.
// engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS, which suppresses them.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/magnifier.hpp>
#include <cstddef>

/* ── Bar layout parity ──────────────────────────────────────────── */

static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(pineforge::Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(pineforge::Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(pineforge::Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(pineforge::Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(pineforge::Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(pineforge::Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");

/* ── Trade layout parity ────────────────────────────────────────── */

static_assert(sizeof(pf_trade_t) == sizeof(pineforge::TradeC),
              "pf_trade_t / pineforge::TradeC size mismatch");
static_assert(offsetof(pf_trade_t, entry_time) == offsetof(pineforge::TradeC, entry_time),
              "pf_trade_t::entry_time offset mismatch");
static_assert(offsetof(pf_trade_t, exit_time) == offsetof(pineforge::TradeC, exit_time),
              "pf_trade_t::exit_time offset mismatch");
static_assert(offsetof(pf_trade_t, entry_price) == offsetof(pineforge::TradeC, entry_price),
              "pf_trade_t::entry_price offset mismatch");
static_assert(offsetof(pf_trade_t, qty) == offsetof(pineforge::TradeC, qty),
              "pf_trade_t::qty offset mismatch");
static_assert(offsetof(pf_trade_t, commission) == offsetof(pineforge::TradeC, commission),
              "pf_trade_t::commission offset mismatch");
static_assert(offsetof(pf_trade_t, entry_bar_index) == offsetof(pineforge::TradeC, entry_bar_index),
              "pf_trade_t::entry_bar_index offset mismatch");
static_assert(offsetof(pf_trade_t, exit_bar_index) == offsetof(pineforge::TradeC, exit_bar_index),
              "pf_trade_t::exit_bar_index offset mismatch");

/* ── SecurityDiag layout parity ─────────────────────────────────── */
/* The middle two fields differ in name (complete_count / partial_count
 * vs the internal C++ names) but layout is identical. Size + first +
 * last field offsets cover the whole struct. */

static_assert(sizeof(pf_security_diag_t) == sizeof(pineforge::SecurityDiagC),
              "pf_security_diag_t / pineforge::SecurityDiagC size mismatch");
static_assert(offsetof(pf_security_diag_t, sec_id) == offsetof(pineforge::SecurityDiagC, sec_id),
              "pf_security_diag_t::sec_id offset mismatch");
static_assert(offsetof(pf_security_diag_t, feed_count) == offsetof(pineforge::SecurityDiagC, feed_count),
              "pf_security_diag_t::feed_count offset mismatch");

/* ── TraceEntry layout parity ───────────────────────────────────── */

static_assert(sizeof(pf_trace_entry_t) == sizeof(pineforge::TraceEntryC),
              "pf_trace_entry_t / pineforge::TraceEntryC size mismatch");
static_assert(offsetof(pf_trace_entry_t, timestamp) == offsetof(pineforge::TraceEntryC, timestamp),
              "pf_trace_entry_t::timestamp offset mismatch");
static_assert(offsetof(pf_trace_entry_t, value) == offsetof(pineforge::TraceEntryC, value),
              "pf_trace_entry_t::value offset mismatch");

/* ── Report layout parity ───────────────────────────────────────── */

static_assert(sizeof(pf_report_t) == sizeof(pineforge::ReportC),
              "pf_report_t / pineforge::ReportC size mismatch");
static_assert(offsetof(pf_report_t, total_trades) == offsetof(pineforge::ReportC, total_trades),
              "pf_report_t::total_trades offset mismatch");
static_assert(offsetof(pf_report_t, trades) == offsetof(pineforge::ReportC, trades),
              "pf_report_t::trades offset mismatch");
static_assert(offsetof(pf_report_t, net_profit) == offsetof(pineforge::ReportC, net_profit),
              "pf_report_t::net_profit offset mismatch");
static_assert(offsetof(pf_report_t, security_diag) == offsetof(pineforge::ReportC, security_diag),
              "pf_report_t::security_diag offset mismatch");
static_assert(offsetof(pf_report_t, trace_names_len) == offsetof(pineforge::ReportC, trace_names_len),
              "pf_report_t::trace_names_len tail offset mismatch");
static_assert(offsetof(pf_report_t, metrics) == offsetof(pineforge::ReportC, metrics),
              "pf_report_t::metrics offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve) == offsetof(pineforge::ReportC, equity_curve),
              "pf_report_t::equity_curve offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve_len) == offsetof(pineforge::ReportC, equity_curve_len),
              "pf_report_t::equity_curve_len offset mismatch");

/* ── Magnifier distribution enum parity ─────────────────────────── */

static_assert(static_cast<int>(pineforge::MagnifierDistribution::UNIFORM)      == PF_MAGNIFIER_UNIFORM,
              "MagnifierDistribution::UNIFORM enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::COSINE)       == PF_MAGNIFIER_COSINE,
              "MagnifierDistribution::COSINE enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::TRIANGLE)     == PF_MAGNIFIER_TRIANGLE,
              "MagnifierDistribution::TRIANGLE enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::ENDPOINTS)    == PF_MAGNIFIER_ENDPOINTS,
              "MagnifierDistribution::ENDPOINTS enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::FRONT_LOADED) == PF_MAGNIFIER_FRONT_LOADED,
              "MagnifierDistribution::FRONT_LOADED enum value mismatch");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::BACK_LOADED)  == PF_MAGNIFIER_BACK_LOADED,
              "MagnifierDistribution::BACK_LOADED enum value mismatch");

/* ───────────────────────────────────────────────────────────────────
 * Runtime-library-side extern "C" implementations.
 *
 * These live in libruntime.a (statically linked into every compiled
 * strategy .so), so consumers find them via dlsym on any .so. The
 * other extern "C" symbols listed in pineforge.h are emitted PER
 * STRATEGY by the codegen (in emit_top.py::_emit_extern_c).
 * ─────────────────────────────────────────────────────────────────── */

extern "C" {

/* Toggle per-bar trace recording on a live strategy. Default off; the
 * harness flips it on per-strategy via this entry point before running
 * a backtest whose per-bar values it wants to cross-reference against
 * TradingView. */
PF_API void strategy_set_trace_enabled(pf_strategy_t s, int on) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_trace_enabled(on != 0);
}

/* Returns the error message captured by the most recent run() on this
 * strategy, or an empty string if the run completed normally. The
 * pointer remains valid until the next run() (which clears the
 * captured error before it begins). Returns NULL only when ``s`` is
 * NULL. The runtime catches all std::exception derivatives inside
 * BacktestEngine::run() so the C ABI never unwinds an exception across
 * the extern "C" boundary; consumers must check this after every run
 * to surface engine-rejected configurations (e.g. script_tf finer than
 * input_tf, request.security TF below the chart TF without a supported
 * lower-TF emulation, missing input_tf when securities are registered).
 */
PF_API const char* strategy_get_last_error(pf_strategy_t s) {
    if (!s) return nullptr;
    return static_cast<pineforge::BacktestEngine*>(s)->last_error().c_str();
}

PF_API void strategy_set_trade_start_time(pf_strategy_t s, int64_t timestamp_ms) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_trade_start_time(timestamp_ms);
}

PF_API int strategy_stream_begin(pf_strategy_t s,
                                 const pf_bar_t* warmup_bars,
                                 int n_warmup,
                                 const char* input_tf,
                                 const char* script_tf) {
    if (!s) return -1;
    auto* engine = static_cast<pineforge::BacktestEngine*>(s);
    const auto* bars = reinterpret_cast<const pineforge::Bar*>(warmup_bars);
    return engine->stream_begin(
        bars, n_warmup,
        input_tf ? std::string(input_tf) : std::string(),
        script_tf ? std::string(script_tf) : std::string()) ? 0 : -1;
}

PF_API int strategy_stream_push_tick(pf_strategy_t s,
                                     const pf_trade_tick_t* tick) {
    if (!s || !tick) return -1;
    const pineforge::TradeTick native{
        tick->timestamp,
        tick->trade_id,
        tick->price,
        tick->qty,
        tick->is_buyer_maker != 0,
    };
    return static_cast<pineforge::BacktestEngine*>(s)->stream_push_tick(native)
        ? 0 : -1;
}

PF_API int strategy_stream_push_ticks(pf_strategy_t s,
                                      const pf_trade_tick_t* ticks,
                                      int n) {
    if (!s || n < 0 || (n > 0 && !ticks)) return -1;
    auto* engine = static_cast<pineforge::BacktestEngine*>(s);
    for (int i = 0; i < n; ++i) {
        const pineforge::TradeTick native{
            ticks[i].timestamp,
            ticks[i].trade_id,
            ticks[i].price,
            ticks[i].qty,
            ticks[i].is_buyer_maker != 0,
        };
        if (!engine->stream_push_tick(native)) return -1;
    }
    return 0;
}

PF_API int strategy_stream_advance_time(pf_strategy_t s, int64_t timestamp_ms) {
    if (!s) return -1;
    return static_cast<pineforge::BacktestEngine*>(s)
        ->stream_advance_time(timestamp_ms) ? 0 : -1;
}

PF_API int strategy_stream_end(pf_strategy_t s, int finalize_partial_input_bar) {
    if (!s) return -1;
    return static_cast<pineforge::BacktestEngine*>(s)
        ->stream_end(finalize_partial_input_bar != 0) ? 0 : -1;
}

PF_API int strategy_stream_fill_report(pf_strategy_t s, pf_report_t* out) {
    if (!s || !out) return -1;
    static_cast<pineforge::BacktestEngine*>(s)->fill_report(
        reinterpret_cast<pineforge::ReportC*>(out));
    return 0;
}

/* Override the chart TZ for ``hour``/``minute``/``dayofweek``/etc. See
 * pineforge.h docstring; NULL or empty are normalised to the legacy UTC
 * fast path. */
PF_API void strategy_set_chart_timezone(pf_strategy_t s, const char* tz) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_chart_timezone(
        tz ? std::string(tz) : std::string());
}

/* Plumb the symbol's exchange timezone / session string from the data feed
 * into syminfo_ (feeds session.ismarket / time(session)). NULL is ignored. */
PF_API void strategy_set_syminfo_timezone(pf_strategy_t s, const char* tz) {
    if (!s || !tz) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_timezone(std::string(tz));
}

PF_API void strategy_set_syminfo_session(pf_strategy_t s, const char* session) {
    if (!s || !session) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_session(std::string(session));
}

/* Inject the instrument tick size (syminfo.mintick). Drives the directional
 * stop-entry snap (long ceil / short floor) and slippage = N*mintick economics.
 * Defaults to 0.01 (crypto/equity); set per-instrument (e.g. 0.25 for ES,
 * 0.00001 for FX). Non-positive values are ignored. */
PF_API void strategy_set_syminfo_mintick(pf_strategy_t s, double mintick) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_mintick(mintick);
}

/* Inject the instrument point value (syminfo.pointvalue) — the $ per point per
 * contract multiplier applied to realized PnL and MFE/MAE. Defaults to 1.0
 * (crypto/equity); set per-instrument (e.g. 50 for ES). Non-positive ignored. */
PF_API void strategy_set_syminfo_pointvalue(pf_strategy_t s, double pointvalue) {
    if (!s) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_pointvalue(pointvalue);
}

/* Inject a fundamental/exchange metadata value (shares_outstanding_total,
 * recommendations_*, target_price_*, …) by Pine member name. Without an
 * injection the corresponding syminfo.* read returns na. NULL key ignored. */
PF_API void strategy_set_syminfo_metadata(pf_strategy_t s, const char* key,
                                          double value) {
    if (!s || !key) return;
    static_cast<pineforge::BacktestEngine*>(s)->set_syminfo_metadata(
        std::string(key), value);
}

/* See PF_ABI_VERSION doc in pineforge.h. */
PF_API int pf_abi_version(void) { return PF_ABI_VERSION; }

/* Return the runtime library version. */
PF_API pf_version_t pf_version_get(void) {
    pf_version_t v;
    v.major      = PINEFORGE_VERSION_MAJOR;
    v.minor      = PINEFORGE_VERSION_MINOR;
    v.patch      = PINEFORGE_VERSION_PATCH;
    v.commit_sha = PINEFORGE_GIT_SHA;
    return v;
}

PF_API const char* pf_version_string(void) {
    return PINEFORGE_VERSION_FULL;
}

} /* extern "C" */
