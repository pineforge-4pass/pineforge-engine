/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * O1 report contract v1: data declarations only, no runtime feature probe.
 * See docs/design/o1-report-contract.md. Including this header does not
 * establish provider support for SUMMARY_ONLY or STREAMED.
 * C++ row/point visitors are declared in the sibling report_pages.hpp;
 * they use these modes/metadata and introduce no second mode vocabulary.
 */
#ifndef PINEFORGE_REPORT_MODE_H
#define PINEFORGE_REPORT_MODE_H

#include <pineforge/pineforge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Version of the opt-in report envelope; independent of PF_ABI_VERSION.
 *  The legacy pf_report_t layout and all existing function signatures stay
 *  unchanged. No runtime C export is introduced by this header. */
#define PF_REPORT_ENVELOPE_VERSION 1

/** Fixed-width mode word, including for consumers using short C enums. */
typedef uint32_t pf_report_mode_v1_t;
enum {
    PF_REPORT_MODE_FULL = 0,         /**< Legacy owning report sequences. */
    PF_REPORT_MODE_SUMMARY_ONLY = 1, /**< FULL metric bits without presentation overrides; no captured sequences; refused with an observer, selection window or observed phase and no sink. */
    PF_REPORT_MODE_STREAMED = 2      /**< Complete sequences in an external sink. */
};

/** Setter/read outcomes. Rejection leaves configuration unchanged; invalid
 *  envelope descriptors also leave the destination unchanged. */
enum {
    PF_REPORT_MODE_ACCEPTED = 0,
    PF_REPORT_MODE_INVALID = -1,
    PF_REPORT_MODE_UNSUPPORTED = -2,
    PF_REPORT_MODE_BUSY = -3,
    PF_REPORT_MODE_CONFLICT = -4,
    PF_REPORT_MODE_REPLAY_UNAVAILABLE = -5
};

/** Logical cardinality and explicit capture policy for one sequence.
 *  Both flags are 0 or 1; external implies captured. An enabled, captured
 *  empty sequence has captured=1 and logical_count=0. SUMMARY_ONLY has both
 *  flags 0 even when logical_count is positive. These fields do not certify
 *  run completion. Counts refer to the authoritative pass exactly once. */
typedef struct pf_report_sequence_v1 {
    int64_t logical_count;
    uint32_t captured;
    uint32_t external;
} pf_report_sequence_v1;

/** Fixed-size report metadata. Initialize struct_size and version before a
 *  native envelope read. complete is 1 only after the current attempt and
 *  all required replay/receipt validation succeed.
 *
 *  The seven logical counts are independent of resident array lengths.
 *  Optional disabled sources have count/flags 0. Output rows/events remain
 *  outside pf_report_t; their capture policy is stated here for consumers. */
typedef struct pf_report_metadata_v1 {
    uint32_t struct_size;
    uint32_t version;
    pf_report_mode_v1_t mode;
    uint32_t complete;
    pf_report_sequence_v1 trades;
    pf_report_sequence_v1 equity_curve;
    pf_report_sequence_v1 broker_state_hash;
    pf_report_sequence_v1 trace;
    pf_report_sequence_v1 trace_names;
    pf_report_sequence_v1 output_rows;
    pf_report_sequence_v1 output_events;
} pf_report_metadata_v1;

/** Opt-in envelope, not an extension of the caller-allocated ABI-4 report.
 *  report.total_trades remains the logical total; report.trades_len and
 *  report.equity_curve_len describe resident arrays only. In SUMMARY_ONLY
 *  and STREAMED those arrays are NULL/0. The payload's existing free rules
 *  apply; the envelope itself is caller-owned. */
typedef struct pf_report_envelope_v1 {
    pf_report_metadata_v1 metadata;
    pf_report_t report;
} pf_report_envelope_v1;

PF_STATIC_ASSERT(sizeof(pf_report_mode_v1_t) == 4);
PF_STATIC_ASSERT(sizeof(pf_report_sequence_v1) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sequence_v1, captured) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sequence_v1, external) == 12);
PF_STATIC_ASSERT(sizeof(pf_report_metadata_v1) == 128);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, trades) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, equity_curve) == 32);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, broker_state_hash) == 48);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, trace) == 64);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, trace_names) == 80);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, output_rows) == 96);
PF_STATIC_ASSERT(offsetof(pf_report_metadata_v1, output_events) == 112);
PF_STATIC_ASSERT(offsetof(pf_report_envelope_v1, report) == 128);
PF_STATIC_ASSERT(sizeof(pf_report_envelope_v1) == 128 + sizeof(pf_report_t));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PINEFORGE_REPORT_MODE_H */
