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

/* Macros (PINEFORGE_VERSION_MAJOR / _MINOR / _PATCH / _STRING / _FULL,
 * PINEFORGE_GIT_SHA) live in the generated <pineforge/version.h>. */
#include <pineforge/version.h>

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

/** @defgroup pf_types Types
 *  @brief POD types and enums passed across the C ABI.
 *  @{
 */

/** Bar-magnifier sub-bar sampling distribution.
 *
 *  Selects how intra-bar synthetic ticks are placed when the bar
 *  magnifier is enabled in #run_backtest_full. Layout-compatible with the
 *  internal C++ `pineforge::MagnifierDistribution` enum class — a
 *  `static_assert` in `c_abi.cpp` guarantees the integer values match. */
typedef enum pf_magnifier_distribution_e {
    PF_MAGNIFIER_UNIFORM       = 0, /**< Uniform spacing across the parent bar. */
    PF_MAGNIFIER_COSINE        = 1, /**< Cosine-tapered density. */
    PF_MAGNIFIER_TRIANGLE      = 2, /**< Triangle-tapered density. */
    PF_MAGNIFIER_ENDPOINTS     = 3, /**< Default — exact O,H,L,C points plus uniform fill between. */
    PF_MAGNIFIER_FRONT_LOADED  = 4, /**< Sample density biased toward bar open. */
    PF_MAGNIFIER_BACK_LOADED   = 5  /**< Sample density biased toward bar close. */
} pf_magnifier_distribution_t;

/** Single OHLCV bar pushed into the engine.
 *
 *  Layout-compatible with the internal C++ `pineforge::Bar` struct. */
typedef struct pf_bar_s {
    double  open;       /**< Open price.   */
    double  high;       /**< High price.   */
    double  low;        /**< Low price.    */
    double  close;      /**< Close price.  */
    double  volume;     /**< Bar volume.   */
    int64_t timestamp;  /**< Bar open time, Unix milliseconds. */
} pf_bar_t;

/** Closed-trade record returned in pf_report_t::trades.
 *
 *  Layout-compatible with internal `pineforge::TradeC`. */
typedef struct pf_trade_s {
    int64_t entry_time;     /**< Entry fill time (Unix ms). */
    int64_t exit_time;      /**< Exit  fill time (Unix ms). */
    double  entry_price;    /**< Entry fill price (incl. slippage). */
    double  exit_price;     /**< Exit  fill price (incl. slippage). */
    double  pnl;            /**< Net realized PnL in account currency. */
    double  pnl_pct;        /**< Net realized PnL as a percentage of entry capital. */
    int     is_long;        /**< 1 if long, 0 if short. */
    double  max_runup;      /**< Peak favorable price travel during the trade ($/unit qty). */
    double  max_drawdown;   /**< Peak adverse  price travel during the trade ($/unit qty). */
    double  qty;            /**< Filled quantity. */
} pf_trade_t;

/** Per-`request.security()` site diagnostic counters.
 *
 *  Layout-compatible with internal `pineforge::SecurityDiagC`. */
typedef struct pf_security_diag_s {
    int     sec_id;          /**< Stable id for the request.security site. */
    int64_t feed_count;      /**< Higher-TF feed bars consumed. */
    int64_t complete_count;  /**< Evaluations on completed parent bars. */
    int64_t partial_count;   /**< Evaluations on still-forming parent bars. */
} pf_security_diag_t;

/** Single per-bar trace entry.
 *
 *  Emitted when the source script contains `// @pf-trace name=expr`
 *  pragmas and tracing is enabled via #strategy_set_trace_enabled.
 *  Layout-compatible with internal `pineforge::TraceEntryC`. */
typedef struct pf_trace_entry_s {
    int64_t timestamp;  /**< Bar timestamp (Unix ms). */
    int32_t bar_index;  /**< Zero-based bar index. */
    int32_t name_id;    /**< Index into pf_report_t::trace_names. */
    double  value;      /**< Traced expression value on this bar. */
} pf_trace_entry_t;

/** Backtest report filled by #run_backtest / #run_backtest_full.
 *
 *  Layout-compatible with internal `pineforge::ReportC`.
 *
 *  ### Ownership and lifetime
 *  The struct itself is caller-owned (typically stack). The embedded
 *  arrays (`trades`, `security_diag`, `trace`, `trace_names`) are
 *  heap-allocated by the runtime; the caller must invoke #report_free
 *  exactly once on each filled report. `trace_names` string pointers
 *  remain owned by the strategy handle until #strategy_free. */

typedef struct pf_report_s {
    /* Trades */
    int             total_trades;       /**< Closed-trade count (== trades_len). */
    pf_trade_t*     trades;             /**< Heap array of closed trades. */
    int             trades_len;         /**< Length of #trades. */
    double          net_profit;         /**< Sum of all closed-trade PnL. */

    /* Bar processing counts */
    int64_t         input_bars_processed;   /**< Source-feed bars consumed. */
    int64_t         script_bars_processed;  /**< Script-timeframe bars evaluated. */

    /* Security diagnostics */
    int64_t         security_feeds_total;     /**< Total higher-TF feed bars across all security sites. */
    int64_t         security_complete_total;  /**< Total complete-bar evals across all security sites. */
    int64_t         security_partial_total;   /**< Total partial-bar evals across all security sites. */

    /* Bar magnifier diagnostics */
    int64_t         magnifier_sub_bars_total;     /**< Sub-bars synthesized by the magnifier. */
    int64_t         magnifier_sample_ticks_total; /**< Sample ticks visited by the magnifier. */

    /* Timeframe metadata */
    int             input_tf_seconds;       /**< Detected/configured input timeframe (seconds). */
    int             script_tf_seconds;      /**< Script timeframe (seconds). */
    int             script_tf_ratio;        /**< script_tf_seconds / input_tf_seconds. */
    int             needs_aggregation;      /**< 1 if input → script TF aggregation was performed. */
    int             bar_magnifier_enabled;  /**< 1 if magnifier was active for this run. */

    /* Per-security feed/eval counters */
    pf_security_diag_t* security_diag;      /**< One entry per request.security site. */
    int                 security_diag_len;  /**< Length of #security_diag. */

    /* Per-bar trace records */
    pf_trace_entry_t*   trace;              /**< Per-bar trace records (empty unless tracing enabled). */
    int                 trace_len;          /**< Length of #trace. */
    const char**        trace_names;        /**< Names indexed by pf_trace_entry_t::name_id. */
    int                 trace_names_len;    /**< Length of #trace_names. */
} pf_report_t;

/** @} */ /* end of pf_types */

/** Opaque handle to a compiled strategy instance. */
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

/** @defgroup pf_lifecycle Strategy lifecycle
 *  @brief Create, run, and destroy a compiled strategy instance.
 *  @{
 */

/** Allocate a new strategy instance.
 *
 *  @param params_json  Currently ignored; pass `NULL`.
 *  @return Strategy handle, or `NULL` on allocation failure.
 *
 *  Caller owns the returned handle and must release it via #strategy_free. */
PF_API pf_strategy_t strategy_create(const char* params_json);

/** Release a strategy handle previously returned by #strategy_create.
 *
 *  Safe to call with `NULL`. Invalidates any `pf_report_t::trace_names`
 *  pointers obtained from this strategy. */
PF_API void strategy_free(pf_strategy_t s);

/** Run a backtest with auto-detected timeframe and no bar magnifier.
 *
 *  @param s     Strategy handle from #strategy_create.
 *  @param bars  Non-NULL pointer to OHLCV bars (length @p n).
 *  @param n     Bar count (>= 0).
 *  @param out   Non-NULL output report. Fields are populated with heap
 *               allocations the caller must release via #report_free. */
PF_API void run_backtest(pf_strategy_t s,
                         pf_bar_t* bars,
                         int n,
                         pf_report_t* out);

/** Run a backtest with explicit timeframe and magnifier configuration.
 *
 *  @param s                  Strategy handle.
 *  @param bars               Bar feed.
 *  @param n                  Bar count.
 *  @param input_tf           Input timeframe ("1", "5", "15", "60", "1D", ...).
 *                            Empty string → auto-detect from bar timestamps.
 *  @param script_tf          Script timeframe. Empty string → defaults to @p input_tf.
 *  @param bar_magnifier      Boolean (0 / non-zero) — enable bar magnifier.
 *  @param magnifier_samples  Sub-bar samples per parent bar (typical: 4).
 *  @param magnifier_dist     Sampling distribution (see #pf_magnifier_distribution_t).
 *  @param out                Output report. Free with #report_free. */
PF_API void run_backtest_full(pf_strategy_t s,
                              pf_bar_t* bars,
                              int n,
                              const char* input_tf,
                              const char* script_tf,
                              int bar_magnifier,
                              int magnifier_samples,
                              pf_magnifier_distribution_t magnifier_dist,
                              pf_report_t* out);

/** Free heap arrays attached to a filled report.
 *
 *  Idempotent. Safe to call with `NULL` or an already-freed report.
 *  The `pf_report_t` struct itself is caller-owned. */
PF_API void report_free(pf_report_t* report);

/** @} */ /* end of pf_lifecycle */

/** @defgroup pf_config Per-strategy configuration
 *  @brief Override @c input.*() values, `strategy(...)` params, and runtime knobs.
 *  @{
 */

/** Override a Pine @c input.*() value before the next run.
 *
 *  @param s      Strategy handle.
 *  @param key    The input's title (or fallback identifier).
 *  @param value  Serialized value — numbers as decimal strings,
 *                booleans as `"true"` / `"false"`.
 *
 *  Calls after #run_backtest are accepted but only take effect on
 *  subsequent runs. */
PF_API void strategy_set_input(pf_strategy_t s,
                               const char* key,
                               const char* value);

/** Override a `strategy(...)` declaration parameter.
 *
 *  Recognised @p key values: `initial_capital`, `commission_value`,
 *  `default_qty_value`, `pyramiding`, `slippage`,
 *  `process_orders_on_close`, `close_entries_rule`, `default_qty_type`,
 *  `commission_type`. */
PF_API void strategy_set_override(pf_strategy_t s,
                                  const char* key,
                                  const char* value);

/** Toggle volume-weighted bar-magnifier sampling.
 *
 *  Has no effect unless the bar magnifier is enabled in
 *  #run_backtest_full. */
PF_API void strategy_set_magnifier_volume_weighted(pf_strategy_t s,
                                                   int on);

/* ───────────────────────────────────────────────────────────────────
 * RUNTIME LIBRARY EXPORTS — implemented in libruntime
 * ─────────────────────────────────────────────────────────────────── */

/** Toggle per-bar trace recording. Default off (zero-cost when off).
 *
 *  Enables capture for `// @pf-trace name=expr` pragmas already compiled
 *  into the strategy `.so`. Trace records appear in pf_report_t::trace. */
PF_API void strategy_set_trace_enabled(pf_strategy_t s, int on);

/** Set the earliest Unix-ms timestamp at which strategy order commands
 *  may fire.
 *
 *  Earlier bars still execute user code and warm TA/series state, but
 *  `strategy.entry/order/exit/close` commands are ignored. */
PF_API void strategy_set_trade_start_time(pf_strategy_t s, int64_t timestamp_ms);

/** Set the strategy's chart timezone (IANA / POSIX TZ string).
 *
 *  Pine builtins ``hour``, ``minute``, ``second``, ``dayofmonth``,
 *  ``dayofweek``, ``month``, ``year`` and ``weekofyear`` return the
 *  wall-clock for the chart's timezone — TV exports trade rows in chart
 *  TZ too. Engine bars are stored as Unix-ms (UTC), so without this
 *  override these builtins return UTC and silently diverge from TV by N
 *  hours when the chart is on a non-UTC zone (Asia/Taipei = UTC+8 is the
 *  validator default).
 *
 *  Pass `NULL`, `""`, `"UTC"` or `"Etc/UTC"` for the legacy UTC
 *  behaviour (cheap, mutex-free). Any other value names a TZ resolved by
 *  the system tzdata; the per-bar decomposition then runs under a
 *  process-global mutex so multi-threaded harnesses don't corrupt each
 *  other's wall time.
 *
 *  Should be called before #run_backtest / #run_backtest_full. Persists
 *  across runs on the same strategy handle until overridden. */
PF_API void strategy_set_chart_timezone(pf_strategy_t s, const char* tz);

/** Returns the error message captured by the most recent #run_backtest /
 *  #run_backtest_full call on this strategy.
 *
 *  Returns an empty string when the run completed normally, or `NULL`
 *  only when `s` itself is `NULL`. The pointer is owned by the engine
 *  and remains valid until the next #run_backtest* call (which clears
 *  the captured error before it begins).
 *
 *  The runtime catches every `std::exception` derivative inside the
 *  engine's run loop so the C ABI never unwinds a C++ exception across
 *  the `extern "C"` boundary. Consumers must check this after every
 *  run to surface engine-rejected configurations such as a script
 *  timeframe finer than the input timeframe, a `request.security`
 *  timeframe below the chart timeframe without a supported lower-TF
 *  emulation, or a missing input timeframe when securities are
 *  registered. */
PF_API const char* strategy_get_last_error(pf_strategy_t s);

/** @} */ /* end of pf_config */

/** @defgroup pf_version Version query
 *  @brief Runtime version metadata.
 *  @{
 */

/** Runtime version descriptor returned by #pf_version_get. */
typedef struct pf_version_s {
    int         major;       /**< Major version. */
    int         minor;       /**< Minor version. */
    int         patch;       /**< Patch version. */
    const char* commit_sha;  /**< Short git commit SHA, or `""` if unknown. */
} pf_version_t;

/** @return Linked runtime version. */
PF_API pf_version_t pf_version_get(void);

/** Full git-derived version descriptor.
 *
 *  Returns `"MAJOR.MINOR.PATCH[-N-gSHA[-dirty]]"` for git checkouts, or
 *  plain `"MAJOR.MINOR.PATCH"` for tarball builds. The pointer is to a
 *  static string with program lifetime; do not free. */
PF_API const char* pf_version_string(void);

/** @} */ /* end of pf_version */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_H */
