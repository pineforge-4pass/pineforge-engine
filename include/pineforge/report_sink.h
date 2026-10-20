/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CR-150 P00: report sink C ABI declarations, DESIGN.md revision 6,
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
 *  Scratch files are engine-private and live until handle destruction
 *  (3.8:902-909). */
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
 *  library never changes process-wide signal dispositions. */
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
 *  trades_retained is 1 when closedtrades.* history also stays in RAM.
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

/** Returns report sink ABI version 1 (DESIGN 6:1300). */
PF_API uint32_t pf_report_sink_version(void);

/** Install, replace or remove (NULL descriptor) the sink (6:1301, 1307-1309).
 *  Returns 0 accepted; -1 bad struct_size, version, path or handle; -2 a
 *  stream is running or begins; -3 run in progress. Strings are copied.
 *  The sink composes with the execution observer and selected window.
 *  Write errors fail the run with the sink I/O code planned for
 *  docker/run_failure_codes.json (DESIGN 3.7:881-888; 6:1330-1332).
 *
 *  With a sink, pf_report_t contains summary scalars, total_trades and
 *  metrics; trades, equity_curve, broker_state_hash and trace are NULL with
 *  length 0. Thus total_trades need not equal trades_len, and the equity
 *  and hash lengths need not equal script_bars_processed (6:1312-1317).
 *  Counts and SHA-256 come from the receipt. Without a sink the existing
 *  entry points and report behavior stay unchanged (6:1352-1354). */
PF_API int strategy_set_report_sink_v1(pf_strategy_t, const pf_report_sink_v1*);

/** Install, replace or remove (NULL) the single sink using a borrowed fd.
 *  Mutually exclusive with strategy_set_report_sink_v1: either setter
 *  replaces the other kind, and NULL through either setter removes it.
 *  Returns 0 accepted; -1 bad handle, struct_size, version or negative fd;
 *  -2 stream running/beginning; -3 run in progress, as the path setter.
 *  Descriptor I/O/type validation occurs at run begin; a closed fd or a
 *  non-stream descriptor fails the run with report_sink_failed. Summary
 *  reports and strategy_report_sink_receipt_v1 follow the path sink rules. */
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_REPORT_SINK_H */
