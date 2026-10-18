/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CR-150 P00: report sink C ABI declarations, DESIGN.md revision 6,
 * section 3.7 lines 779-897 and section 6 lines 1255-1294.
 * No definitions or runtime feature probe are supplied by this contract.
 * Include this additive header explicitly, as for execution_observer.h.
 */

#ifndef PINEFORGE_REPORT_SINK_H
#define PINEFORGE_REPORT_SINK_H

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Path sink descriptor (DESIGN 6:1260-1266, 1284-1286).
 *  struct_size is sizeof(pf_report_sink_v1), version is 1. The setter copies
 *  the strings. The engine creates or truncates path, writes the version-1
 *  header at run begin, and writes checked, buffered records (3.7:878-885).
 *  NULL run_id means ""; NULL scratch_dir means the directory of path.
 *  Scratch files are engine-private and live until handle destruction
 *  (3.8:899-906). */
typedef struct pf_report_sink_v1 {
    uint32_t    struct_size;
    uint32_t    version;
    const char* path;
    const char* run_id;
    const char* scratch_dir;
} pf_report_sink_v1;

/** Trailer receipt (DESIGN 6:1268-1283).
 *  The seven counts correspond to E, T, O, V, K, X, H in that order.
 *  sha256_hex is the trailer's lowercase SHA-256, NUL-terminated, and is ""
 *  until the trailer exists. complete becomes 1 once the trailer is written.
 *  trades_retained is 1 when closedtrades.* history also stays in RAM.
 *  The explicit seven-byte reserved field gives a 144-byte layout with no
 *  implicit padding. PF_ABI_VERSION stays 4; no existing report/trade grows.
 *  OPEN S1: receipt query return codes, caller initialization and output on
 *  refusal are not specified by DESIGN; see docs/design/cr150-contracts.md. */
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

/** Returns report sink ABI version 1 (DESIGN 6:1277). */
PF_API uint32_t pf_report_sink_version(void);

/** Install, replace or remove (NULL descriptor) the sink (6:1278, 1284-1286).
 *  Returns 0 accepted; -1 bad struct_size, version, path or handle; -2 a
 *  stream is running or begins; -3 run in progress. Strings are copied.
 *  The sink composes with the execution observer and selected window.
 *  Writes fail the run with report_sink_failed (3.7:878-885).
 *
 *  With a sink, pf_report_t contains summary scalars, total_trades and
 *  metrics; trades, equity_curve, broker_state_hash and trace are NULL with
 *  length 0. Thus total_trades need not equal trades_len, and the equity
 *  and hash lengths need not equal script_bars_processed (6:1289-1294).
 *  Counts and SHA-256 come from the receipt. Without a sink the existing
 *  entry points and report behavior stay unchanged (6:1329-1331). */
PF_API int strategy_set_report_sink_v1(pf_strategy_t, const pf_report_sink_v1*);

/** Query the sink receipt (DESIGN 6:1279; OPEN S1 above).
 *  Failed, refused, aborted or truncated runs have no valid trailer
 *  (3.7:788-790); a receipt without completion is not a valid sink. */
PF_API int strategy_report_sink_receipt_v1(pf_strategy_t, pf_report_sink_receipt_v1*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_REPORT_SINK_H */
