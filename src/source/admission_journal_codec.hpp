#pragma once
#include <pineforge/source/market_admission.hpp>

namespace pineforge::admission {
inline namespace market_admission_v2 {
struct JournalRecordSummary {
    std::uint64_t sequence = 0;
    int rejected_bar = -1;
    bool rejected = false;
};
detail::ScratchStatus scan_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, JournalRecordSummary&) noexcept;
detail::ScratchStatus fold_journal_record(const detail::ScratchByteSource&,
    detail::ScratchBuffer, std::uint64_t&) noexcept;
}
}
