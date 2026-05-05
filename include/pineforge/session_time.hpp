#pragma once
#include <cstdint>
#include <string>

namespace pineforge {

// Pine time(timeframe, session?, timezone?) and time_close(...).
// Returns Unix milliseconds, or na<int64_t>() when the bar is outside the
// requested session (TradingView semantics for filtered sessions).

int64_t pine_time(int64_t bar_ms,
                  const std::string& tf,
                  const std::string& session,
                  const std::string& tz,
                  const std::string& chart_tf);

int64_t pine_time_close(int64_t bar_ms,
                        const std::string& tf,
                        const std::string& session,
                        const std::string& tz,
                        const std::string& chart_tf);

} // namespace pineforge
