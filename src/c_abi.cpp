/*
 * c_abi.cpp — runtime-side implementation of the public C ABI declared
 * in <pineforge/pineforge.h>.
 *
 * Contains:
 *   - Layout-compatibility static_asserts between the public C PODs
 *     and the internal C++ types they mirror. If any of these trip,
 *     the C ABI has drifted from the internal representation — fix
 *     BEFORE shipping a .so that consumers depend on.
 *   - The runtime-library-side `extern "C"` symbols (currently just
 *     strategy_set_trace_enabled and pf_version_get). The other
 *     `extern "C"` symbols listed in pineforge.h (strategy_create,
 *     run_backtest, etc.) are emitted per-compiled-strategy by the
 *     codegen, not here.
 */

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
