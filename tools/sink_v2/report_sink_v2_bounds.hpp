#pragma once

#include "report_sink_v2_writer.hpp"
#include <array>

namespace pineforge { namespace report_sink {

// Upper counts and bytes must come from the stage-A-verified stream/receipt,
// or from stronger compiler certificates. Header descriptors alone have no
// row counts. An absent proof is not a zero.
struct TranscodeShape {
    RecordSet records{};
    std::array<std::uint64_t, 7> counts{}; // E T O V K X H
    std::uint64_t input_bytes = 0;        // complete v1, including trailer
};

struct TranscodeBounds {
    std::uint64_t spool_bytes = 0; // whole output, including ranks and tail
    std::uint64_t spool_files = 1;
    std::uint64_t block_count = 0;
    std::uint64_t codec_bytes = 0;
    std::uint64_t buffer_bytes = 0;
    std::uint64_t E_as = 0;
};

// process_reserve is a separately certified image/allocator/stack/loader
// envelope. E_as is an address-space admission bound conditional on that
// certificate, not an RSS measurement. Resident tmpfs output is spool_bytes
// and must also enter the jail's inclusive memory admission.
TranscodeBounds transcode_bounds(const TranscodeShape&, int level,
                                std::uint64_t process_reserve,
                                const BlockPolicy& = {});

}} // namespace pineforge::report_sink
