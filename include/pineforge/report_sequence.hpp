#pragma once

#include <cstdint>

// CR-150 lane o1w-seq (R14--R18). The optional per-bar sequences -- the
// broker-state hashes, the runtime trace, the recorded output rows and their
// events -- are not part of a report's scalar facts. A full report keeps them
// where it always did (the owning arrays in ReportC, or the report sink
// stream). The summary retention mode keeps only a fixed ordered witness per
// sequence, so a trial's resident memory does not grow with the bar, trace or
// event population.
//
// ReportMode::LegacyArrays is the default and the byte-identical historical
// behaviour. ReportMode::SummaryOnly is the placeholder the lane brief names
// (the final summary/full flag is owned by lane o1w-contract); in it,
// set_optional_report_mode returns 0, the ReportC optional arrays stay null,
// and optional_sequence_summary() returns the fixed witness. Without a sink,
// enabled output recording retains the current row: output_bars_len() is 1
// once a row exists, and output_bar_times_copy()/output_series_copy() expose
// that row at local row 0, independently of its global bar index. Event
// readers answer empty. A report sink, when installed, still streams the
// complete sequence externally in either mode.
#define PINEFORGE_HAS_OPTIONAL_REPORT_SEQUENCES_V1 1

namespace pineforge {
inline namespace engine_script_run_v19 {
namespace report_sequence {

enum class ReportMode : std::uint8_t {
    LegacyArrays = 0,
    SummaryOnly = 1,
};

// One sequence's ordered witness: how many records were consumed and a digest
// folded over every semantic field in record order. Both are O(1) state. The
// witness is maintained by SummaryOnly runs and by every run whose report sink
// streams the sequence; a pure LegacyArrays run without a sink keeps the
// owning ReportC arrays instead and does not promise a complete witness.
struct SequenceWitness {
    std::uint64_t count = 0;
    std::uint64_t digest = 0;
};

struct OptionalSequenceSummary {
    SequenceWitness hashes;
    SequenceWitness traces;
    SequenceWitness rows;
    SequenceWitness events;
};

}  // namespace report_sequence
}  // inline namespace engine_script_run_v19
}  // namespace pineforge
