#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace pineforge {

// ---------------------------------------------------------------------------
// Pine time(timeframe, session?, timezone?) and time_close(...).
// Returns Unix milliseconds, or na<int64_t>() when the bar is outside the
// requested session (TradingView semantics for filtered sessions).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Low-level session helpers (exposed for engine_run.cpp and unit tests).
// ---------------------------------------------------------------------------

// Convert "HHMM" string to minutes-since-midnight.  Returns -1 on parse error.
int hhmm_to_minutes(const std::string& hhmm);

// True when local_tm falls within any of the comma-separated HHMM-HHMM
// windows in windows_body.  "24x7" or empty always returns true.
bool local_time_in_session_windows(const std::string& windows_body,
                                   const struct tm& local_tm);

// True when bar_ms (Unix ms) falls inside the session string for the given tz.
bool passes_session_filter(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms);

// ---------------------------------------------------------------------------
// Session predicates backing session.is* Pine v6 variables.
//
// LIMITATION: The engine has a single syminfo.session string and cannot
// distinguish RTH from ETH.  Therefore:
//   session.isfirstbar_regular  == session.isfirstbar
//   session.islastbar_regular   == session.islastbar
// in the current architecture.  Future work: add SymInfo.regular_session
// field for strict RTH separation (deferred — separate sprint).
//
// Pre/post-market predicates assume standard US ETH windows:
//   premarket : 04:00 – RTH_open  (RTH_open parsed from session string)
//   postmarket: RTH_close – 20:00
// For exchanges with non-standard ETH (e.g. LSE auctions), accuracy may
// degrade.  This is documented behaviour, not a bug.
// ---------------------------------------------------------------------------

// pine_session_ismarket: true when bar_ms is inside the regular session.
bool pine_session_ismarket(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms);

// pine_session_ispremarket: true when bar_ms is in the pre-market window
// [04:00, RTH_open) local time, on days matching the session day filter.
bool pine_session_ispremarket(const std::string& session,
                              const std::string& tz,
                              int64_t bar_ms);

// pine_session_ispostmarket: true when bar_ms is in the post-market window
// [RTH_close, 20:00) local time, on days matching the session day filter.
bool pine_session_ispostmarket(const std::string& session,
                               const std::string& tz,
                               int64_t bar_ms);

} // namespace pineforge
