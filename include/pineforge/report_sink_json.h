/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CR-397 bulk report transport, descriptor freeze v1. This is separate from
 * pf_report_t and the existing sink receipt, summary and diagnostics.
 */
#ifndef PINEFORGE_REPORT_SINK_JSON_H
#define PINEFORGE_REPORT_SINK_JSON_H

#include <pineforge/report_sink.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PF_REPORT_SINK_JSON_VERSION 1u
#define PF_REPORT_SINK_JSON_LINE_BYTES 1048576u
#define PF_REPORT_SINK_JSON_BUFFER_BYTES 1048577u
#define PF_REPORT_SINK_JSON_WINDOW_BYTES 65536u

/* Words, rather than C enums, keep the ABI independent of -fshort-enums. */
#define PF_SINK_JSON_PREPARE 1u
#define PF_SINK_JSON_GUARD 2u

#define PF_SINK_JSON_OK 0u
#define PF_SINK_JSON_NOT_IMPLEMENTED 1u
#define PF_SINK_JSON_SOURCE_INVALID 2u
#define PF_SINK_JSON_SOURCE_IO 3u
#define PF_SINK_JSON_IDENTITY_DIFFERS 4u
#define PF_SINK_JSON_DIGEST_DIFFERS 5u
#define PF_SINK_JSON_COUNT_DIFFERS 6u
#define PF_SINK_JSON_SCRATCH_IO 7u
#define PF_SINK_JSON_RESOURCE 8u
#define PF_SINK_JSON_INTERNAL 9u

#define PF_SINK_JSON_PREPARE_HEADER 1u
#define PF_SINK_JSON_PREPARE_RECORD 2u
#define PF_SINK_JSON_PREPARE_TRAILER 3u
#define PF_SINK_JSON_GUARD_IDENTITY 4u
#define PF_SINK_JSON_GUARD_HEADER 5u
#define PF_SINK_JSON_GUARD_RECORD 6u
#define PF_SINK_JSON_GUARD_TRAILER 7u
#define PF_SINK_JSON_GUARD_DIGEST 8u
#define PF_SINK_JSON_FRAGMENT 9u

#define PF_SINK_JSON_LINE 1u
#define PF_SINK_JSON_HEADER 2u
#define PF_SINK_JSON_TRAILER 3u
#define PF_SINK_JSON_FRAMING 4u
#define PF_SINK_JSON_READ 5u
#define PF_SINK_JSON_WRITE 6u
#define PF_SINK_JSON_IDENTITY 7u
#define PF_SINK_JSON_DIGEST 8u
#define PF_SINK_JSON_COUNT 9u
#define PF_SINK_JSON_UNSUPPORTED 10u

/** No pointers, no owning storage. The prefix is diagnostic only: the complete
 *  observed offending line is in request.error_buffer[0:result.error_bytes].
 *  Never reopen a mutable source to reconstruct a transient parse refusal.
 *  line_number is one-based; zero means no line was read. field_index is a
 *  one-based wire field excluding the tag, zero when inapplicable. */
typedef struct pf_report_sink_json_error_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t stage;
    uint32_t category;
    uint64_t line_number;
    uint64_t byte_offset;
    uint32_t field_index;
    uint32_t tag;
    int32_t os_errno;
    uint32_t offending_bytes;
    char message[192];
    uint8_t offending[256];
} pf_report_sink_json_error_v1;

/** Initialize size/version and zero all reserved words/bytes.
 *
 *  All fds are borrowed regular files, never closed, with offsets unchanged
 *  (pread/pwrite). -1 omits an output. Outputs must be empty, writable,
 *  distinct files, not aliases of the input. prepare emits comma-separated
 *  array bodies, without brackets/LF; fragments are provisional until complete.
 *  guard has no fragment outputs; guard_tag is ASCII E or T. The host opens the CURRENT
 *  source path afresh for EACH guard, passing the verified inode/size, digest,
 *  seven counts in ETOVKXH order, and raw verified header including LF.
 *
 *  Both operations require header_buffer/header_capacity and
 *  error_buffer/error_capacity >= BUFFER_BYTES. For guard, header_buffer
 *  receives the fresh raw header: the host MUST apply the pinned
 *  header-JSON parser before interpreting later-stage errors/digest/count,
 *  preserving priority even for shapes outside the native JSON subset.
 *  Identity and raw header framing refusals precede that parse. Buffers,
 *  request, result, and guard's verified_header must not overlap. These fixed
 *  capacities capture the reference reader's MAX_LINE_BYTES+1 overflow probe.
 *  No pointer is retained. flags/reserved are zero in v1.
 *
 *  prepare validates E/T/O/V/K/X/H, projects E and the first 16 T fields,
 *  and folds trade summary / equity endpoints. An unsupported parser shape
 *  (JSON depth/key-table capacity) returns NOT_IMPLEMENTED,
 *  never a successful partial verification. The host discards fragments and
 *  uses its strict reader. Guard preserves captured evidence rather than
 *  reopening for fallback. This is not an envelope-mode provider or default
 *  route switch. The host currently requires an explicit opt-in pending
 *  executed refusal, byte, memory and performance verification. */
typedef struct pf_report_sink_json_request_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t operation;
    uint32_t flags;
    int32_t sink_fd;
    int32_t trades_fd;
    int32_t equity_fd;
    uint32_t guard_tag;
    uint64_t expected_ino;
    uint64_t expected_size;
    int64_t expected_counts[7];
    char expected_sha256_hex[65];
    uint8_t reserved[7];
    const char* verified_header;
    uint32_t verified_header_bytes;
    uint32_t header_capacity;
    char* header_buffer;
    uint8_t* error_buffer;
    uint32_t error_capacity;
    uint32_t reserved_word;
} pf_report_sink_json_request_v1;

/** Initialize size/version. Complete means strict source/trailer acceptance
 *  for prepare, or exhaustion of the selected raw scan for guard. A deferred
 *  result error or fragment I/O error does not replace an earlier source
 *  refusal. result_tag/result_field identify the FIRST deferred required-result
 *  failure: E fields 2/3, or T field 1 for report numbering, 5..12 for required
 *  finite values, 21 for cumulative-PnL/drawdown overflow. T failures outrank
 *  an earlier E failure; within each fold the first failure wins. Zero means
 *  none. The host preserves source, run-ID,
 *  engine-receipt, trade, small-result, E-result, then staging priority.
 *  summary.count_t is the full verified T count; remaining fold values are
 *  usable only without a T result failure. net/average use the engine report
 *  and Python round remains host-owned. Existing 64/72-byte shapes are
 *  embedded unchanged; no new execution state or execution-hash leaves. */
typedef struct pf_report_sink_json_result_v1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t status;
    uint32_t complete;
    int64_t counts[7];
    uint64_t source_ino;
    uint64_t source_size;
    char sha256_hex[65];
    uint8_t reserved[7];
    pf_report_sink_summary_v1 summary;
    pf_report_sink_diagnostics_v1 diagnostics;
    uint64_t equity_json_bytes;
    uint64_t trades_json_bytes;
    char equity_json_sha256_hex[65];
    char trades_json_sha256_hex[65];
    uint8_t reserved_fragments[6];
    uint32_t header_bytes;
    uint32_t error_bytes;
    uint32_t result_field;
    uint32_t result_tag;
    pf_report_sink_json_error_v1 error;
} pf_report_sink_json_result_v1;

PF_STATIC_ASSERT(sizeof(pf_report_sink_json_error_v1) == 496);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, struct_size) == 0);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, version) == 4);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, stage) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, category) == 12);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, line_number) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, byte_offset) == 24);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, field_index) == 32);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, tag) == 36);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, os_errno) == 40);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, offending_bytes) == 44);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, message) == 48);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_error_v1, offending) == 240);
/* This descriptor, like the supported runtime, uses the 64-bit pointer ABI. */
PF_STATIC_ASSERT(sizeof(void*) == 8);
PF_STATIC_ASSERT(sizeof(pf_report_sink_json_request_v1) == 216);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, struct_size) == 0);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, version) == 4);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, operation) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, flags) == 12);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, sink_fd) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, trades_fd) == 20);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, equity_fd) == 24);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, guard_tag) == 28);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, expected_ino) == 32);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, expected_size) == 40);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, expected_counts) == 48);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, expected_sha256_hex) == 104);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, reserved) == 169);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, verified_header) == 176);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, verified_header_bytes) == 184);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, header_capacity) == 188);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, header_buffer) == 192);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, error_buffer) == 200);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, error_capacity) == 208);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_request_v1, reserved_word) == 212);
PF_STATIC_ASSERT(sizeof(pf_report_sink_json_result_v1) == 960);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, struct_size) == 0);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, version) == 4);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, status) == 8);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, complete) == 12);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, counts) == 16);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, source_ino) == 72);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, source_size) == 80);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, sha256_hex) == 88);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, reserved) == 153);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, summary) == 160);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, diagnostics) == 224);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, equity_json_bytes) == 296);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, trades_json_bytes) == 304);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, equity_json_sha256_hex) == 312);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, trades_json_sha256_hex) == 377);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, reserved_fragments) == 442);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, header_bytes) == 448);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, error_bytes) == 452);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, result_field) == 456);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, result_tag) == 460);
PF_STATIC_ASSERT(offsetof(pf_report_sink_json_result_v1, error) == 464);
#if defined(__cplusplus)
PF_STATIC_ASSERT(alignof(pf_report_sink_json_request_v1) == 8);
PF_STATIC_ASSERT(alignof(pf_report_sink_json_result_v1) == 8);
PF_STATIC_ASSERT(alignof(pf_report_sink_json_error_v1) == 8);
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
PF_STATIC_ASSERT(_Alignof(pf_report_sink_json_request_v1) == 8);
PF_STATIC_ASSERT(_Alignof(pf_report_sink_json_result_v1) == 8);
PF_STATIC_ASSERT(_Alignof(pf_report_sink_json_error_v1) == 8);
#endif

#ifdef __cplusplus
}
#endif
#endif
