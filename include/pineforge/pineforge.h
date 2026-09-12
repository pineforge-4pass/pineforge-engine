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

/* pf_pending_order_v1_t / pf_field_desc_t -- the generated, C-compatible POD
 * mirror of the engine's resting-order record (ABI v4, task 7). Regenerate
 * with scripts/gen_pending_order_mirror.py; never edit by hand. */
#include <pineforge/pending_order_mirror.hpp>

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

/** Monotonic ABI version of pf_report_t / pf_trade_t layout. Bumped
 *  whenever a caller-visible struct grows. Consumers MUST verify
 *  pf_abi_version() == PF_ABI_VERSION before calling run_backtest.
 *  pf_report_t is caller-allocated: growth causes silent stack corruption
 *  in old callers that under-size the struct. pf_trade_t is runtime-
 *  allocated: growth causes array-stride misindexing in old readers that
 *  iterate the trades array with the stale sizeof. Value 2 = first
 *  versioned layout (metrics + equity curve); .so files predating this
 *  macro have no pf_abi_version symbol — treat dlsym failure as
 *  version 1. Value 3 appends pf_trade_t::open_at_end (the range-end
 *  close flag); a v2 reader iterating trades with the v2 stride would
 *  misindex every row after the first. Value 4 appends the live-runtime
 *  accessors and the per-bar broker-state hash array to pf_report_t. */
#define PF_ABI_VERSION 4

/** Feature probe for the opt-in split chart/request.security feed boundary.
 *  When defined, #strategy_set_aux_security_feed is available. */
#define PINEFORGE_HAS_AUX_SECURITY_FEED_V1 1

/** Feature probe for native higher-timeframe request.security feeds.
 *  When defined, #strategy_set_native_security_feed is available. */
#define PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1 1

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

/** One provider-neutral realtime executed-trade update.
 *
 *  `sequence` is optional: pass 0 when the normalized source has no stable
 *  ordering key. Non-zero values must increase strictly within a stream.
 *  `quantity` is expressed in the configured symbol's volume units and is
 *  accumulated into the input bar's volume. The source adapter owns all
 *  provider-specific fields and normalization. */
typedef struct pf_trade_tick_s {
    int64_t  timestamp;  /**< Source event time, Unix milliseconds. */
    uint64_t sequence;   /**< Normalized per-stream sequence, or 0. */
    double   price;      /**< Executed trade price (> 0). */
    double   quantity;   /**< Traded quantity in symbol volume units (>= 0). */
} pf_trade_tick_t;

/** Closed-trade record returned in pf_report_t::trades.
 *
 *  Layout-compatible with internal `pineforge::TradeC`. */
typedef struct pf_trade_s {
    int64_t entry_time;     /**< Entry fill time (Unix ms). */
    int64_t exit_time;      /**< Exit  fill time (Unix ms). */
    double  entry_price;    /**< Entry fill price (incl. slippage). */
    double  exit_price;     /**< Exit  fill price (incl. slippage). */
    double  pnl;            /**< Net realized PnL in account currency (commission-inclusive). */
    double  pnl_pct;        /**< Net return-on-cost in percent: pnl (NET of commission) /
                             *   entry cost (entry_price * qty * pointvalue) * 100. This is
                             *   TradingView's "Net P&L %" convention, arbitrated 2026-06-12
                             *   against a real TV export (trade #258 short: 102.44 USD on a
                             *   2276.66 entry => 4.50%). Degenerates to the old gross
                             *   (exit/entry-1)*100 form for longs with zero commission;
                             *   the previous short form (entry/exit-1)*100 was wrong on
                             *   large moves. Sign always matches pnl. */
    int     is_long;        /**< 1 if long, 0 if short. */
    double  max_runup;      /**< Peak favorable price travel during the trade ($/unit qty). */
    double  max_drawdown;   /**< Peak adverse  price travel during the trade ($/unit qty). */
    double  qty;            /**< Filled quantity. */
    double  commission;     /**< Entry+exit commission actually deducted from pnl
                             *   (account currency). pnl is already net of this. */
    int32_t entry_bar_index;/**< Script-bar index of the entry fill (0-based). */
    int32_t exit_bar_index; /**< Script-bar index of the exit fill (0-based). */
    int32_t open_at_end;    /**< 1 when this row is the RANGE-END close of a position
                             *   that was still open after the final bar; 0 for an
                             *   exit the script or a bracket produced. TradingView's
                             *   deep-backtest report does not leave the last position
                             *   open: it reports it as a closed trade whose exit leg is
                             *   the range's last bar at that bar's CLOSE, with an
                             *   empty exit Signal, and counts it in closedTrades
                             *   (orb-lite on NYSE:F 1D: Entry short 2026-03-16 @ 11.82,
                             *   Exit 2026-04-30 @ 12.08 = the last close,
                             *   closedTrades:1). The engine emulates that row
                             *   (operator decision 2026-09-02): exit_time is the last
                             *   script bar's label, exit_price its mintick-rounded
                             *   close with no slippage, commission per the strategy's
                             *   rules, pnl/pnl_pct/excursions as for any close.
                             *   Appended in ABI v3. */
} pf_trade_t;

/** Trade-level statistics block — computed once each for all / long / short.
 *
 *  Loss-side fields (`gross_loss`, `avg_loss`, `largest_loss`) are
 *  **positive magnitudes** (absolute values of the underlying negative PnL). */
typedef struct pf_trade_stats_s {
    int32_t num_trades;                /**< Closed trades in this block (all / long-only / short-only). */
    int32_t num_wins;                  /**< Trades with pnl > 0. */
    int32_t num_losses;                /**< Trades with pnl < 0. */
    int32_t num_even;                  /**< Trades with pnl == 0.0 exactly; breaks both win and loss
                                        *   streaks; excluded from win/loss averages.
                                        *   Invariant: num_trades == num_wins + num_losses + num_even. */
    double  percent_profitable;        /**< 100 * num_wins / num_trades, in PERCENT (0-100).
                                        *   NaN when num_trades == 0. */
    double  net_profit;                /**< Sum of pnl (account currency, net of commission). */
    double  net_profit_pct;            /**< net_profit as a percent of initial capital (0-100 scale).
                                        *   NaN when initial capital <= 0. */
    double  gross_profit;              /**< Sum of winning pnl. */
    double  gross_profit_pct;          /**< gross_profit as a percent of initial capital (0-100 scale).
                                        *   NaN when initial capital <= 0. */
    double  gross_loss;                /**< Sum of |losing pnl| — POSITIVE magnitude (TV display convention). */
    double  gross_loss_pct;            /**< gross_loss as a percent of initial capital (0-100 scale).
                                        *   NaN when initial capital <= 0. */
    double  profit_factor;             /**< gross_profit / gross_loss. NaN when gross_loss == 0. */
    double  avg_trade;                 /**< net_profit / num_trades. NaN when num_trades == 0. */
    double  avg_trade_pct;             /**< Mean of per-trade pnl_pct over all trades.
                                        *   NaN when num_trades == 0. */
    double  avg_win;                   /**< gross_profit / num_wins. NaN when num_wins == 0. */
    double  avg_win_pct;               /**< Mean of per-trade pnl_pct over winning trades.
                                        *   NaN when num_wins == 0. */
    double  avg_loss;                  /**< gross_loss / num_losses (positive magnitude).
                                        *   NaN when num_losses == 0. */
    double  avg_loss_pct;              /**< Mean of the NEGATED pnl_pct of the losing trades. Since
                                        *   pnl_pct is net return-on-cost (sign matches pnl), this is
                                        *   a genuinely POSITIVE magnitude. Basis = pf_trade_t::pnl_pct.
                                        *   NaN when num_losses == 0. */
    double  ratio_avg_win_avg_loss;    /**< avg_win / avg_loss. NaN unless both sides non-empty. */
    double  largest_win;               /**< Single largest pnl among winning trades.
                                        *   NaN when num_wins == 0. */
    double  largest_win_pct;           /**< Maximum pnl_pct over winning trades — an INDEPENDENT
                                        *   maximum, not the pct of the largest-USD win (TV convention,
                                        *   validated 2026-06-12 vs TV export).
                                        *   NaN when num_wins == 0. */
    double  largest_loss;              /**< Single largest |pnl| among losing trades (positive magnitude).
                                        *   NaN when num_losses == 0. */
    double  largest_loss_pct;          /**< Maximum of -pnl_pct over losing trades (positive magnitude) —
                                        *   an INDEPENDENT maximum, not the pct of the largest-USD loss
                                        *   (TV convention, validated 2026-06-12 vs TV export: All
                                        *   "Largest loss %" came from a different trade than the
                                        *   largest USD loss). NaN when num_losses == 0. */
    double  commission_paid;           /**< Sum of pf_trade_t::commission in the block. */
    double  expectancy;                /**< (num_wins/num_trades)*avg_win - (num_losses/num_trades)*avg_loss,
                                        *   account currency per trade. NaN when num_trades == 0. */
    int32_t max_consecutive_wins;      /**< Longest winning run; even trades reset both streaks. */
    int32_t max_consecutive_losses;    /**< Longest losing run; even trades reset both streaks. */
    double  avg_bars_in_trade;         /**< Mean of (exit_bar_index - entry_bar_index + 1) in SCRIPT
                                        *   bars, over all trades — inclusive of the entry bar (TV
                                        *   convention, validated 2026-06-12).
                                        *   NaN when num_trades == 0. */
    double  avg_bars_in_wins;          /**< Mean bar duration of winning trades, inclusive of the entry
                                        *   bar (TV convention, validated 2026-06-12).
                                        *   NaN when num_wins == 0. */
    double  avg_bars_in_losses;        /**< Mean bar duration of losing trades, inclusive of the entry
                                        *   bar (TV convention, validated 2026-06-12).
                                        *   NaN when num_losses == 0. */
} pf_trade_stats_t;

/** Equity-curve-derived statistics (all-trades only, like TV). */
typedef struct pf_equity_stats_s {
    double max_equity_drawdown;        /**< Peak-to-trough equity drop, positive currency magnitude. */
    double max_equity_drawdown_pct;    /**< max_equity_drawdown relative to the peak in effect
                                        *   (PERCENT 0-100). */
    double max_equity_runup;           /**< Trough-to-peak rise where the trough resets on each new
                                        *   equity peak (mirrors the engine's intra-run extremes). */
    double max_equity_runup_pct;       /**< max_equity_runup relative to that trough (PERCENT 0-100). */
    double buy_hold_return;            /**< initial_capital * (last_close/first_open - 1), currency.
                                        *   NaN when first chart open is non-finite or <= 0. */
    double buy_hold_return_pct;        /**< buy_hold_return as PERCENT.
                                        *   NaN when first chart open is non-finite or <= 0. */
    double sharpe_tv;                  /**< Month-end-resampled equity simple returns (chart timezone,
                                        *   open-time bucketing), risk-free 2%/yr (2/12 per month),
                                        *   annualized by sqrt(12). Uses sample (N-1) stddev.
                                        *   NaN with <2 monthly returns or zero deviation. */
    double sortino_tv;                 /**< Same resampling as sharpe_tv; uses population downside
                                        *   deviation vs the monthly risk-free.
                                        *   NaN with <2 monthly returns or zero deviation. */
    double sharpe_bar;                 /**< Per-script-bar returns, annualized by observed bar density
                                        *   (bars per year = (len-1)/calendar span), NOT a fixed
                                        *   calendar formula. Uses sample (N-1) stddev.
                                        *   NaN with <2 returns or zero deviation. */
    double sortino_bar;                /**< Same construction as sharpe_bar over per-bar returns;
                                        *   uses population downside deviation.
                                        *   NaN with <2 returns or zero deviation. */
    double cagr;                       /**< PERCENT per year: 100*((final_equity/initial_capital)^(1/years)-1).
                                        *   NaN when span <= 0 or either side <= 0. */
    double calmar;                     /**< cagr / max_equity_drawdown_pct — BOTH IN PERCENT, so the
                                        *   ratio is dimensionless. NaN when drawdown is 0. */
    double recovery_factor;            /**< net_profit / max_equity_drawdown (currency / currency).
                                        *   NaN when drawdown is 0. */
    double time_in_market_pct;         /**< PERCENT (0-100) of script bars with an open position
                                        *   at bar close. */
    double open_pl;                    /**< Mark-to-market open profit at the final bar. */
} pf_equity_stats_t;

/** Composite metrics container: trade stats (all / long / short) +
 *  equity-curve stats. */
typedef struct pf_metrics_s {
    pf_trade_stats_t  all, longs, shorts;
    pf_equity_stats_t equity;
} pf_metrics_t;

/** Single per-script-bar equity point.
 *
 *  `time_ms` is the script-bar **open** timestamp (Unix ms).
 *  `equity` = `initial_capital` + `net_profit` + `open_profit` at bar close. */
typedef struct pf_equity_point_s {
    int64_t time_ms;       /**< Script-bar OPEN timestamp (Unix ms). */
    double  equity;        /**< initial_capital + net_profit + open_profit. */
    double  open_profit;   /**< Mark-to-market open P&L at bar close. */
} pf_equity_point_t;

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
 *  arrays (`trades`, `security_diag`, `trace`, `trace_names`,
 *  `equity_curve`, `broker_state_hash`) are heap-allocated by the
 *  runtime; the caller must invoke #report_free exactly once on each
 *  filled report. `trace_names` string pointers remain owned by the
 *  strategy handle until #strategy_free. */

typedef struct pf_report_s {
    /* Trades */
    int             total_trades;       /**< Closed-trade count (== trades_len), including
                                         *   the range-end close of a position still open
                                         *   after the final bar (pf_trade_t::open_at_end). */
    pf_trade_t*     trades;             /**< Heap array of closed trades; script-driven exits
                                         *   first, then the range-end rows (open_at_end=1). */
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

    /* Computed trading metrics. Trade-based blocks reported for all /
     * long-only / short-only; equity-based stats are all-trades only.
     * Loss-side fields are positive magnitudes. Undefined values are NaN
     * (see per-field docs). */
    pf_metrics_t        metrics;
    /* Per-script-bar equity curve. time_ms is the script-bar OPEN
     * timestamp; equity = initial_capital + net_profit + open_profit at
     * bar close. Heap-allocated; freed by report_free. len ==
     * script_bars_processed, EXCEPT after a mid-run error (check
     * strategy_get_last_error): an exception can truncate the curve, and
     * metrics then describe the truncated prefix. NOTE int64_t length
     * (ctypes: c_int64). */
    pf_equity_point_t*  equity_curve;
    int64_t             equity_curve_len;
    /* Per-script-bar broker-state hash, filled when
     * #strategy_set_broker_state_hash_recording is on; freed by
     * #report_free. NULL / 0-length when recording was off (default) or no
     * script bars were dispatched. When populated, len ==
     * script_bars_processed and the last element equals
     * #strategy_broker_state_hash's value at the end of the run.
     * ABI v4. */
    uint64_t*           broker_state_hash;
    int64_t             broker_state_hash_len;
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
 *
 *  NOTE: Per-strategy symbols (strategy_create, run_backtest, etc.) are
 *  emitted by the codegen with internal C++ types (ReportC, Bar) that are
 *  layout-compatible but type-distinct from the public C PODs below.
 *  Guard with PINEFORGE_NO_STRATEGY_DECLS so engine.hpp can include this
 *  header for its POD types without conflicting with per-strategy TU
 *  definitions.
 */

/** Native execution contract query. Returns 1 Legacy, 2 NativeMarketV1, -1 invalid.
 *  Absence of this symbol on a known ABI4/stream-v1 library means legacy. */
PF_API int strategy_execution_contract(pf_strategy_t s);

/** Versioned native run specification. All string pointers are non-null.
 *  session may be empty (all-day literal). session_key, timeframes, timezone
 *  and tickerid are nonempty. optional_mask bits: 0 quantity_grid, 1
 *  max_abs_units, 2 initial_margin_fraction, 3 max_open_lots. */
typedef struct pf_native_run_spec_v1 {
    uint32_t struct_size;
    const char *session_key; uint64_t run_number;
    const char *input_tf, *script_tf;
    const char *ticker, *tickerid, *type, *currency, *basecurrency, *description, *volumetype;
    const char *timezone, *session, *chart_timezone;
    double initial_capital, point_value, account_fx, price_tick;
    uint32_t slippage_ticks, fee_kind; double fee_value;
    uint32_t optional_mask, close_execution, allowed_open_directions;
    double quantity_grid, max_abs_units, initial_margin_fraction;
    uint64_t max_open_lots;
} pf_native_run_spec_v1;

/** Apply a native v1 specification. Returns 0 on success, -1 on failure.
 *  Legacy handles refuse without native state mutation. */
PF_API int strategy_configure_native_v1(pf_strategy_t s, const pf_native_run_spec_v1* spec);

#ifndef PINEFORGE_NO_STRATEGY_DECLS

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

#endif /* PINEFORGE_NO_STRATEGY_DECLS */

/* ───────────────────────────────────────────────────────────────────
 * RUNTIME LIBRARY EXPORTS — implemented in libpineforge
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

/** @} */ /* end of pf_config */

/** @addtogroup pf_lifecycle
 *  @{
 */

/** Return the physical entry incarnation for one closed-trade row.
 *
 *  Partial-close/FIFO fragments emitted from the same physical entry share
 *  this value. Distinct broker entry objects receive distinct monotonically
 *  increasing values even when Pine reuses the same user-visible entry ID.
 *  The value is scoped to one strategy run and is intended as report
 *  provenance, not as a stable cross-run identifier.
 *
 *  @param s            Strategy handle whose most recent run filled a report.
 *  @param trade_index  Zero-based row index into that report's `trades`
 *                      array — the script's closed trades followed by the
 *                      range-end rows (`open_at_end`, ABI v3), which carry
 *                      the incarnation of the lot they mark like any other
 *                      close.
 *  @return Non-zero physical-entry identity, or 0 for an invalid index or a
 *          legacy/synthetic trade without PendingOrder provenance. */
PF_API uint64_t strategy_closed_trade_entry_incarnation(
    pf_strategy_t s, int trade_index);

/** @} */ /* end of pf_lifecycle */

/** @defgroup pf_streaming Historical to realtime streaming
 *  @brief Warm on confirmed OHLCV and continue the same strategy instance on
 *         normalized ordered trades from any data source.
 *  @{
 */

/** Warm a strategy with confirmed OHLCV, then switch the same instance to a
 *  realtime trade stream without resetting position, equity, pending orders,
 *  Pine variables, TA state, request.security state, or timeframe aggregation.
 *
 *  The warmup must contain at least one complete fixed-duration input bar.
 *  Normalized ticks start at or after the next input bar's open. This
 *  lifecycle uses close-only strategy calculation (the Pine strategy default)
 *  while resting broker orders are evaluated on every normalized trade.
 *
 *  calc_on_order_fills, historical probe/tail overrides, timestamped FX,
 *  auxiliary and native security feeds are rejected. No every-tick strategy
 *  callback is provided; hand-written strategies follow the same lifecycle.
 *  @return 0 on success, -1 on failure. Inspect #strategy_get_last_error. */
PF_API int strategy_stream_begin(pf_strategy_t s,
                                 const pf_bar_t* warmup_bars,
                                 int n_warmup,
                                 const char* input_tf,
                                 const char* script_tf);

/** Native live extension version (1). Additive to ABI v4; callers must probe
 *  this symbol before using the confirmed-bar/action API in older modules. */
PF_API int strategy_stream_api_version(void);

/** One physical emulator lot action. is_long describes the position side:
 *  buy = is_entry == is_long. A reversal is exits followed by an entry.
 *  price/time are emulator reference values, not external broker fills.
 *  Strings are borrowed until the next stream mutation or queue clear. */
typedef struct pf_stream_order_action {
    uint64_t sequence;
    int64_t timestamp_ms;
    int32_t bar_index;
    int32_t is_entry;
    int32_t is_long;
    double quantity;
    double price;
    const char* order_id;
    const char* comment;
    uint64_t entry_incarnation;
} pf_stream_order_action_t;

/** Consume one confirmed input-timeframe bar. Its script-timeframe aggregate
 *  uses the existing batch OHLC fill kernel when complete. Close-only strategy
 *  calculation; no bar magnifier or synthetic trade ticks. The first tick/bar
 *  locks the feed mode. Off-grid/out-of-order data, invalid OHLCV and missing
 *  in-session bars fail. Calendar-closed intervals may be skipped. No padding.
 *  Returns 0/-1. On ANY input-processing failure discard/recover the instance:
 *  the operation is not transactional and can have advanced native state. */
PF_API int strategy_stream_push_bar(pf_strategy_t s, const pf_bar_t* bar);

/** Queued physical fills since the last clear, in execution order. Historical
 *  warmup and report-only range-end rows never enqueue. Sequence starts at 1
 *  after warmup and is not reset by clear. Returns -1 on a null handle. */
PF_API int strategy_stream_order_actions_len(pf_strategy_t s);
/** Copy one action; return 0 on success, -1 on invalid handle/index/output. */
PF_API int strategy_stream_order_action_get(pf_strategy_t s, int index,
                                            pf_stream_order_action_t* out);
/** Clear observed events after the caller durably journals them. */
PF_API void strategy_stream_order_actions_clear(pf_strategy_t s);
/** Versioned deterministic fingerprint of observable broker/stream state.
 *  Excludes the consumable queue and arbitrary private strategy members.
 *  This is a replay check, not a complete state snapshot or cryptographic hash.
 *  Fresh replay must use deterministic strategy code, the same pinned engine
 *  build and configuration. Fingerprint representations may change between builds.
 *  Returns 0 for NULL. */
PF_API uint64_t strategy_stream_state_hash(pf_strategy_t s);

/** Push one normalized realtime trade. Returns 0 on success, -1 on failure. */
PF_API int strategy_stream_push_tick(pf_strategy_t s,
                                     const pf_trade_tick_t* tick);

/** Push an ordered batch of realtime trades. Semantically identical to
 *  repeated #strategy_stream_push_tick calls, with lower FFI overhead. */
PF_API int strategy_stream_push_ticks(pf_strategy_t s,
                                      const pf_trade_tick_t* ticks,
                                      int n);

/** Advance the stream clock and close every input bar whose end is <= the
 *  supplied time. Quiet in-session intervals become zero-volume carry-forward
 *  bars; intervals outside the configured syminfo session are skipped. */
PF_API int strategy_stream_advance_time(pf_strategy_t s, int64_t timestamp_ms);

/** End a realtime stream. When @p finalize_partial_input_bar is non-zero, the
 *  currently forming input bar is dispatched before ending; normally callers
 *  should first advance to a confirmed boundary and pass zero here. */
PF_API int strategy_stream_end(pf_strategy_t s, int finalize_partial_input_bar);

/** Snapshot the cumulative warmup + realtime report. The embedded arrays are
 *  caller-owned after return and must be released with #report_free. */
PF_API int strategy_stream_fill_report(pf_strategy_t s, pf_report_t* out);

/** @} */ /* end of pf_streaming */

/** @defgroup pf_live Live-runtime surface (ABI v4)
 *  Default-off flags and read-only accessors used by pineforge-live. None of
 *  them changes a historical run unless enabled. @{ */
/** Request cooperative abort of the run in progress (see c_abi.cpp). */
PF_API void strategy_request_abort(pf_strategy_t s);
/** 0 = completed, 1 = NOT_COMPLETED (aborted), -1 = @p s is NULL. */
PF_API int  strategy_last_run_status(pf_strategy_t s);
/** Live-runtime tail semantics (spec §3.1): the LAST bar of the array fed to
 *  every subsequent run() is a still-forming bar, not the chart's rightmost
 *  historical bar. This is persistent configuration, not a one-shot flag --
 *  it stays in effect until a caller passes @p on == 0, so a handle reused
 *  for a later plain historical replay must be explicitly turned back off.
 *  Effects when @p on is non-zero:
 *    1. `barstate.islast` is false for that bar.
 *    2. `session.islastbar` is computed from the bucket calendar (no next
 *       bar to peek at, so it evaluates whether the next bucket -- this
 *       bar's timestamp plus one script-TF step -- falls out of session).
 *    3. `bar_index` stays put; `last_bar_index` is frozen at the horizon
 *       bar (`horizon_bars - 1`), when @p horizon_bars > 0. `last_bar_time`
 *       is exact when the horizon bar is in the script-bar array, one
 *       script-TF step per missing bar past the array's last bar
 *       otherwise; under aggregation (input_tf < script_tf) it is
 *       extrapolated from the first bar instead, and the aggregation-path
 *       caveat below applies.
 *    4. The range-end synthetic close row/trade is skipped (no
 *       `open_at_end` row); the final equity point keeps `open_profit`.
 *    5. Interior bars (every bar before the last) are unaffected.
 *  Dispatch-path scope: effects 1, 3, and 4 above are honoured on every
 *  dispatch path. Effect 2 (`session.islastbar` from the bucket calendar)
 *  is honoured only on `run_simple_bar_loop` (the input_tf == script_tf
 *  simple bar loop); the single-timeframe `run(bars, n)` overload never
 *  evaluates session predicates at all (pre-existing -- `session.ismarket`/
 *  `session.islastbar` stay at their reset-state `false` there regardless
 *  of this flag). On the non-magnifier aggregation path (input_tf <
 *  script_tf) effect 2 is UNDEFINED: the tail bar's `session.islastbar`
 *  reads the ordinary `in_session && barstate.islast` expression instead of
 *  the calendar lookahead (false there, since this flag also forces
 *  `barstate.islast` false). Callers must feed an input_tf == script_tf
 *  array until that gap closes, matching
 *  #strategy_set_probe_suppress_tail_logic's dispatch-path-scope caveat.
 *  Default off (@p on == 0): every historical run stays byte-identical to
 *  before this flag existed. */
PF_API void strategy_set_realtime_tail(pf_strategy_t s, int on, int horizon_bars);
/** Live probe tail suppression (spec §3.2): the LAST bar of the array fed to
 *  every subsequent run() runs only the broker's pre-`on_bar` steps and
 *  returns, in this order: intraday-cap deferred close, advancing native
 *  source-series history (`_push_source_series`), settling resting
 *  stop/limit orders against the bar (`process_pending_orders`), the
 *  max-intraday-loss path check (`evaluate_max_intraday_loss_over_path`),
 *  and updating per-trade extremes (`update_per_trade_extremes`).
 *  `on_bar` is never invoked for that bar, and nothing that ordinarily runs
 *  after it runs either -- no `invoke_chart_on_bar`, no
 *  `flush_same_bar_close`, no POOC second pass, no `process_margin_call`
 *  (and, under process_orders_on_close, the pre-script carried-position
 *  margin helpers), no `settle_dormant_bracket_reissues`, no
 *  post-liquidation sizing refresh. A margin call or intraday-cap close that
 *  would ordinarily fire against the forming bar therefore surfaces only at
 *  settlement (the next non-suppressed run), never against the
 *  still-forming probe bar itself.
 *  The run's last-bar fills are exactly the settled book's fills against
 *  the forming bar, and the post-run pending-order book is the book in
 *  force during that bar. This is persistent configuration, like
 *  #strategy_set_realtime_tail, and independent of it -- do not assume the
 *  two flags are coupled; set each explicitly.
 *  Dispatch-path scope (ABI v4 / live v1): this flag is honoured only on the
 *  standard `dispatch_bar` path -- the single-timeframe run loop and the
 *  input_tf == script_tf simple bar loop. It is a silent no-op under
 *  `calc_on_order_fills` (the COOF scheduler dispatches the last bar in full,
 *  `on_bar` included) and under the bar magnifier (`run_magnified_bar` never
 *  reaches `dispatch_bar`); both are gated features in v1 and a probe must
 *  not enable them. On the non-magnifier aggregation path
 *  (input_tf < script_tf) the semantics are UNDEFINED until the partial-
 *  bucket forming-bar flag lands: today the bar suppressed is whichever
 *  script bar is dispatched while walking the array's last input bar (a
 *  completed bucket, when that input bar opens a new one), and a trailing
 *  partial bucket is never dispatched at all. Callers must feed an
 *  input_tf == script_tf array until that flag exists.
 *  Clear this flag (on = 0) before `strategy_stream_begin`; the warmup
 *  replay is a run().
 *  Default off (@p on == 0): every historical run stays byte-identical to
 *  before this flag existed. */
PF_API void strategy_set_probe_suppress_tail_logic(pf_strategy_t s, int on);
/** Force this run's intrabar path order (ABI v4 live-runtime surface): the
 *  leg order every OHLC-path helper (`bar_path_uses_high_first` and
 *  everything built on it -- stop/limit fill priority, exit trail walking,
 *  dual-entry-stop arbitration, and bar-magnifier sub-bar sampling) uses for
 *  the CURRENT and every subsequent run(), until a caller sets a different
 *  mode. Values:
 *    - `0` AUTO (default): the unchanged TV-emulator rule -- the leg nearer
 *      `open` (by `|high-open|` vs `|open-low|`) goes first.
 *    - `1` HIGH_FIRST: force `O -> H -> L -> C` regardless of the bar's own
 *      shape.
 *    - `2` LOW_FIRST: force `O -> L -> H -> C` regardless of the bar's own
 *      shape.
 *  Any other @p mode is clamped to AUTO.
 *  A live probe runs the SAME forming bar under BOTH forced orders and emits
 *  only the fills that agree between the two -- a fill that depends on which
 *  leg TradingView's own (unobservable, still-forming) bar will resolve to
 *  is path-dependent and must be suppressed rather than guessed.
 *  This is persistent configuration, like #strategy_set_realtime_tail -- it
 *  stays in effect until a caller passes @p mode == 0, so a handle reused
 *  for a later plain historical replay must be explicitly set back to AUTO.
 *  Applies to run() only: a stream continued via #strategy_stream_begin
 *  dispatches its realtime ticks outside any run() and always sees AUTO,
 *  regardless of this setting.
 *  Default AUTO (@p mode == 0): every historical run stays byte-identical to
 *  before this flag existed. */
PF_API void strategy_set_path_order(pf_strategy_t s, int mode);
/** The dual-entry-stop arbitration decided on the LAST bar the most recent
 *  run() dispatched: a flat position resting exactly one long stop-only
 *  ENTRY and one short stop-only ENTRY, both touched on that bar
 *  (`dual_entry_stop_path_winner`, internal). Values mirror
 *  `internal::DualEntryStopPathWinner`'s enumerator order:
 *    - `0` None -- no such pair was arbitrated on that bar (not flat, no
 *      matching pair, or neither/only one side touched).
 *    - `1` LongFirst -- the long stop's first-touch position on the intrabar
 *      path came first (or the two tied, which the engine always resolves
 *      in the long leg's favour).
 *    - `2` ShortFirst -- the short stop's first-touch position came first.
 *    - `-1` -- @p s is NULL.
 *  This is a per-BAR snapshot, not a live read of the engine's per-pass
 *  working state: it is written once, at the arbitration itself, and then
 *  holds for the rest of that bar even though the working state goes back
 *  to None the moment the winning side fills (position no longer flat) or
 *  its stop-entry admission is declined -- neither of which undoes the fact
 *  that TradingView's broker emulator arbitrated a real pair that bar. A
 *  caller therefore gets the right answer whether it reads this after a
 *  `strategy_set_probe_suppress_tail_logic` forming-bar probe (a single
 *  `process_pending_orders` pass) or after an ordinary
 *  `process_orders_on_close` run with no tail suppression (two passes, the
 *  winner already filled by the second).
 *  A live probe reads this after a forming-bar run to see which side the
 *  engine's own broker-emulator tie-break picked, without having to re-run
 *  and infer it from which of the two possible fills came back.
 *  Only the standard (non-`calc_on_order_fills`) dispatch path updates this
 *  value; it is a silent no-op under the COOF scheduler, mirroring
 *  #strategy_set_probe_suppress_tail_logic's dispatch-path-scope caveat. */
PF_API int strategy_last_bar_dual_entry_path(pf_strategy_t s);
/** Toggle per-script-bar broker-state hash recording (spec §3.4, ABI v4).
 *
 *  When @p on is non-zero, every subsequent run() appends
 *  #strategy_broker_state_hash's value to pf_report_t::broker_state_hash
 *  immediately after each script bar is dispatched, so the array's length
 *  matches pf_report_t::script_bars_processed. Cleared (recorded array
 *  emptied, not the flag itself) at the start of every run(); the flag is
 *  persistent configuration, like #strategy_set_realtime_tail, and stays
 *  set until a caller passes @p on == 0.
 *  Also covers #strategy_stream_begin's warmup run() and every script bar
 *  dispatched afterward by the realtime tick stream, so
 *  #strategy_stream_fill_report's cumulative report satisfies the same
 *  len == script_bars_processed invariant. Set this BEFORE
 *  #strategy_stream_begin to also record the warmup leg -- reset_run_state()
 *  (which stream_begin's internal run() invokes) empties the recorded
 *  array, not the flag, but a flag flipped on only after stream_begin
 *  returns misses the warmup bars already dispatched.
 *  Default off (@p on == 0): pf_report_t::broker_state_hash is NULL /
 *  0-length and every historical run stays byte-identical to before this
 *  flag existed. */
PF_API void strategy_set_broker_state_hash_recording(pf_strategy_t s, int on);
/** Return the broker-state hash of the FINAL state after the most recent
 *  run() (see #strategy_set_broker_state_hash_recording's doc and
 *  pf_report_t::broker_state_hash for the per-bar recording; this accessor
 *  works whether or not recording was enabled). Compare only within the same
 *  pinned engine build and configuration; this is not a serialized checkpoint.
 *  Returns 0 when @p s is
 *  NULL. */
PF_API uint64_t strategy_broker_state_hash(pf_strategy_t s);
/** Number of orders resting in the engine's pending-order book after the
 *  most recent run() (ABI v4 live-runtime surface, task 7, spec 3.6): the
 *  book in force for the NEXT bar. 0 when @p s is NULL. Read-only; a
 *  historical run is byte-identical whether or not a caller reads it. */
PF_API int strategy_pending_orders_len(pf_strategy_t s);
/** Copy the @p index-th resting order (0-based, the engine's own book
 *  order -- insertion order; broker fill priority is decided at fill time
 *  from `created_seq`, not from this index) into @p out as a
 *  pf_pending_order_v1_t value snapshot. Copies
 *  min(@p size_in, sizeof(pf_pending_order_v1_t)) bytes: an older reader
 *  with a smaller struct receives a prefix (struct_version and size first),
 *  a newer reader with a larger one receives the whole v1 and must not
 *  read past pf_pending_order_v1_t::size. Strings are NUL-terminated
 *  char[64] copies with a `_truncated` flag and a `_hash64` (FNV-1a 64 of
 *  the full string); enums are int32 values; NaN sentinels are copied
 *  verbatim. Returns 0 on success, -1 -- with nothing written -- when
 *  @p s or @p out is NULL, @p index is out of range, or @p size_in < 8
 *  (too small to hold even the `struct_version` + `size` header; every
 *  larger @p size_in is honoured as a prefix copy). The layout is
 *  self-described by #strategy_pending_order_layout. */
PF_API int strategy_pending_order_get(pf_strategy_t s, int index, void* out, size_t size_in);
/** The field table of pf_pending_order_v1_t as THIS runtime compiled it --
 *  one pf_field_desc_t {name, type, offset, size} per field, in struct
 *  order, starting with `struct_version` and `size`. Static storage: the
 *  pointer stays valid for the life of the process and needs no handle.
 *  @p count (may be NULL) receives the row count. An FFI consumer builds
 *  its struct from this table rather than from a hand-typed copy, so the
 *  mirror can grow (append-only) without breaking it. */
PF_API const pf_field_desc_t* strategy_pending_order_layout(int* count);
/** Engine-computed fill quantity of the @p index-th resting order (ABI v4
 *  live-runtime surface, task 8, spec 3.6): the contracts the entry kernel
 *  would OPEN if that order filled at @p fill_price, sized by the engine's
 *  own rules so a live runtime never re-implements them. @p fill_price is
 *  slipped the way the kernel slips it (`apply_slippage`; an entry with a
 *  limit leg takes the unslipped limit-or-better route). @p partition
 *  receives which sizing rule produced the value:
 *    - `0` EXPLICIT -- a script-supplied qty: for `strategy.entry` the
 *      lot-floored contracts (`apply_qty_step`) of a fixed qty, or the
 *      explicit percent/cash budget sized at the fill for a per-call
 *      qty_type override; a `strategy.order` explicit qty is dispatched
 *      verbatim (no lot step).
 *    - `1` FROZEN_PLACEMENT -- a quantity fixed before the fill, never
 *      re-derived from the fill price: the default percent_of_equity /
 *      cash MARKET (or strategy.order) size frozen at the signal close
 *      (`frozen_default_qty`); a MARKET's frozen broker transaction (a
 *      finalized flat pair's `paired_flat_market_transaction_qty`; a
 *      same-bar-market member's `sbmt_tx_qty` from flat or as a kept
 *      over-cap add); or what one of the two MARKET reversal kernels
 *      opens -- a same-bar-market member against an opposite live position
 *      opens the remainder `sbmt_tx_qty - min(sbmt_tx_qty, live qty)`
 *      (`apply_same_bar_market_tx_reversal`), and the exact SHORT-seed
 *      collision's final short re-opens the residual
 *      `pyramid_entries[0].qty - pyramid_entries[1].qty` after closing both
 *      lots (`short_seed_collision_final_short_is_live`). Both kernels are
 *      modelled; each reports `close_only` 1 when it opens nothing.
 *    - `2` DEFAULT_STOP_PLACEMENT -- the DEFAULT percent_of_equity <= 100
 *      pure STOP entry's placement size (`default_stop_placement_qty`,
 *      round-7 family K), when `use_default_stop_placement_qty` says the
 *      fill consumes it: created flat, filling from flat, positive fill.
 *    - `3` AT_FILL -- default sizing at the slipped fill (`calc_qty`).
 *  @p close_only receives 1 when the kernel's close-only predicate fires
 *  -- the fill closes against the live opposite position and that
 *  predicate opens no leg of its own: the order's
 *  `affordability_close_only` (entry leg declined at placement), the
 *  priced-entry `prior_cycle_close_only` rule (opposite live position whose
 *  cycle the order was not placed in -- `created_position_side !=` the
 *  live side -- and not a KI-65 `reverses_same_bar_market_from_flat`), the
 *  same-cycle frozen explicit-FIXED transaction the close consumes exactly,
 *  a finalized flat MARKET pair against an opposite position, or one of the
 *  two reversal kernels above opening nothing. Where the order was created
 *  FLAT the engine's close-only branch is `close_opposite_then_enter`: a
 *  transaction larger than the live position still opens the remainder, so
 *  a consumer compares @p qty with the live position. A replaced
 *  default-percent short (`replaced_percent_short_market_is_live`) is
 *  dispatched `close_opposite_then_enter` with its `frozen_default_qty`:
 *  @p qty is that transaction, @p close_only 0. The probe answers for the
 *  order filling against the CURRENT book and position; fills that an
 *  earlier order in the same pass would make first are not simulated.
 *  NOT folded into @p qty: the deferred-flip
 *  carry (`tv_carry_qty`, added by `enter_market_from_flat` for a priced
 *  entry firing from FLAT whose placement side is the opposite of the
 *  requested side) -- read `tv_carry_qty` / `created_position_side` from
 *  the mirror. Returns 0 on success; 1 -- with @p qty NaN, @p close_only 0,
 *  @p partition -1 -- when the order is an EXIT (its fill quantity is
 *  decided against the live position at the fill, not by a partition); -1
 *  with nothing written when @p s is NULL, @p index is out of range, or any
 *  out-pointer is NULL. Read-only: no historical run changes because a
 *  caller probed it. */
PF_API int strategy_pending_order_fill_qty(pf_strategy_t s, int index, double fill_price,
                                           double* qty, int* close_only, int* partition);
/** 1 when the @p index-th resting order's entry-relative offsets
 *  (`profit_ticks` / `loss_ticks` / `trail_points`) resolve now (ABI v4,
 *  task 8): entries, plain orders and exits with an empty `from_entry`
 *  always; an exit bound to a `from_entry` only once that id has filled in
 *  the CURRENT position cycle -- the gate the engine's own
 *  `materialize_relative_exit_prices_for_live_position` and eligibility
 *  pass share. 0 otherwise; -1 when @p s is NULL or @p index is out of
 *  range. */
PF_API int strategy_pending_order_level_resolved(pf_strategy_t s, int index);
/** The price levels the @p index-th resting order would fire at, as the
 *  engine's fill path resolves them (ABI v4, task 8). A leg the order
 *  carries as a price (`stop_price`, `limit_price`, `trail_price`) is
 *  reported verbatim -- it is already on the price grid. A leg carried as
 *  a tick offset is resolved against the live position's average entry
 *  price only when #strategy_pending_order_level_resolved is 1 AND a
 *  position is live, with the POSITION side's sign exactly as the fill
 *  path: `limit = entry + dir * profit_ticks * mintick`, `stop = entry -
 *  dir * loss_ticks * mintick` (dir = +1 long, -1 short; both
 *  `level_on_price_grid`), and `trail_activation = entry +/- ceil(
 *  trail_points - 5e-5) * mintick` snapped to the tick grid
 *  (`trail_points` wins over `trail_price` when both are set, as in
 *  `resolve_exit_path_fill`). NaN for a leg that is unset or not yet
 *  resolvable. Returns 0; -1 with nothing written when @p s is NULL,
 *  @p index is out of range, or any out-pointer is NULL. */
PF_API int strategy_pending_order_effective_levels(pf_strategy_t s, int index, double* stop,
                                                   double* limit, double* trail_activation);
/** The trail extreme the exit trail legs ride (`trail_best_price_`: the
 *  running high of a long / low of a short since the position filled,
 *  bar extremes folded in as the fill path folds them). NaN when @p s is
 *  NULL and NaN until a position has filled. */
PF_API double strategy_trail_best_price(pf_strategy_t s);
/** The live position's volume-weighted average entry price
 *  (`position_entry_price_`). NaN when @p s is NULL and NaN when the
 *  position is flat (the engine keeps 0 there; a live reader must not
 *  mistake it for a price). */
PF_API double strategy_position_avg_price(pf_strategy_t s);
/** The live position's cycle id (`position_cycle_seq_`): 0 when flat, a
 *  fresh nonzero id per open or reversal, unchanged across same-direction
 *  adds -- the id `created_position_cycle_seq` on a mirrored order refers
 *  to. -1 when @p s is NULL. */
PF_API int64_t strategy_position_cycle_seq(pf_strategy_t s);
/** Task 9: closed-trade id / exit-comment string accessors, indexing the
 *  same REPORT row space as #strategy_closed_trade_entry_incarnation
 *  (`trades_` then the range-end rows, `open_at_end`). The returned pointer
 *  is valid until the next run() (or stream call) on this handle, like
 *  #strategy_get_last_error. NULL on a NULL @p s or an out-of-range
 *  @p trade_index. */
PF_API const char* strategy_closed_trade_entry_id(pf_strategy_t s, int trade_index);
/** See #strategy_closed_trade_entry_id for the row-space/lifetime/NULL
 *  contract. The returned string is the engine's own INTERNAL exit id, not
 *  always the script's `strategy.exit`/`strategy.close` id verbatim:
 *    - a real `strategy.exit` bracket leg -- the user's own id, unchanged.
 *    - a `strategy.close(id, ...)` close -- `"__close__" + id`.
 *    - `strategy.close_all()` / a bare-id `strategy.close()` -- the literal
 *      `"__close__"` (empty target id appended).
 *    - a margin-call forced liquidation -- the sentinel `"__margin_call__"`.
 *    - an intraday-cap close (`risk.max_intraday_loss`, or the
 *      max-filled-orders cap) -- empty (`""`).
 *  A caller matching exit ids back to its own `strategy.close` calls should
 *  strip the `"__close__"` prefix rather than compare verbatim. */
PF_API const char* strategy_closed_trade_exit_id(pf_strategy_t s, int trade_index);
/** See #strategy_closed_trade_entry_id. */
PF_API const char* strategy_closed_trade_exit_comment(pf_strategy_t s, int trade_index);
/** Task 9: why the @p trade_index-th REPORT-row closed trade exited.
 *  Values:
 *    - `0` UNKNOWN -- reserved for the documented "no cause" value on a
 *      VALID trade. Every in-range row currently falls through the
 *      derivation below to at worst `1` SCRIPT, so no live derivation
 *      today actually returns `0`; it is not used for a bad index (see
 *      `-1` below, final review F7).
 *    - `1` SCRIPT -- a `strategy.close` / `strategy.close_all` market close,
 *      or a reversal-driven close.
 *    - `2` BRACKET -- a `strategy.exit` stop/limit/trail/profit/loss leg.
 *    - `3` MARGIN_CALL -- a forced liquidation slice.
 *    - `4` INTRADAY_LOSS_CAP -- `risk.max_intraday_loss`.
 *    - `5` INTRADAY_FILL_CAP -- the max-filled-orders intraday cap.
 *    - `6` RANGE_END -- the still-open position closed at the end of a
 *      flag-off run (`open_at_end`, ABI v3) -- this always wins over every
 *      other cause below it.
 *  Derivation order (see `BacktestEngine::closed_trade_close_cause`,
 *  engine_trade_accessors.cpp): `open_at_end` -> 6; `exit_id ==
 *  "__margin_call__"` -> 3; an empty `exit_id` with `exit_comment` starting
 *  `"Close Position (Max number of filled orders"` -> 5, or `"Close
 *  Position (Max intraday Loss)"` -> 4; the row's `exit_from_bracket` flag
 *  -- true only for a REAL `strategy.exit` leg, either an `OrderType::EXIT`
 *  fill whose id does NOT carry the internal `"__close__"` prefix that a
 *  deferred `strategy.close`/`close_all` order is also given (that path
 *  reuses the same `OrderType::EXIT` fill machinery), or a whole-position
 *  bracket revived and fired at the margin-call event price
 *  (`revive_position_brackets_after_margin_call_partial`) -- -> 2;
 *  otherwise 1.
 *  `-1` when @p s is NULL, or when @p trade_index is out of range (final
 *  review F7: matches every sibling indexed live accessor's -1-on-bad-index
 *  convention -- #strategy_pending_order_fill_qty,
 *  #strategy_pending_order_level_resolved,
 *  #strategy_pending_order_effective_levels). `0` therefore never
 *  ambiguously means "bad index"; a caller can tell "row exists but reads
 *  UNKNOWN" apart from "bad index" without a separate
 *  #strategy_closed_trade_entry_incarnation `report_trade_count` bounds
 *  check first, though doing that check is still good practice. */
PF_API int strategy_closed_trade_close_cause(pf_strategy_t s, int trade_index);
/** Task 9: the script-facing signed position size (`strategy.position_size`;
 *  KI-64 freeze-aware -- while a same-bar `process_orders_on_close` close is
 *  frozen for the current bar, this reads the PRE-close position, matching
 *  what the script itself observes). NaN when @p s is NULL. */
PF_API double strategy_position_size(pf_strategy_t s);
/** Task 9: initial capital plus realized net profit
 *  (`strategy.initial_capital + strategy.netprofit`). NOT Pine's
 *  `strategy.equity`, which adds open profit on top of this (unlike the
 *  last point of `pf_report_t::equity_curve`, which does). NaN when @p s is
 *  NULL. */
PF_API double strategy_current_equity(pf_strategy_t s);
/** Task 9: total SCRIPT bars dispatched by the most recent run() (mirrors
 *  `pf_report_t::script_bars_processed`, engine_report.cpp) -- includes a
 *  stream's warmup leg and every realtime tick-driven bar dispatched
 *  afterward by #strategy_stream_push_tick / #strategy_stream_push_ticks.
 *  `-1` when @p s is NULL. */
PF_API int64_t strategy_script_bars_processed(pf_strategy_t s);
/** @} */

/** @addtogroup pf_config
 *  @{
 */

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

/** Plumb the symbol's exchange timezone (IANA string) into syminfo. Feeds
 *  ``session.ismarket`` / ``time(session)`` predicates. Defaults to "UTC"
 *  (crypto). Distinct from #strategy_set_chart_timezone — the chart TZ
 *  drives wall-clock builtins and intraday-cap day rollover; this drives
 *  session membership. `NULL` is ignored. Call before #run_backtest*. */
PF_API void strategy_set_syminfo_timezone(pf_strategy_t s, const char* tz);

/** Set the symbol's session string (e.g. "0930-1600:23456", default
 *  "24x7"). Feeds ``session.ismarket`` / ``time(session)``. `NULL`
 *  ignored. Call before #run_backtest*. */
PF_API void strategy_set_syminfo_session(pf_strategy_t s, const char* session);

/** Set the instrument class (``syminfo.type``: "forex", "stock", "crypto",
 *  "futures", "index", "fund", "cfd", ...; default "crypto"). Scripts branch
 *  on it for instrument conventions (e.g. the forex pip size). `NULL` /
 *  empty ignored. Call before #run_backtest*. */
PF_API void strategy_set_syminfo_type(pf_strategy_t s, const char* type);

/** Set one of the remaining string ``syminfo.*`` members by Pine member
 *  name: "ticker", "tickerid", "currency", "basecurrency", "description",
 *  "volumetype" (and "type"). Returns 0 when set, -1 for an unknown key,
 *  empty value or NULL. Call before #run_backtest*. */
PF_API int strategy_set_syminfo_string(pf_strategy_t s, const char* key,
                                       const char* value);

/** Set the instrument tick size (``syminfo.mintick``, default 0.01). Drives the
 *  directional stop-entry snap and ``slippage = N*mintick`` economics. Set
 *  per-instrument (e.g. 0.25 for ES, 0.00001 for FX). Non-positive ignored.
 *  Call before #run_backtest*. */
PF_API void strategy_set_syminfo_mintick(pf_strategy_t s, double mintick);

/** Set the instrument point value (``syminfo.pointvalue``, default 1.0) — the
 *  $-per-point-per-contract multiplier applied to every money path: realized
 *  PnL and MFE/MAE, open profit / mark-to-market equity (and the drawdown /
 *  runup extremes), percent-of-equity and cash position sizing, percent
 *  commission notionals, and the margin admission check. Set per-instrument
 *  (e.g. 50 for ES). Non-positive ignored. Call before #run_backtest*. */
PF_API void strategy_set_syminfo_pointvalue(pf_strategy_t s, double pointvalue);

/** Inject a fundamental/exchange metadata value by Pine member name
 *  (e.g. "shares_outstanding_total", "target_price_average"). These have
 *  no OHLCV source; reads of un-injected members return na. Call before
 *  #run_backtest*. */
PF_API void strategy_set_syminfo_metadata(pf_strategy_t s, const char* key,
                                          double value);

/** Install a timestamped quote-to-account currency conversion curve.
 *
 *  Each value is account-currency units per one unit of the symbol's quote
 *  currency and becomes active, inclusively, at the corresponding Unix-ms
 *  timestamp. The latest active value carries forward; broker events before
 *  the first point use the scalar `account_currency_fx` metadata fallback.
 *  Installing a curve also selects the converted account-currency broker
 *  ledger, including during that pre-first fallback interval; it is not
 *  equivalent to a same-currency run merely because a rate happens to be 1.
 *  Arrays are copied. Timestamps must be strictly increasing and rates
 *  positive and finite. Pass `n == 0` to clear the curve and restore scalar
 *  behavior. Timestamped curves currently support ordinary historical runs.
 *  Broker-open rate changes on margin-call-enabled carried positions are
 *  TV-pinned for 1x longs; carried shorts and leveraged positions fail closed
 *  at the crossing. Streaming, calc-on-order-fills, and bar-magnifier runs
 *  also fail closed.
 *
 *  @return 0 on success, -1 for a null strategy or invalid arrays. */
PF_API int strategy_set_account_currency_fx_series(
    pf_strategy_t s, const int64_t* effective_from_ms,
    const double* account_per_quote, int n);

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
/** Copy a finer feed used exclusively by same-symbol request.security calls.
 *
 *  The next ordinary #run_backtest_full call must receive native chart bars
 *  with @c input_tf equal to @c script_tf and bar magnifier disabled. Chart
 *  OHLCV, broker fills, and @c bar_index continue to advance only from that
 *  native chart feed; @p bars advance only request.security evaluators.
 *  Every auxiliary bar must map to exactly one native chart bar and every
 *  native chart bar must have at least one auxiliary bar, otherwise the run
 *  fails closed via #strategy_get_last_error. Arrays are copied. Pass
 *  @p n == 0 to clear the auxiliary feed.
 *
 *  @return 0 on success, -1 for a null strategy or invalid input. */
PF_API int strategy_set_aux_security_feed(pf_strategy_t s,
                                          const pf_bar_t* bars,
                                          int n,
                                          const char* input_tf);
#endif

#ifdef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
/** Copy the exchange's OWN bars of one higher timeframe for same-symbol
 *  request.security calls that request exactly that timeframe.
 *
 *  On intraday charts of CME futures and US/Indian equities TradingView's
 *  request.security(syminfo.tickerid, "D", close) returns the exchange's
 *  daily bar -- the 15:00 CT settlement on ES1!/NQ1!, the official closing
 *  print on NASDAQ/NYSE/NSE -- which no aggregation of the intraday feed can
 *  reproduce. With a native feed installed for @p timeframe ("D", "1D", "W",
 *  ...), every completed request.security bucket of that timeframe carries
 *  the native bar's OHLCV (matched by the bucket's own label: the session-day
 *  stamp for D/W/M, the grid open for intraday) instead of the aggregate;
 *  bucket TIMING -- when the bar completes relative to the chart -- and every
 *  other timeframe's request are unchanged, and so are chart OHLCV, broker
 *  fills and bar_index. A "W" / "M" request with no feed of its own reads
 *  the daily feed's bars aggregated per week / month (TradingView builds its
 *  W/M bars from the native daily bars, never from intraday prints), keyed by
 *  the period's first session-day. A completed bucket with no native bar
 *  keeps its aggregate (counted, not fatal). Bars must be strictly
 *  increasing; arrays are copied; pass @p n == 0 to clear the feed for
 *  @p timeframe. Historical runs only: stream_begin() fails closed while a
 *  native feed is installed.
 *
 *  @return 0 on success, -1 for a null strategy or invalid input. */
PF_API int strategy_set_native_security_feed(pf_strategy_t s,
                                             const char* timeframe,
                                             const pf_bar_t* bars,
                                             int n);
#endif

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

/** @return Monotonic ABI version (see #PF_ABI_VERSION). */
PF_API int pf_abi_version(void);

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
