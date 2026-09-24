#pragma once

// R5 lane D2-D: the report's UTC month key, read through a memo by the
// monthly Sharpe / Sortino walk (src/engine_metrics.cpp). Internal to the
// kernel's report metrics and to the rows that hold the memo to the key it
// keeps (tests/test_utc_month_memo.cpp); not installed.

#include <cstdint>
#include <ctime>

namespace pineforge {
namespace metrics {
namespace detail {

// gmtime_r's (tm_year + 1900) * 12 + tm_mon for the stamp's whole seconds --
// the millisecond stamp divided by 1000, truncated toward zero -- by integer
// civil-from-days arithmetic within +/-2^40 seconds of the epoch, gmtime_r
// beyond (R5 lane PERF-P5611).
int month_key_utc(int64_t ts_ms);

// The whole seconds [lo, hi) of one civil month inside the arithmetic's span,
// and the key month_key_utc answers for every one of them. Empty until a
// month is kept; a caller-owned value that nothing else reads.
struct UtcMonthMemo {
    int64_t lo = 1;
    int64_t hi = 0;
    int key = 0;
};

// month_key_utc(ts_ms), and the month of the stamp's second kept in `memo`
// when the arithmetic answered it (src/engine_metrics.cpp).
int keep_utc_month(UtcMonthMemo& memo, int64_t ts_ms);

// month_key_utc(ts_ms): the kept key while the stamp's second is inside the
// kept month, else keep_utc_month's answer.
inline int month_key_utc(UtcMonthMemo& memo, int64_t ts_ms) {
    const int64_t secs = static_cast<int64_t>(static_cast<time_t>(ts_ms / 1000));
    if (memo.lo <= secs && secs < memo.hi) return memo.key;
    return keep_utc_month(memo, ts_ms);
}

}  // namespace detail
}  // namespace metrics
}  // namespace pineforge
