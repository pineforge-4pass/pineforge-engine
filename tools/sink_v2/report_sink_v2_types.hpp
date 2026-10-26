#pragma once

// Tool-private parked layouts; no engine setter, export or ABI extension.
#include <pineforge/report_sink.h>

typedef struct pf_report_sink_v2 {
    uint32_t struct_size;
    uint32_t version;
    const char* path;
    const char* run_id;
    const char* scratch_dir;
    int32_t zstd_level;
    uint32_t flags;
    uint32_t queue_depth;
    uint32_t reserved;
} pf_report_sink_v2;

/** v2 logical receipt. complete and sha256_hex are set only after sealing and
 *  checked close. nonfinite_t_field is the first nullable numeric T column
 *  (1-based) that the run_json adapter would reject; the sink itself allows it.
 *  Queries require initialized struct_size/version and leave invalid outputs
 *  unchanged. A v1 receipt query also works for v2, with its logical digest.
 *  scratch_flags reports detected memory backing (1), unknown backing (2) or
 *  recognized disk backing (0); it is local and absent from logical identity. */
typedef struct pf_report_sink_receipt_v2 {
    uint32_t struct_size, version;
    int64_t count_e, count_t, count_o, count_v, count_k, count_x, count_h;
    int64_t file_bytes;
    int64_t block_count;
    char sha256_hex[65];
    uint8_t reserved[3];
    uint32_t complete;
    uint32_t trades_retained;
    uint32_t threaded;
    int64_t wins, losses;
    double best_trade, worst_trade, max_drawdown;
    uint32_t nonfinite_t_field;
    uint32_t data_complete;
    uint64_t closed_count;
    uint64_t open_at_end_count;
    double open_at_end_pnl;
    uint32_t scratch_flags; /* bit 0 memory filesystem; bit 1 backing unknown */
    uint8_t reserved_summary[12];
} pf_report_sink_receipt_v2;

PF_STATIC_ASSERT(sizeof(pf_report_sink_v2) == 48);
PF_STATIC_ASSERT(offsetof(pf_report_sink_v2, zstd_level) == 32);
PF_STATIC_ASSERT(sizeof(pf_report_sink_receipt_v2) == 248);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, file_bytes) == 64);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, block_count) == 72);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, sha256_hex) == 80);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, complete) == 148);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, threaded) == 156);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, wins) == 160);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, best_trade) == 176);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, nonfinite_t_field) == 200);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, data_complete) == 204);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, closed_count) == 208);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, open_at_end_count) == 216);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, open_at_end_pnl) == 224);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, scratch_flags) == 232);
PF_STATIC_ASSERT(offsetof(pf_report_sink_receipt_v2, reserved_summary) == 236);
