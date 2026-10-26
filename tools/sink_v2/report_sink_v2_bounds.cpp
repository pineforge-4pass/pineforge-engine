#include "report_sink_v2_bounds.hpp"
#include "report_sink_v2_codec.hpp"
#include "report_sink_v2_support.hpp"
#include <algorithm>
#include <limits>

namespace pineforge { namespace report_sink {
namespace {
constexpr std::uint64_t safe = UINT64_C(9007199254740991);
[[noreturn]] void invalid() {
    throw BlockError(BlockFailure::invalid_input, "uncertified or overflowing v2 admission bound");
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (a > safe || b > safe - a) invalid();
    return a + b;
}
std::uint64_t mul(std::uint64_t a, std::uint64_t b) {
    if (a > safe || (a && b > safe / a)) invalid();
    return a * b;
}
std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b) {
    if (!b) invalid();
    return a / b + (a % b != 0);
}
std::uint64_t pages(std::uint64_t n, std::uint64_t width) {
    // Pages contain at most 61,440 payload bytes. The last page is short.
    return add(mul(n, width), mul(ceil_div(n, 61440 / width), 4096));
}
}

TranscodeBounds transcode_bounds(const TranscodeShape& shape, int level,
                                std::uint64_t process_reserve, const BlockPolicy& policy) {
    if (!shape.input_bytes || !process_reserve || shape.records.series_slots < 0
        || shape.records.constant_count < 0 || shape.records.series_slots > 65536
        || shape.records.constant_count > 65536 || policy.block_bytes < 4096
        || policy.block_bytes > (16u << 20) || policy.late_threads < 1 || policy.late_threads > 4) invalid();
    std::array<std::uint64_t, 8> columns{{3, 20,
        3 + static_cast<std::uint64_t>(shape.records.series_slots), 11,
        static_cast<std::uint64_t>(shape.records.constant_count), 4, 1, 1}};
    std::array<bool, 7> present{{true, true, shape.records.outputs, shape.records.outputs,
        shape.records.outputs, shape.records.trace, shape.records.hashes}};
    std::array<std::uint64_t, 8> blocks{};
    std::uint64_t rows = 0, cells = 0, framing = 0;
    TranscodeBounds out;
    for (unsigned tag = 0; tag < 7; ++tag) {
        const auto n = shape.counts[tag];
        if ((!present[tag] && n) || (tag == 4 && n > 1) || !policy.block_records[tag]
            || policy.block_records[tag] > 65536) invalid();
        rows = add(rows, n);
        cells = add(cells, mul(n, columns[tag]));
        if (!n) continue;
        const auto header = add(32, mul(48, columns[tag]));
        if (header >= BlockPolicy::max_block_bytes) invalid();
        // Next-fit closes on a record cap, soft bytes or the hard builder
        // budget. For each byte-driven close, adjacent bins exceed threshold.
        // Sixteen bytes/cell covers builder/encoded numeric, validity and
        // string length/index bookkeeping; string bytes are <= input bytes.
        const auto payload = add(shape.input_bytes, mul(16, mul(n, columns[tag])));
        const auto threshold = std::min<std::uint64_t>(policy.block_bytes,
            BlockPolicy::max_block_bytes - header);
        const auto by_size = add(mul(2, ceil_div(payload, threshold)), 2);
        blocks[tag] = std::min(n, add(ceil_div(n, policy.block_records[tag]), by_size));
    }
    if (!policy.block_records[7] || policy.block_records[7] > 1048576) invalid();
    blocks[7] = ceil_div(rows, policy.block_records[7]);
    cells = add(cells, rows); // I's byte column, conservatively charged as 16
    for (unsigned tag = 0; tag < 8; ++tag) {
        out.block_count = add(out.block_count, blocks[tag]);
        framing = add(framing, mul(blocks[tag], add(32, mul(51, columns[tag]))));
    }
    // Canonical payload stores <= raw. SUM2 alone may expand, so use its
    // independent stored ceiling. Always charge FULL indexes: this remains
    // an upper bound when final counts select COMPACT.
    auto bytes = add(2u << 20, ColumnCodec::max_summary_stored_bytes);
    bytes = add(bytes, add(shape.input_bytes, mul(16, cells)));
    bytes = add(bytes, framing);
    for (auto n : blocks) bytes = add(bytes, pages(n, 512));
    for (unsigned direction = 0; direction < 10; ++direction)
        bytes = add(bytes, pages(shape.counts[1], shape.counts[1] > UINT32_MAX ? 8 : 4));
    bytes = add(bytes, pages(std::min<std::uint64_t>(shape.counts[0], 1200), 256));
    out.spool_bytes = add(bytes, 4096 + 160);
    const auto codec = codec_workspace_bound(level);
    out.codec_bytes = add(codec.encoder, codec.decoder);
    // The production transcoder is inline. Eight builders, two reused Umax
    // scratch buffers, metadata and F/I/O. Six further R cover live and
    // retained decoded capacities (4R), stored/raw column buffers (R combined),
    // and current-block input fingerprints/I tags (<R even at maximum caps).
    // These are released before SUMMARY/F; no run-length history.
    const auto fixed = add(out.codec_bytes, (4u << 20) + (2u << 20));
    out.buffer_bytes = add(mul(16, BlockPolicy::max_block_bytes), fixed);
    if (policy.late_threads > 1) {
        // Late phases never overlap ingestion. Charge the maximum live
        // phase, including the SUM of every lane's private storage:
        // rank lanes: two-R numeric scratch each, decoder-only static context;
        // verification lanes: bounded metadata/schema copies (2R), one
        // decoded block (R), framing/offsets (2R), stored/raw columns (R),
        // two pages, one 64-KiB probe/bitset and a 64-KiB reread (4*64 KiB),
        // and an independent decoder context. Charge another 2R for the
        // caller's original metadata plus the fixed codec/F/I/O envelope.
        // Charge an 8-MiB address-space stack/TLS reserve per extra thread.
        // As with allocator/loader state, a host using larger thread stacks
        // must certify the excess in process_reserve. Resident usage and the
        // unchanged +16-MiB/1-MiB slope band still need kernel VmHWM proof.
        const auto rank_lanes = std::min(2u, policy.late_threads - 1);
        const auto rank_buffers = add(mul(rank_lanes + 1, mul(2, BlockPolicy::max_block_bytes)),
                                      mul(rank_lanes, codec.decoder)); // includes figure lane's scratch
        const auto verify_buffers = mul(policy.late_threads,
            add(mul(6, BlockPolicy::max_block_bytes), add(4u * 65536, codec.decoder)));
        const auto stacks = mul(policy.late_threads - 1, 8u << 20);
        const auto late = add(fixed, add(mul(2, BlockPolicy::max_block_bytes),
            add(std::max(rank_buffers, verify_buffers), stacks)));
        out.buffer_bytes = std::max(out.buffer_bytes, late);
    }
    out.E_as = add(out.buffer_bytes, process_reserve);
    return out;
}

}} // namespace pineforge::report_sink
