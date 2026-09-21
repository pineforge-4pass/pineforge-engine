#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace pineforge {

// TradingView accepts fixed-offset names like "GMT+1" / "UTC-5" with the
// human-readable sign convention. POSIX TZ strings use the opposite sign, so
// engine code that calls setenv("TZ", ...) must normalize through this helper.
std::string normalize_timezone_for_posix(const std::string& tz);

// ---------------------------------------------------------------------------
// Timeframe bucket open / close for a bar timestamp (the `time(timeframe,
// session?, timezone?)` and `time_close(...)` family a source frontend lowers
// to).
// Returns Unix milliseconds, or na<int64_t>() when the bar is outside the
// requested session (TradingView semantics for filtered sessions).
//
// `syminfo_tz` is the symbol's exchange timezone (Pine `syminfo.timezone`;
// codegen passes `syminfo_.timezone`). It is the DEFAULT zone the `session`
// window is interpreted in when `tz` is empty — Pine reads a tz-less session
// in the exchange timezone, not UTC. An explicit `tz` always wins. It does
// NOT influence the timeframe open/close computation (explicit `tz`, else
// UTC, exactly as before); an empty `syminfo_tz` reproduces the historical
// UTC session default byte-for-byte.
//
// Feature macro: generated code tests this to decide whether the trailing
// `syminfo_tz` argument exists, so one transpiler output compiles against
// both this header and older ones (`PF_PINE_TIME_SYMINFO_TZ_ARG(x)` in the
// emitted prelude expands to `, (x)` only when it is defined).
// ---------------------------------------------------------------------------

#define PF_PINE_TIME_HAS_SYMINFO_TZ 1

int64_t timeframe_time(int64_t bar_ms,
                       const std::string& tf,
                       const std::string& session,
                       const std::string& tz,
                       const std::string& chart_tf,
                       const std::string& syminfo_tz = std::string());

int64_t timeframe_time_close(int64_t bar_ms,
                             const std::string& tf,
                             const std::string& session,
                             const std::string& tz,
                             const std::string& chart_tf,
                             const std::string& syminfo_tz = std::string());

// Symbol-clock forms. `sym_tz` / `sym_session` are Pine's syminfo.timezone
// and syminfo.session (the engine's runtime overrides). For a D/W/M `tf`
// called WITHOUT a valid session argument the returned open / close is the
// SYMBOL's daily/weekly/monthly bar (17:00 ET on OANDA forex, 09:30 ET RTH
// on NASDAQ equities, 00:00 UTC on a 24x7 UTC symbol — see
// session_period_open_ms in timeframe.hpp). A valid `session` argument
// defines the day in its own `tz` exactly as the forms above do
// (TradingView rolls `time("D", "0000-2359", "America/New_York")` at New
// York midnight on a UTC symbol — measured), and its window is read in the
// explicit `tz`, else `sym_tz` (the syminfo default above), else UTC.
// An intraday `tf` (with or without a session argument, which only
// filters) is the symbol's day-stamp-anchored HTF grid bucket
// (session_intraday_bucket_open_ms in timeframe.hpp — the grid
// request.security aggregates on): time("60") on NYSE:F is 09:30 / 10:30 /
// .. / 15:30 ET, time("240") on NSE:NIFTY 09:15 / 13:15 IST and on
// OANDA:XAUUSD the 17:00-ET-anchored 4h grid (pin-time-hours tapes,
// 2025-04-01..07-01); the five-argument forms stay on the epoch grid.
// With sym_tz="UTC" and an empty / "24x7" sym_session these are
// bit-identical to the five-argument forms above.
//
// Feature macro: generated code tests this to decide whether the trailing
// (sym_tz, sym_session) pair exists (`PF_PINE_TIME_SESSION_DAY_ARGS(tz, s)`
// in the emitted prelude expands to `, tz, s` only when it is defined, so
// the same generated.cpp collapses to the 5-arg call on older engines).
#define PF_PINE_TIME_HAS_SESSION_DAY 1

int64_t timeframe_time(int64_t bar_ms,
                       const std::string& tf,
                       const std::string& session,
                       const std::string& tz,
                       const std::string& chart_tf,
                       const std::string& sym_tz,
                       const std::string& sym_session);

int64_t timeframe_time_close(int64_t bar_ms,
                             const std::string& tf,
                             const std::string& session,
                             const std::string& tz,
                             const std::string& chart_tf,
                             const std::string& sym_tz,
                             const std::string& sym_session);

// ---------------------------------------------------------------------------
// Low-level session helpers (exposed for engine_run.cpp, unit tests,
// session_trading_day_open_ms, and session predicates).
// ---------------------------------------------------------------------------
// Convert "HHMM" string to minutes-since-midnight.  Returns -1 on parse error.
int hhmm_to_minutes(const std::string& hhmm);

// Pine time/date extraction from a Unix-ms bar timestamp in timezone `tz`.
// Value-identical to Pine hour()/minute()/dayofweek()/... (dayofweek 1=Sun..7=Sat,
// month 1..12, year full). Codegen routes hour()/minute()/... through these
// instead of an inline setenv+tzset lambda, which stops the per-call macOS
// tzset()->notifyd IPC storm (KI-35): UTC uses tzset-free gmtime_r, other zones
// use the cached ScopedTimezone. DST-correct (localtime_r via the tz database).
int local_hour(int64_t bar_ms, const std::string& tz);
int local_minute(int64_t bar_ms, const std::string& tz);
int local_second(int64_t bar_ms, const std::string& tz);
int local_dayofmonth(int64_t bar_ms, const std::string& tz);
int local_dayofweek(int64_t bar_ms, const std::string& tz);
int local_month(int64_t bar_ms, const std::string& tz);
int local_year(int64_t bar_ms, const std::string& tz);
int local_weekofyear(int64_t bar_ms, const std::string& tz);

// True when local_tm falls within any of the comma-separated HHMM-HHMM
// windows in windows_body.  "24x7" or empty always returns true.
bool local_time_in_session_windows(const std::string& windows_body,
                                   const struct tm& local_tm);

// True when bar_ms (Unix ms) falls inside the session string for the given tz.
bool passes_session_filter(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms);

// Floor bar_ms to local-calendar midnight in the given IANA timezone.
// DST edge-case: if mktime() returns -1 (non-existent midnight — e.g.,
// America/Havana, Pacific/Lord_Howe spring-forward), the implementation
// retries with midnight + 1 h and midnight + 2 h to find a valid epoch
// (i.e. uses the first available second after the gap). A warning is
// logged once per occurrence via fprintf(stderr). The fallback is
// conservative: the returned timestamp is never more than 2 hours later
// than the true calendar-day start; downstream consumers
// (session_trading_day_open_ms) inherit the same semantics.
int64_t calendar_day_open_local_ms(int64_t bar_ms, const std::string& tz);

// -------------------------------------------------------------------------
// time_tradingday built-in
// -------------------------------------------------------------------------
// Returns the Unix-ms timestamp of the session-open of the trading day
// that contains bar_ms.  See implementation for full algorithm + DST notes.
int64_t session_trading_day_open_ms(int64_t bar_ms,
                                    const std::string& session,
                                    const std::string& tz);

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

bool session_in_market(const std::string& session,
                       const std::string& tz,
                       int64_t bar_ms);

bool session_in_premarket(const std::string& session,
                          const std::string& tz,
                          int64_t bar_ms);

bool session_in_postmarket(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms);

// Chart-timeframe forms: the rule the session.is* variables of a CHART bar
// follow. TradingView evaluates them on the chart bar, and a daily-or-higher
// bar covers whole session days rather than a time of day, so on "1D" and
// above it documents session.ismarket as always true and session.ispremarket
// / session.ispostmarket as always false (Pine docs, Sessions). OANDA:XAUUSD
// @1D made the gap physical: an 1800-1700 America/New_York session and a tape
// stamped at the 17:00 ET break put every daily open OUTSIDE the time-of-day
// windows above, session.ismarket never held, and a script that ANDs every
// signal with it took 0 trades against TradingView's 57. With an intraday or
// empty chart_tf these forms defer to the three-argument time-of-day forms
// above, byte for byte.
bool session_in_market(const std::string& session,
                       const std::string& tz,
                       int64_t bar_ms,
                       const std::string& chart_tf);

bool session_in_premarket(const std::string& session,
                          const std::string& tz,
                          int64_t bar_ms,
                          const std::string& chart_tf);

bool session_in_postmarket(const std::string& session,
                           const std::string& tz,
                           int64_t bar_ms,
                           const std::string& chart_tf);

// ---------------------------------------------------------------------------
// Deprecated `pine_*` spellings. They are exact inline forwards, kept so
// generated code and the source adapter compile unchanged; new code calls the
// neutral names above.
// ---------------------------------------------------------------------------

inline int64_t pine_time(int64_t bar_ms, const std::string& tf,
                         const std::string& session, const std::string& tz,
                         const std::string& chart_tf,
                         const std::string& syminfo_tz = std::string()) {
    return timeframe_time(bar_ms, tf, session, tz, chart_tf, syminfo_tz);
}

inline int64_t pine_time(int64_t bar_ms, const std::string& tf,
                         const std::string& session, const std::string& tz,
                         const std::string& chart_tf, const std::string& sym_tz,
                         const std::string& sym_session) {
    return timeframe_time(bar_ms, tf, session, tz, chart_tf, sym_tz, sym_session);
}

inline int64_t pine_time_close(int64_t bar_ms, const std::string& tf,
                               const std::string& session, const std::string& tz,
                               const std::string& chart_tf,
                               const std::string& syminfo_tz = std::string()) {
    return timeframe_time_close(bar_ms, tf, session, tz, chart_tf, syminfo_tz);
}

inline int64_t pine_time_close(int64_t bar_ms, const std::string& tf,
                               const std::string& session, const std::string& tz,
                               const std::string& chart_tf, const std::string& sym_tz,
                               const std::string& sym_session) {
    return timeframe_time_close(bar_ms, tf, session, tz, chart_tf, sym_tz, sym_session);
}

inline int pine_hour(int64_t bar_ms, const std::string& tz) { return local_hour(bar_ms, tz); }
inline int pine_minute(int64_t bar_ms, const std::string& tz) { return local_minute(bar_ms, tz); }
inline int pine_second(int64_t bar_ms, const std::string& tz) { return local_second(bar_ms, tz); }
inline int pine_dayofmonth(int64_t bar_ms, const std::string& tz) { return local_dayofmonth(bar_ms, tz); }
inline int pine_dayofweek(int64_t bar_ms, const std::string& tz) { return local_dayofweek(bar_ms, tz); }
inline int pine_month(int64_t bar_ms, const std::string& tz) { return local_month(bar_ms, tz); }
inline int pine_year(int64_t bar_ms, const std::string& tz) { return local_year(bar_ms, tz); }
inline int pine_weekofyear(int64_t bar_ms, const std::string& tz) { return local_weekofyear(bar_ms, tz); }

inline int64_t pine_time_tradingday(int64_t bar_ms, const std::string& session,
                                    const std::string& tz) {
    return session_trading_day_open_ms(bar_ms, session, tz);
}

inline bool pine_session_ismarket(const std::string& session, const std::string& tz,
                                  int64_t bar_ms) {
    return session_in_market(session, tz, bar_ms);
}

inline bool pine_session_ispremarket(const std::string& session, const std::string& tz,
                                     int64_t bar_ms) {
    return session_in_premarket(session, tz, bar_ms);
}

inline bool pine_session_ispostmarket(const std::string& session, const std::string& tz,
                                      int64_t bar_ms) {
    return session_in_postmarket(session, tz, bar_ms);
}

inline bool pine_session_ismarket(const std::string& session, const std::string& tz,
                                  int64_t bar_ms, const std::string& chart_tf) {
    return session_in_market(session, tz, bar_ms, chart_tf);
}

inline bool pine_session_ispremarket(const std::string& session, const std::string& tz,
                                     int64_t bar_ms, const std::string& chart_tf) {
    return session_in_premarket(session, tz, bar_ms, chart_tf);
}

inline bool pine_session_ispostmarket(const std::string& session, const std::string& tz,
                                      int64_t bar_ms, const std::string& chart_tf) {
    return session_in_postmarket(session, tz, bar_ms, chart_tf);
}

} // namespace pineforge
