/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Report sink C ABI declarations, DESIGN.md revision 6,
 * section 3.7 lines 782-901 and section 6 lines 1278-1317.
 * No definitions or runtime feature probe are supplied by this contract.
 * Include this additive header explicitly, as for execution_observer.h.
 */

#ifndef PINEFORGE_REPORT_SINK_H
#define PINEFORGE_REPORT_SINK_H

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Path sink descriptor (DESIGN 6:1283-1289, 1307-1309).
 *  struct_size is sizeof(pf_report_sink_v1), version is 1. The setter copies
 *  the strings. The engine creates or truncates path, writes the version-1
 *  header at run begin, and writes checked, buffered records (3.7:881-888).
 *  NULL run_id means ""; NULL scratch_dir means the directory of path.
 *  Scratch files are engine-private and live until the next run begins or
 *  handle destruction (3.8:902-909). */
typedef struct pf_report_sink_v1 {
    uint32_t    struct_size;
    uint32_t    version;
    const char* path;
    const char* run_id;
    const char* scratch_dir;
} pf_report_sink_v1;

/** Borrowed sequential stream sink descriptor (CX-SINK-B1/B2).
 *  struct_size is sizeof(pf_report_sink_fd_v1), version is 1. fd is the
 *  caller-selected write end of a FIFO or AF_UNIX SOCK_STREAM socket; there
 *  is no library default fd number. NULL run_id means ""; it is copied.
 *  No path or scratch directory is supplied or derived for this sink.
 *
 *  The caller keeps fd open through the run and report capture. The library
 *  neither duplicates nor closes it, including on replacement, removal,
 *  failure, abort or handle destruction. The caller closes it to deliver EOF
 *  to the trusted finalizer. A new run on the same fd appends a new stream;
 *  use a fresh stream per run when the reader expects one header/trailer.
 *  Each begin checks that fd is open and matches its installed device/inode
 *  identity; a closed or replaced descriptor fails with report_sink_failed.
 *  A host that dup2s a fresh FIFO onto the same descriptor number for the
 *  next run must install the sink again.
 *
 *  Bytes, canonical JSON, hashes, LF framing and the 1 MiB line limit are
 *  identical to the path sink. Writes are strictly sequential: no open,
 *  creation, seek, pwrite, truncation, unlink or rename on this route.
 *  Partial writes and EINTR are retried; EAGAIN blocks in poll() with bounded
 *  buffering. Only a successful run writes a trailer, as its last line.
 *  Failure or abort leaves no valid trailer; the receipt stays incomplete.
 *
 *  Socket writes use send(..., MSG_NOSIGNAL). For a FIFO the host MUST ignore
 *  SIGPIPE (Python does by default; C hosts use signal(SIGPIPE, SIG_IGN))
 *  so a closed reader becomes the typed report_sink_failed error. The
 *  library never changes process-wide SIGPIPE disposition. A successful new inherited-arena attach sets SIGXFSZ to SIG_IGN process-wide; detach does not restore it. Hosts relying on their SIGXFSZ handler or default termination must account for this before attaching an arena. */
typedef struct pf_report_sink_fd_v1 {
    uint32_t    struct_size;
    uint32_t    version;
    int         fd;
    const char* run_id;
} pf_report_sink_fd_v1;

/** Trailer receipt (DESIGN 6:1291-1298,1305-1306).
 *  The seven counts correspond to E, T, O, V, K, X, H in that order.
 *  sha256_hex is the trailer's lowercase SHA-256, NUL-terminated, and is ""
 *  until the trailer exists. complete becomes 1 once the trailer is written.
 *  trades_retained is 1 when closedtrades.* history stays script-readable.
 *  The explicit seven-byte reserved field gives a 144-byte layout with no
 *  implicit padding. PF_ABI_VERSION stays 4; no existing report/trade grows.
 *  RESOLVED S1: receipt query initialization and outcomes follow the
 *  CORE-TOP rev 6 amendment (10-18 16:04); see the query declaration below. */
typedef struct pf_report_sink_receipt_v1 {
    uint32_t struct_size, version;
    int64_t  count_e, count_t, count_o, count_v, count_k, count_x, count_h;
    char     sha256_hex[65];
    uint8_t  reserved[7];
    uint32_t complete;
    uint32_t trades_retained;
} pf_report_sink_receipt_v1;

PF_STATIC_ASSERT(sizeof(pf_report_sink_receipt_v1) == 144);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v1, count_e) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v1, sha256_hex) == 64);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v1, reserved) == 129);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v1, complete) == 136);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v1, trades_retained) == 140);

/** Report-only T-row summary (CX-SINK-B5a), independent of the 144-byte
 *  trailer receipt above. Size 64, alignment 8, with no implicit padding:
 *  struct_size 0, version 4, count_t 8, wins 16, losses 24, best_trade 32,
 *  worst_trade 40, max_drawdown 48, nonfinite_t_field 56, complete 60.
 *
 *  Rows enter in global T order, using the original report doubles. Wins
 *  and losses count strictly positive/negative P&L; either zero counts as
 *  neither. Best/worst start with the first P&L and retain the previous
 *  operand on ties (including signed zero). Empty extrema are +0.0.
 *  Drawdown is the nonpositive minimum of cumulative trade P&L minus its
 *  running peak, both starting at +0.0; it is not equity-curve drawdown.
 *
 *  nonfinite_t_field is 0 if all eight canonical doubles are finite.
 *  Otherwise it names the first field of the first offending T row:
 *  5 entry_price, 6 exit_price, 7 qty, 8 pnl, 9 pnl_pct, 10 max_runup,
 *  11 max_drawdown, 12 commission. These are T wire field indices.
 *  complete mirrors the trailer receipt. Values are published only with
 *  a successful trailer; until then they are zero and complete is 0. */
typedef struct pf_report_sink_summary_v1 {
#if defined(__cplusplus)
    alignas(8)
#elif defined(_MSC_VER)
    __declspec(align(8))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Alignas(8)
#elif defined(__GNUC__)
    __attribute__((aligned(8)))
#endif
    uint32_t struct_size;
    uint32_t version;
    int64_t  count_t;
    int64_t  wins;
    int64_t  losses;
    double   best_trade;
    double   worst_trade;
    double   max_drawdown;
    int32_t  nonfinite_t_field;
    uint32_t complete;
} pf_report_sink_summary_v1;

PF_STATIC_ASSERT(sizeof(pf_report_sink_summary_v1) == 64);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, struct_size) == 0);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, version) == 4);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, count_t) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, wins) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, losses) == 24);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, best_trade) == 32);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, worst_trade) == 40);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, max_drawdown) == 48);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, nonfinite_t_field) == 56);
PF_STATIC_ASSERT(offsetof(pf_report_sink_summary_v1, complete) == 60);
#if defined(__cplusplus)
PF_STATIC_ASSERT(alignof(pf_report_sink_summary_v1) == 8);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
PF_STATIC_ASSERT(_Alignof(pf_report_sink_summary_v1) == 8);
#endif

/** Fixed-size annualization provenance from the emitted v1 records.
 *  Independent of both existing receipts; no wire or legacy layout changes.
 *  first/last are the first/last presented E rows, not input-feed endpoints.
 *  trade_at_start is 1 iff any emitted T row has entry_bar_index <= 0.
 *  Published only after a successful trailer. No sink, failure, abort or
 *  an unstarted generation yields all-zero values and complete 0.
 *  Read through strategy_report_sink_diagnostics_v1 (pineforge.h). */
typedef struct pf_report_sink_diagnostics_v1 {
    uint32_t struct_size;
    uint32_t version;
    int64_t count_e;
    pf_equity_point_t first;
    pf_equity_point_t last;
    uint32_t trade_at_start;
    uint32_t complete;
} pf_report_sink_diagnostics_v1;

PF_STATIC_ASSERT(sizeof(pf_report_sink_diagnostics_v1) == 72);
PF_STATIC_ASSERT(offsetof(pf_report_sink_diagnostics_v1, count_e) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_diagnostics_v1, first) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sink_diagnostics_v1, last) == 40);
PF_STATIC_ASSERT(offsetof(pf_report_sink_diagnostics_v1, trade_at_start) == 64);
PF_STATIC_ASSERT(offsetof(pf_report_sink_diagnostics_v1, complete) == 68);

/** Returns report sink ABI version 1 (DESIGN 6:1300). */
PF_API uint32_t pf_report_sink_version(void);

/** Install, replace or remove (NULL descriptor) the sink (6:1301, 1307-1309).
 *  Returns 0 accepted; -1 bad struct_size, version, path or handle, or invalid environment arena;
 *  -2 stream running/beginning; -3 run in progress or removal while an
 *  ordinary run's scratch curve is live. Strings are copied.
 *  A refused removal keeps the sink and its summary report unchanged;
 *  it never restores a full curve array from scratch.
 *  Sealed trade rows (ledger base > 0) also refuse removal with -3 and
 *  set_trade_history_streamable(false) with coded logic_error engine_invariant,
 *  preserving the sink, receipt and report without rehydrating rows until a
 *  new run begin clears size/base to 0 (ordinary scratch ownership still
 *  independently refuses removal).
 *  Completed fd generations allow NULL through either setter to detach the
 *  next-run sink while preserving their receipt, summary and borrowed fd.
 *  The sink composes with the execution observer and selected window. Write errors
 *  fail the run with the sink I/O code planned for docker/run_failure_codes.json
 *  (DESIGN 3.7:881-888; 6:1330-1332). With a sink, pf_report_t contains summary
 *  scalars, total_trades and metrics; trades, equity_curve, broker_state_hash and
 *  trace are NULL with length 0. Thus total_trades need not equal trades_len, and
 *  equity/hash lengths need not equal script_bars_processed (6:1312-1317). Counts and SHA-256 come from the receipt.
 *  Without a sink the existing entry points and report behavior stay unchanged (6:1352-1354).
 *  A non-NULL descriptor attaches the environment arena if configured: PF_SCRATCH_ARENA_FD=5 and positive PF_SCRATCH_ARENA_BYTES, with validation passing.
 *  The first successful attach sets SIGXFSZ to SIG_IGN process-wide; the prior disposition is not restored. Same fd/size reattaches are no-ops.
 *  Hosts relying on their SIGXFSZ handler or default termination must account for this before attaching. */
PF_API int strategy_set_report_sink_v1(pf_strategy_t, const pf_report_sink_v1*);

/** Install, replace or remove (NULL) the single sink using a borrowed fd.
 *  Mutually exclusive with strategy_set_report_sink_v1: either setter
 *  replaces the other kind. NULL through either setter removes next-run
 *  configuration only when the generation guards permit it.
 *  Returns 0 accepted; -1 bad handle, struct_size, version, negative fd or invalid environment arena;
 *  -2 stream running/beginning; -3 run in progress or removal of a
 *  scratch-backed or sealed-base generation, as the path setter.
 *  A successfully completed fd generation permits NULL through either setter
 *  to detach next-run configuration while preserving its receipt, summary
 *  and borrowed fd. Path generations have no completed-generation detach:
 *  their scratch/base guards still apply. A refusal preserves the generation.
 *  Descriptor I/O/type and installed device/inode identity are checked at
 *  each run begin; a closed, replaced or non-stream descriptor fails the run
 *  with report_sink_failed. A host that dup2s a fresh FIFO onto the same
 *  descriptor number for the next run must install the sink again. Summary
 *  reports and strategy_report_sink_receipt_v1 follow the path sink rules.
 *  A non-NULL descriptor attaches the environment arena if configured: PF_SCRATCH_ARENA_FD=5 and positive PF_SCRATCH_ARENA_BYTES, with validation passing.
 *  The first successful attach sets SIGXFSZ to SIG_IGN process-wide; the prior disposition is not restored. Same fd/size reattaches are no-ops.
 *  Hosts relying on their SIGXFSZ handler or default termination must account for this before attaching. */
PF_API int strategy_set_report_sink_fd_v1(pf_strategy_t, const pf_report_sink_fd_v1*);

/** Query the sink receipt (DESIGN 6:1302; RESOLVED S1).
 *  CORE-TOP's rev 6 amendment (10-18 16:04) uses the observation query
 *  strategy_execution_observation_v1 in execution_observer.h as its model.
 *  The caller first sets out->struct_size and out->version.
 *
 *  Returns 0 with the receipt filled; -1 with *out unchanged for a bad
 *  handle, struct_size or version. With no sink installed, returns 0 with
 *  all counts 0, sha256_hex "", complete 0.
 *
 *  Failed, refused, aborted or truncated runs have no valid trailer
 *  (3.7:791-793); a receipt without completion is not a valid sink. */
PF_API int strategy_report_sink_receipt_v1(pf_strategy_t, pf_report_sink_receipt_v1*);

/** Query the T-row summary for either the path or fd sink.
 *  Initialize out->struct_size to sizeof(*out), out->version to 1.
 *  Returns 0 filled; -1 with *out unchanged for NULL out, bad handle,
 *  struct_size or version, exactly as strategy_report_sink_receipt_v1.
 *  No sink, an unstarted run, failure or abort yields zero values and
 *  complete 0. After successful report capture, count_t equals the trailer's
 *  count_t and complete is 1. A nonzero nonfinite_t_field must be rejected
 *  by consumers as report_result_invalid{field:"trades.<name>",
 *  reason:"nonfinite"} before using the summary.
 *  Queries never capture a report, read a sink, or advance the fold; repeated
 *  reads return the same value. Replacement/removal and new runs reset it. */
PF_API int strategy_report_sink_summary_v1(pf_strategy_t, pf_report_sink_summary_v1*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_REPORT_SINK_H */
