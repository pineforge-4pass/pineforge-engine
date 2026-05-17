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

// -------------------------------------------------------------------------
// Helpers promoted from file-static for use by pine_time_tradingday and
// session predicates (Agent A).
// -------------------------------------------------------------------------

// Parse a 4-digit HHMM string (e.g., "0930") into minutes since midnight.
// Returns -1 on parse error.
int hhmm_to_minutes(const std::string& hhmm);

// Floor bar_ms to local-calendar midnight in the given IANA timezone.
// DST edge-case: if mktime() returns -1 (non-existent midnight — e.g.,
// America/Havana, Pacific/Lord_Howe spring-forward), the implementation
// retries with midnight + 1 h and midnight + 2 h to find a valid epoch
// (i.e. uses the first available second after the gap). A warning is
// logged once per occurrence via fprintf(stderr). The fallback is
// conservative: the returned timestamp is never more than 2 hours later
// than the true calendar-day start; downstream consumers (pine_time_tradingday)
// inherit the same semantics.
int64_t calendar_day_open_local_ms(int64_t bar_ms, const std::string& tz);

// -------------------------------------------------------------------------
// time_tradingday built-in
// -------------------------------------------------------------------------
// Returns the Unix-ms timestamp of the session-open of the trading day
// that contains bar_ms.
//
// Algorithm:
//   1. Parse session start from the first window in `session` (e.g.,
//      "0930-1600" → 570 minutes).
//   2. Obtain bar's local time in `tz`.
//   3. If bar's local minutes-of-day >= session_start_minutes:
//        trading_day_open = today's session_start in `tz`.
//      Else:
//        trading_day_open = yesterday's session_start in `tz`.
//   4. Edge case — 24/7 session (empty string, "24x7", or "0000-2400"):
//        fall back to UTC calendar-day midnight.
//
// DST handling: delegates to calendar_day_open_local_ms() which already
// contains the mktime-retry fallback. See that function's header comment.
int64_t pine_time_tradingday(int64_t bar_ms,
                             const std::string& session,
                             const std::string& tz);

} // namespace pineforge
