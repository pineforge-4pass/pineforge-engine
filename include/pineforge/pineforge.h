/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pineforge.h — public C ABI for the PineForge runtime.
 *
 * This header is the single source of truth for the harness ↔ compiled-
 * strategy boundary. Every PineForge-generated .so exports a fixed set
 * of C symbols declared below; the Python harness (validate_detailed_
 * report.py) and any C/C++/FFI consumer of compiled strategies links
 * against this contract.
 *
 * STABILITY GUARANTEE
 * ───────────────────
 * Within the same PINEFORGE_VERSION_MAJOR, this header's POD struct
 * layouts and `extern "C"` symbol signatures are append-only. Fields
 * are never reordered, removed, or retyped; new fields may only be
 * appended at the end of structs. New functions may be added; existing
 * functions are not removed or signature-changed.
 *
 * Across major versions all bets are off. Bump
 * PINEFORGE_VERSION_MAJOR when breaking the ABI.
 *
 * SCOPE — WHAT THIS HEADER COVERS
 * ───────────────────────────────
 * ✓ Lifecycle of a compiled strategy (create / destroy)
 * ✓ Running a backtest (auto-detect or fully configured)
 * ✓ Per-strategy configuration (inputs, overrides, magnifier, trace)
 * ✓ The shape of the report returned to the harness
 *
 * SCOPE — WHAT THIS HEADER DOES NOT COVER (BY DESIGN)
 * ───────────────────────────────────────────────────
 * ✗ The contract between codegen-emitted strategy code and the runtime
 *   internals (TA classes, math, series, strategy commands). That
 *   contract stays C++ — codegen and runtime ship together and are
 *   versioned in lockstep within the closed transpiler.
 * ✗ Source-compiling strategies. Use the closed transpiler binary.
 *
 * The C++ headers under `<pineforge/engine.hpp>` etc. are *internal*
 * implementation surface — not part of this stability guarantee.
 */

#ifndef PINEFORGE_H
#define PINEFORGE_H

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────── */

#define PINEFORGE_VERSION_MAJOR 0
#define PINEFORGE_VERSION_MINOR 1
#define PINEFORGE_VERSION_PATCH 0

/* ── Visibility ──────────────────────────────────────────────────── */

#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(PINEFORGE_BUILD_SHARED)
    #define PF_API __declspec(dllexport)
  #else
    #define PF_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define PF_API __attribute__((visibility("default")))
#else
  #define PF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Magnifier distribution ──────────────────────────────────────── */
/* Bar magnifier sub-bar sampling distribution. Layout-compatible with
 * the internal C++ `pineforge::MagnifierDistribution` enum class —
 * static_assert in c_abi.cpp guarantees the values match. */

typedef enum pf_magnifier_distribution_e {
    PF_MAGNIFIER_UNIFORM       = 0,
    PF_MAGNIFIER_COSINE        = 1,
    PF_MAGNIFIER_TRIANGLE      = 2,
    PF_MAGNIFIER_ENDPOINTS     = 3,  /* default — exact O,H,L,C points + uniform fill between */
    PF_MAGNIFIER_FRONT_LOADED  = 4,
    PF_MAGNIFIER_BACK_LOADED   = 5
} pf_magnifier_distribution_t;

/* ── Bar ─────────────────────────────────────────────────────────── */
/* Single OHLCV bar pushed into the engine. Layout-compatible with the
 * internal C++ `pineforge::Bar` struct. */

typedef struct pf_bar_s {
    double  open;
    double  high;
    double  low;
    double  close;
    double  volume;
    int64_t timestamp;  /* Unix milliseconds */
} pf_bar_t;

/* ── Trade ───────────────────────────────────────────────────────── */
/* Closed-trade record returned in pf_report_t::trades.
 * Layout-compatible with internal `pineforge::TradeC`. */

typedef struct pf_trade_s {
    int64_t entry_time;
    int64_t exit_time;
    double  entry_price;
    double  exit_price;
    double  pnl;
    double  pnl_pct;
    int     is_long;        /* 1 if long, 0 if short */
    /* Max Adverse / Favorable Excursion in $ per unit qty.
     * `max_runup` = peak favorable price travel (in trade direction).
     * `max_drawdown` = peak adverse price travel (against trade). */
    double  max_runup;
    double  max_drawdown;
    double  qty;
} pf_trade_t;

/* ── Security diagnostic (per request.security site) ─────────────── */
/* Layout-compatible with internal `pineforge::SecurityDiagC`. */

typedef struct pf_security_diag_s {
    int     sec_id;
    int64_t feed_count;
    int64_t complete_count;
    int64_t partial_count;
} pf_security_diag_t;

/* ── Per-bar trace entry ─────────────────────────────────────────── */
/* Emitted by `// @pf-trace name=expr` pragmas in the source script.
 * Layout-compatible with internal `pineforge::TraceEntryC`. */

typedef struct pf_trace_entry_s {
    int64_t timestamp;
    int32_t bar_index;
    int32_t name_id;        /* index into pf_report_t::trace_names */
    double  value;
} pf_trace_entry_t;

/* ── Backtest report ─────────────────────────────────────────────── */
/* Filled by pf_run / pf_run_full and freed via pf_report_free.
 * Layout-compatible with internal `pineforge::ReportC`.
 *
 * Lifetime: arrays (`trades`, `security_diag`, `trace`, `trace_names`)
 * are heap-allocated by the runtime in pf_run / pf_run_full. The caller
 * must invoke pf_report_free exactly once on each filled report.
 * `trace_names` strings are owned by the live strategy handle until
 * pf_strategy_destroy. */

typedef struct pf_report_s {
    /* Trades */
    int             total_trades;
    pf_trade_t*     trades;
    int             trades_len;
    double          net_profit;

    /* Bar processing counts */
    int64_t         input_bars_processed;
    int64_t         script_bars_processed;

    /* Security diagnostics (request.security strategies) */
    int64_t         security_feeds_total;
    int64_t         security_complete_total;
    int64_t         security_partial_total;

    /* Bar magnifier diagnostics */
    int64_t         magnifier_sub_bars_total;
    int64_t         magnifier_sample_ticks_total;

    /* Timeframe metadata */
    int             input_tf_seconds;
    int             script_tf_seconds;
    int             script_tf_ratio;
    int             needs_aggregation;
    int             bar_magnifier_enabled;

    /* Per-security feed/eval counters (one entry per request.security site) */
    pf_security_diag_t* security_diag;
    int                 security_diag_len;

    /* Per-bar trace records (only populated when trace was enabled) */
    pf_trace_entry_t*   trace;
    int                 trace_len;
    const char**        trace_names;
    int                 trace_names_len;
} pf_report_t;

/* ── Strategy handle ─────────────────────────────────────────────── */
/* Opaque pointer to a compiled strategy instance. */

typedef void* pf_strategy_t;

/* ───────────────────────────────────────────────────────────────────
 * STRATEGY .SO EXPORTS — implemented per compiled strategy
 * ───────────────────────────────────────────────────────────────────
 *
 * Each .so emitted by the codegen exports the following symbols. The
 * runtime library itself does NOT define them — they are per-strategy
 * implementations generated by the transpiler.
 *
 * Note on naming: these are the legacy unprefixed names retained for
 * backward compatibility with the existing harness. Future major
 * versions may introduce `pf_`-prefixed equivalents and deprecate the
 * unprefixed forms.
 */

/* Allocate a new strategy instance. The optional `params_json` argument
 * is currently ignored; pass NULL. Returns NULL on allocation failure.
 * Caller owns the returned handle and must release it via strategy_free. */
PF_API pf_strategy_t strategy_create(const char* params_json);

/* Release a strategy handle previously returned by strategy_create.
 * Safe to call with NULL. Invalidates any pf_report_t::trace_names
 * pointers obtained from this strategy. */
PF_API void strategy_free(pf_strategy_t s);

/* Run a backtest using auto-detected timeframe and no bar magnifier.
 * `bars` must be non-NULL with length `n` >= 0. `out` must be non-NULL;
 * its fields are populated with heap allocations the caller must free
 * via pf_report_free. */
PF_API void run_backtest(pf_strategy_t s,
                         pf_bar_t* bars,
                         int n,
                         pf_report_t* out);

/* Run a backtest with full timeframe / magnifier configuration.
 *
 * `input_tf` / `script_tf` may be empty strings — the runtime then
 * auto-detects the input timeframe from bar timestamps and defaults
 * `script_tf` to it.
 *
 * `bar_magnifier` is treated as boolean (0 / non-zero).
 * `magnifier_samples` is the number of intra-bar samples per parent bar
 * when magnifier is enabled (typical: 4). */
PF_API void run_backtest_full(pf_strategy_t s,
                              pf_bar_t* bars,
                              int n,
                              const char* input_tf,
                              const char* script_tf,
                              int bar_magnifier,
                              int magnifier_samples,
                              pf_magnifier_distribution_t magnifier_dist,
                              pf_report_t* out);

/* Free heap allocations attached to a filled report. The pf_report_t
 * struct itself is caller-owned (typically stack); only the embedded
 * arrays are runtime-allocated. Safe to call with NULL or with a report
 * that has already been freed (idempotent best-effort). */
PF_API void report_free(pf_report_t* report);

/* ── Per-strategy configuration ─────────────────────────────────── */
/* Set a Pine `input.*()` value override before running a backtest.
 * `key` is the input's title (or fallback identifier); `value` is the
 * serialized form (numbers as decimal strings, booleans as "true" /
 * "false"). Calls after run_backtest are accepted but only take effect
 * on subsequent runs. */
PF_API void strategy_set_input(pf_strategy_t s,
                               const char* key,
                               const char* value);

/* Override a `strategy(...)` declaration parameter — keys include
 * `initial_capital`, `commission_value`, `default_qty_value`,
 * `pyramiding`, `slippage`, `process_orders_on_close`,
 * `close_entries_rule`, `default_qty_type`, `commission_type`. */
PF_API void strategy_set_override(pf_strategy_t s,
                                  const char* key,
                                  const char* value);

/* Toggle volume-weighted magnifier sampling. Has no effect unless bar
 * magnifier is enabled in run_backtest_full. */
PF_API void strategy_set_magnifier_volume_weighted(pf_strategy_t s,
                                                   int on);

/* ───────────────────────────────────────────────────────────────────
 * RUNTIME LIBRARY EXPORTS — implemented in libruntime
 * ─────────────────────────────────────────────────────────────────── */

/* Toggle per-bar trace recording. Default off (zero-cost when off).
 * The harness calls this to enable trace capture for a specific
 * validation run; corresponding `// @pf-trace` pragmas in the source
 * script must already have been compiled into the .so. */
PF_API void strategy_set_trace_enabled(pf_strategy_t s, int on);

/* ── Version query ──────────────────────────────────────────────── */

typedef struct pf_version_s {
    int         major;
    int         minor;
    int         patch;
    const char* commit_sha;  /* may be empty string if unknown */
} pf_version_t;

PF_API pf_version_t pf_version_get(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_H */
