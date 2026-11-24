#pragma once

#include <pineforge/run_failure.hpp>
#include <stdexcept>

namespace pineforge::detail {

// The existing failure code/args, with an internal type so C accessors can
// carry this newly admitted refusal without changing older error paths.
class RetiredTradeHistoryRead final : public std::logic_error, public RunFailureInfo {
public:
    enum class Reason { Retired, Released };

    explicit RetiredTradeHistoryRead(Reason reason = Reason::Retired)
        : std::logic_error(reason == Reason::Released
              ? "trade ledger: closed trades released"
              : "trade ledger: row outside resident window"),
          RunFailureInfo(RunFailureCode::engine_invariant, {}) {}
};

} // namespace pineforge::detail
