#pragma once

#include <pineforge/run_failure.hpp>
#include <stdexcept>

namespace pineforge::detail {

// The existing failure code/args, with an internal type so C accessors can
// carry this newly admitted refusal without changing older error paths.
class RetiredTradeHistoryRead final : public std::logic_error, public RunFailureInfo {
public:
    RetiredTradeHistoryRead()
        : std::logic_error("trade ledger: row outside resident window"),
          RunFailureInfo(RunFailureCode::engine_invariant, {}) {}
};

} // namespace pineforge::detail
