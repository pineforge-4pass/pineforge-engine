#pragma once
#include <pineforge/source/market_admission.hpp>
#include <pineforge/detail/scratch_arena.hpp>

namespace pineforge::admission {
inline namespace market_admission_v2 {
struct JournalRecordSummary {
    std::uint64_t sequence = 0;
    int rejected_bar = -1;
    bool rejected = false;
};
inline bool rejected_command(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::RejectedIntradayCap: case Outcome::RejectedFrozenMarketCap:
    case Outcome::RejectedAffordability: case Outcome::RejectedPricedCap:
    case Outcome::OpeningRejectedReductionAdmitted: return true;
    default: return false;
    }
}
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, JournalRecordSummary&) noexcept;
detail::ScratchStatus fold_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, std::uint64_t&) noexcept;
// Validate the schema and extend the canonical fold in one forward pass.
// A sequence at/below the frontier is validated without extending the digest.
// Neither value output is published on failure. The optional diagnostic owns
// the selected I/O failure across any validation-only error replay. After a
// skipped text region, any failure replays the old scan for error priority;
// successful records never replay.
detail::ScratchStatus fold_validated_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, JournalRecordSummary&, std::uint64_t&,
    std::uint64_t after_sequence = 0,
    std::optional<detail::ReportScratchIoError>* failure = nullptr) noexcept;
}
}
