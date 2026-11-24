#pragma once

#include <pineforge/run_failure.hpp>

#include <stdexcept>

namespace pineforge::detail {

class ActiveRunResultsRelease final : public std::logic_error, public RunFailureInfo {
public:
    static constexpr const char* message = "run results release refused while a run or stream is active";

    ActiveRunResultsRelease()
        : std::logic_error(message), RunFailureInfo(RunFailureCode::setting_rejected, {}) {}
};

class ReleasedNativeJournalRead final : public std::logic_error, public RunFailureInfo {
public:
    ReleasedNativeJournalRead()
        : std::logic_error("native command journal: run results released"),
          RunFailureInfo(RunFailureCode::engine_invariant, {}) {}
};

// Record the C query refusal unless the handle has a run failure to preserve.
void note_cabi_run_results_read_refusal(BacktestEngine&, const std::exception&) noexcept;

} // namespace pineforge::detail
