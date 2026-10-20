#pragma once

#include <pineforge/run_failure.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace pineforge::report {

// Call only on a value actually computed/copied into a finite result domain.
// An explicit out-of-domain metric sentinel is chosen by its caller before
// this check. In particular, a computed NaN is never an absence.
inline double finite_value(double value, const char* field, const char* prefix = nullptr) {
    if (!std::isfinite(value)) {
        const std::string path = prefix ? std::string(prefix) + "." + field : field;
        throw coded<std::runtime_error>(
            RunFailureCode::report_result_invalid,
            RunFailureArgs{{"field", path}, {"reason", "nonfinite"}},
            "The report producer generated an invalid canonical result.");
    }
    return value;
}

// A missed execution invariant is still an R at the report boundary. This
// does not replace the earlier engine-owned settlement/admission guards.
template<class Trade>
inline void validate_trade(const Trade& trade) {
#define PF_FINITE_TRADE(field) (void)finite_value(trade.field, "trades." #field)
    PF_FINITE_TRADE(entry_price);
    PF_FINITE_TRADE(exit_price);
    PF_FINITE_TRADE(qty);
    PF_FINITE_TRADE(pnl);
    PF_FINITE_TRADE(pnl_pct);
    PF_FINITE_TRADE(max_runup);
    PF_FINITE_TRADE(max_drawdown);
    PF_FINITE_TRADE(commission);
#undef PF_FINITE_TRADE
}

}  // namespace pineforge::report
