#pragma once

// The report's UTC month key, read through a memo by the
// monthly Sharpe / Sortino walk (src/engine_metrics.cpp). Internal to the
// kernel's report metrics and to the rows that hold the memo to the key it
// keeps (tests/test_utc_month_memo.cpp); not installed.

#include <cstdint>

namespace pineforge {
namespace metrics {
namespace detail {

// gmtime_r's (tm_year + 1900) * 12 + tm_mon for the stamp's whole seconds --
// the millisecond stamp divided by 1000, truncated toward zero -- by integer
// civil-from-days arithmetic across the entire int64 millisecond range.
int64_t month_key_utc(int64_t ts_ms);

// The whole seconds [lo, hi) of one civil month inside +/-2^40 seconds,
// and the key month_key_utc answers for every one of them. Empty until a
// month is kept; a caller-owned value that nothing else reads.
struct UtcMonthMemo {
    int64_t lo = 1;
    int64_t hi = 0;
    int64_t key = 0;
};

// month_key_utc(ts_ms), and the month of the stamp's second kept in `memo`
// when inside the cache span (src/engine_metrics.cpp).
int64_t keep_utc_month(UtcMonthMemo& memo, int64_t ts_ms);

// month_key_utc(ts_ms): the kept key while the stamp's second is inside the
// kept month, else keep_utc_month's answer.
inline int64_t month_key_utc(UtcMonthMemo& memo, int64_t ts_ms) {
    const int64_t secs = ts_ms / 1000;
    if (memo.lo <= secs && secs < memo.hi) return memo.key;
    return keep_utc_month(memo, ts_ms);
}

}  // namespace detail
}  // namespace metrics
}  // namespace pineforge
