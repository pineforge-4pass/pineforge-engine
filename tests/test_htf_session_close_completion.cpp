// request.security HTF buckets on exchange-calendar sessions complete on the
// period's LAST chart bar, the way TradingView finalizes them (findings
// 451/452):
//
//   A. an intraday bucket that straddles the RTH close ('240' 13:30-17:30
//      holds 10 of 16 fifteen-minute sub-bars) completes on the 15:45 ET bar,
//      not on the next session's 09:30 bar;
//   B. an equity week completes on Friday 15:45 ET (Friday + 24h is Saturday,
//      still the same week, so the old same-wall-clock-tomorrow test missed
//      it) and a month whose last calendar day is a weekend completes on its
//      last Friday;
//   C. the forex (1700-1700) week / month completes on Friday 16:45 ET, not on
//      Sunday 17:00 (the Friday-17:00 instant is the Friday-open session-day —
//      Saturday's trading date — so the next-bar crossing never fired).
//
// 24x7 / UTC feeds must stay bit-identical: the count / boundary / projection
// rules are the only ones that run there.
#include <cstdio>
#include <string>
#include <vector>

#include <pineforge/timeframe.hpp>
#include <pineforge/bar.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

#define CHECK_EQ_MS(actual, expected)                                          \
    do {                                                                        \
        const int64_t _a = (actual), _e = (expected);                          \
        if (_a != _e) {                                                         \
            std::printf("  FAIL  %s:%d  %s == %s  (got %lld, want %lld)\n",     \
                        __FILE__, __LINE__, #actual, #expected,                 \
                        (long long)_a, (long long)_e);                          \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

namespace {

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

// Every date below is in EDT (UTC-4); the tests never straddle a DST edge.
int64_t edt_ms(int y, int m, int d, int h, int mi) {
    return utc_ms(y, m, d, h + 4, mi);
}

const std::string NY  = "America/New_York";
const std::string RTH = "0930-1600";
const std::string FX  = "1700-1700";
const int64_t k15m = 15 * 60 * 1000;

Bar bar_at(int64_t ts) {
    Bar b;
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.0;
    b.volume = 1.0;
    return b;
}

struct Completion {
    int64_t at;         // timestamp of the input bar that finalized the bucket
    int64_t bucket_ts;  // timestamp of the completed HTF bar (its first sub-bar)
    int subs;
};

// Feed 15m bars and record every completion.
std::vector<Completion> drive(TimeframeAggregator& agg, const std::vector<int64_t>& ts) {
    std::vector<Completion> out;
    for (int64_t t : ts) {
        AggregatedBar r = agg.feed(bar_at(t));
        if (r.is_complete) out.push_back({t, r.bar.timestamp, r.sub_bar_count});
    }
    return out;
}

// RTH 15m grid for one date: 09:30 .. 15:45 ET (26 bars).
void rth_day(std::vector<int64_t>& v, int y, int m, int d) {
    for (int i = 0; i < 26; ++i) v.push_back(edt_ms(y, m, d, 9, 30) + i * k15m);
}

// Forex 15m grid for one session-day: 17:00 ET on (y,m,d) .. 16:45 ET next day.
void fx_session(std::vector<int64_t>& v, int y, int m, int d) {
    for (int i = 0; i < 96; ++i) v.push_back(edt_ms(y, m, d, 17, 0) + i * k15m);
}

bool has_completion_at(const std::vector<Completion>& c, int64_t at) {
    for (const auto& x : c) if (x.at == at) return true;
    return false;
}

const Completion* completion_at(const std::vector<Completion>& c, int64_t at) {
    for (const auto& x : c) if (x.at == at) return &x;
    return nullptr;
}

}  // namespace

// ─── helper ───────────────────────────────────────────────────────────────────

static void test_last_traded_close_helper() {
    std::printf("test_last_traded_close_helper\n");
    // Equity week containing Wed 2025-06-04 closes Fri 2025-06-06 16:00 ET.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 6, 4, 11, 0), NY, RTH,
                                                    CalendarPeriod::WEEK),
                edt_ms(2025, 6, 6, 16, 0));
    // DAY is the plain session close.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 6, 4, 11, 0), NY, RTH,
                                                    CalendarPeriod::DAY),
                edt_ms(2025, 6, 4, 16, 0));
    // August 2025 ends on a Sunday: last traded session-day is Fri 08-29.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 8, 12, 11, 0), NY, RTH,
                                                    CalendarPeriod::MONTH),
                edt_ms(2025, 8, 29, 16, 0));
    // October 2025 ends on a Friday: nothing to skip.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 10, 7, 11, 0), NY, RTH,
                                                    CalendarPeriod::MONTH),
                edt_ms(2025, 10, 31, 16, 0));
    // Forex week: Sun 17:00 ET .. Fri 17:00 ET (Friday-open session is
    // Saturday's trading date and never trades).
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 6, 4, 3, 0), NY, FX,
                                                    CalendarPeriod::WEEK),
                edt_ms(2025, 6, 6, 17, 0));
    // Forex daily: the session-day containing Fri 16:45 closes Fri 17:00.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 6, 6, 16, 45), NY, FX,
                                                    CalendarPeriod::DAY),
                edt_ms(2025, 6, 6, 17, 0));
    // May 2025 ends on a Saturday: the forex month closes Fri 05-30 17:00 ET.
    CHECK_EQ_MS(session_period_last_traded_close_ms(edt_ms(2025, 5, 14, 3, 0), NY, FX,
                                                    CalendarPeriod::MONTH),
                edt_ms(2025, 5, 30, 17, 0));
    // 24x7 / UTC: no weekend to skip — identical to session_period_close_ms.
    const int64_t t = utc_ms(2025, 6, 4, 11, 0);
    CHECK_EQ_MS(session_period_last_traded_close_ms(t, "UTC", "", CalendarPeriod::WEEK),
                session_period_close_ms(t, "UTC", "", CalendarPeriod::WEEK));
    CHECK_EQ_MS(session_period_last_traded_close_ms(t, "UTC", "", CalendarPeriod::WEEK),
                utc_ms(2025, 6, 9, 0, 0));
    CHECK_EQ_MS(session_period_last_traded_close_ms(t, "UTC", "24x7", CalendarPeriod::MONTH),
                utc_ms(2025, 7, 1, 0, 0));
}

// ─── A: RTH intraday buckets ──────────────────────────────────────────────────

static void test_rth_240_completes_at_1545() {
    std::printf("test_rth_240_completes_at_1545\n");
    TimeframeAggregator agg("240", "15", NY, RTH);
    std::vector<int64_t> ts;
    rth_day(ts, 2025, 6, 2);   // Monday
    rth_day(ts, 2025, 6, 3);   // Tuesday
    auto c = drive(agg, ts);
    // Monday: 09:30-13:30 completes by count at 13:15; 13:30-17:30 completes
    // at the session's last bar 15:45 with 10 sub-bars.
    const Completion* first = completion_at(c, edt_ms(2025, 6, 2, 13, 15));
    CHECK(first != nullptr);
    if (first) { CHECK(first->subs == 16); CHECK_EQ_MS(first->bucket_ts, edt_ms(2025, 6, 2, 9, 30)); }
    const Completion* last = completion_at(c, edt_ms(2025, 6, 2, 15, 45));
    CHECK(last != nullptr);
    if (last) { CHECK(last->subs == 10); CHECK_EQ_MS(last->bucket_ts, edt_ms(2025, 6, 2, 13, 30)); }
    // Nothing completes on Tuesday 09:30 (pre-fix: the lazy boundary
    // completion of Monday's 13:30 bucket landed here).
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 3, 9, 30)));
    // Tuesday behaves like Monday.
    CHECK(has_completion_at(c, edt_ms(2025, 6, 3, 13, 15)));
    CHECK(has_completion_at(c, edt_ms(2025, 6, 3, 15, 45)));
    CHECK(c.size() == 4);
}

static void test_rth_60_and_45_complete_at_1545() {
    std::printf("test_rth_60_and_45_complete_at_1545\n");
    {
        TimeframeAggregator agg("60", "15", NY, RTH);
        std::vector<int64_t> ts;
        rth_day(ts, 2025, 6, 2);
        rth_day(ts, 2025, 6, 3);
        auto c = drive(agg, ts);
        // 09:30-10:30 .. 14:30-15:30 by count (6), 15:30-16:30 at 15:45 (2 subs).
        const Completion* last = completion_at(c, edt_ms(2025, 6, 2, 15, 45));
        CHECK(last != nullptr);
        if (last) { CHECK(last->subs == 2); CHECK_EQ_MS(last->bucket_ts, edt_ms(2025, 6, 2, 15, 30)); }
        CHECK(!has_completion_at(c, edt_ms(2025, 6, 3, 9, 30)));
        CHECK(c.size() == 14);
    }
    {
        TimeframeAggregator agg("45", "15", NY, RTH);
        std::vector<int64_t> ts;
        rth_day(ts, 2025, 6, 2);
        rth_day(ts, 2025, 6, 3);
        auto c = drive(agg, ts);
        // 09:30, 10:15, .., 14:45-15:30 by count (8), 15:30-16:15 at 15:45 (2 subs).
        const Completion* last = completion_at(c, edt_ms(2025, 6, 2, 15, 45));
        CHECK(last != nullptr);
        if (last) { CHECK(last->subs == 2); CHECK_EQ_MS(last->bucket_ts, edt_ms(2025, 6, 2, 15, 30)); }
        CHECK(!has_completion_at(c, edt_ms(2025, 6, 3, 9, 30)));
        CHECK(c.size() == 18);
    }
}

// ─── B: equity W / M ──────────────────────────────────────────────────────────

static void test_rth_week_completes_friday_1545() {
    std::printf("test_rth_week_completes_friday_1545\n");
    TimeframeAggregator agg("W", "15", NY, RTH);
    std::vector<int64_t> ts;
    for (int d = 2; d <= 6; ++d) rth_day(ts, 2025, 6, d);   // Mon 06-02 .. Fri 06-06
    rth_day(ts, 2025, 6, 9);                                  // Mon 06-09
    auto c = drive(agg, ts);
    const Completion* w = completion_at(c, edt_ms(2025, 6, 6, 15, 45));
    CHECK(w != nullptr);
    if (w) { CHECK(w->subs == 130); CHECK_EQ_MS(w->bucket_ts, edt_ms(2025, 6, 2, 9, 30)); }
    // Never on an interior session close, never on Monday's first bar.
    for (int d = 2; d <= 5; ++d) CHECK(!has_completion_at(c, edt_ms(2025, 6, d, 15, 45)));
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 9, 9, 30)));
    CHECK(c.size() == 1);
    // The new week is a fresh bucket.
    CHECK_EQ_MS(agg.current().timestamp, edt_ms(2025, 6, 9, 9, 30));
}

static void test_rth_month_ending_on_weekend_completes_last_friday() {
    std::printf("test_rth_month_ending_on_weekend_completes_last_friday\n");
    TimeframeAggregator agg("M", "15", NY, RTH);
    std::vector<int64_t> ts;
    for (int d = 25; d <= 29; ++d) rth_day(ts, 2025, 8, d);   // Mon 08-25 .. Fri 08-29
    rth_day(ts, 2025, 9, 2);                                   // Tue 09-02 (Labor Day gap)
    auto c = drive(agg, ts);
    const Completion* m = completion_at(c, edt_ms(2025, 8, 29, 15, 45));
    CHECK(m != nullptr);
    if (m) { CHECK(m->subs == 130); CHECK_EQ_MS(m->bucket_ts, edt_ms(2025, 8, 25, 9, 30)); }
    CHECK(!has_completion_at(c, edt_ms(2025, 9, 2, 9, 30)));
    CHECK(c.size() == 1);
}

static void test_rth_month_ending_on_weekday_unchanged() {
    std::printf("test_rth_month_ending_on_weekday_unchanged\n");
    // October 2025 ends on a Friday: the pre-existing eager rule already
    // completed it at 15:45; the new rule must not double-complete.
    TimeframeAggregator agg("M", "15", NY, RTH);
    std::vector<int64_t> ts;
    rth_day(ts, 2025, 10, 30);
    rth_day(ts, 2025, 10, 31);
    rth_day(ts, 2025, 11, 3);
    auto c = drive(agg, ts);
    CHECK(has_completion_at(c, edt_ms(2025, 10, 31, 15, 45)));
    CHECK(c.size() == 1);
}

static void test_rth_daily_rule_unchanged() {
    std::printf("test_rth_daily_rule_unchanged\n");
    TimeframeAggregator agg("D", "15", NY, RTH);
    std::vector<int64_t> ts;
    rth_day(ts, 2025, 6, 5);
    rth_day(ts, 2025, 6, 6);
    rth_day(ts, 2025, 6, 9);
    auto c = drive(agg, ts);
    // The pre-existing eager DAY rule completes every session on its 15:45
    // last bar — all three fed days finalize.
    CHECK(c.size() == 3);
    CHECK(has_completion_at(c, edt_ms(2025, 6, 5, 15, 45)));
    CHECK(has_completion_at(c, edt_ms(2025, 6, 6, 15, 45)));
    CHECK(has_completion_at(c, edt_ms(2025, 6, 9, 15, 45)));
}

// ─── C: forex W / M ───────────────────────────────────────────────────────────

static void test_fx_week_completes_friday_1645() {
    std::printf("test_fx_week_completes_friday_1645\n");
    TimeframeAggregator agg("W", "15", NY, FX);
    std::vector<int64_t> ts;
    for (int d = 1; d <= 5; ++d) fx_session(ts, 2025, 6, d);  // Sun 06-01 17:00 .. Fri 06-06 16:45
    fx_session(ts, 2025, 6, 8);                                // Sun 06-08 17:00 ..
    auto c = drive(agg, ts);
    const Completion* w = completion_at(c, edt_ms(2025, 6, 6, 16, 45));
    CHECK(w != nullptr);
    if (w) { CHECK(w->subs == 480); CHECK_EQ_MS(w->bucket_ts, edt_ms(2025, 6, 1, 17, 0)); }
    // Not on an interior session close (Thu 16:45), not on Sunday 17:00.
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 5, 16, 45)));
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 8, 17, 0)));
    CHECK(c.size() == 1);
    CHECK_EQ_MS(agg.current().timestamp, edt_ms(2025, 6, 8, 17, 0));
}

static void test_fx_month_ending_on_weekend_completes_friday_1645() {
    std::printf("test_fx_month_ending_on_weekend_completes_friday_1645\n");
    // May 2025 ends on Saturday: the last traded session-day opens Thu 05-29
    // 17:00 and closes Fri 05-30 17:00 ET.
    TimeframeAggregator agg("M", "15", NY, FX);
    std::vector<int64_t> ts;
    for (int d = 25; d <= 29; ++d) fx_session(ts, 2025, 5, d);
    fx_session(ts, 2025, 6, 1);   // Sun 06-01 17:00: June
    auto c = drive(agg, ts);
    const Completion* m = completion_at(c, edt_ms(2025, 5, 30, 16, 45));
    CHECK(m != nullptr);
    if (m) CHECK_EQ_MS(m->bucket_ts, edt_ms(2025, 5, 25, 17, 0));
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 1, 17, 0)));
    CHECK(c.size() == 1);
}

static void test_fx_month_ending_on_weekday_unchanged() {
    std::printf("test_fx_month_ending_on_weekday_unchanged\n");
    // April 2025 ends on Wednesday: the session opening Tue 04-29 17:00 is
    // the 30th's trading date; the next bar (Wed 17:00) is May. The
    // next-bar crossing already completed it at Wed 16:45 — no double.
    TimeframeAggregator agg("M", "15", NY, FX);
    std::vector<int64_t> ts;
    fx_session(ts, 2025, 4, 28);
    fx_session(ts, 2025, 4, 29);
    fx_session(ts, 2025, 4, 30);
    auto c = drive(agg, ts);
    CHECK(has_completion_at(c, edt_ms(2025, 4, 30, 16, 45)));
    CHECK(c.size() == 1);
}

static void test_fx_daily_unchanged() {
    std::printf("test_fx_daily_unchanged\n");
    TimeframeAggregator agg("D", "15", NY, FX);
    std::vector<int64_t> ts;
    fx_session(ts, 2025, 6, 4);
    fx_session(ts, 2025, 6, 5);
    fx_session(ts, 2025, 6, 8);
    auto c = drive(agg, ts);
    // Every forex session finalizes on its 16:45 last bar (existing DAY rule).
    CHECK(c.size() == 3);
    CHECK(has_completion_at(c, edt_ms(2025, 6, 5, 16, 45)));
    CHECK(has_completion_at(c, edt_ms(2025, 6, 6, 16, 45)));
    CHECK(has_completion_at(c, edt_ms(2025, 6, 9, 16, 45)));  // Sun-open session closes Mon 16:45
}

static void test_fx_240_unchanged() {
    std::printf("test_fx_240_unchanged\n");
    // Forex 4h buckets are anchored 17:00 ET and always hold 16 sub-bars;
    // the session-close rule fires on the same bar as the count rule.
    TimeframeAggregator agg("240", "15", NY, FX);
    std::vector<int64_t> ts;
    fx_session(ts, 2025, 6, 5);
    fx_session(ts, 2025, 6, 8);
    auto c = drive(agg, ts);
    CHECK(c.size() == 12);
    for (const auto& x : c) CHECK(x.subs == 16);
    CHECK(has_completion_at(c, edt_ms(2025, 6, 6, 16, 45)));
    CHECK(!has_completion_at(c, edt_ms(2025, 6, 8, 17, 0)));
}

// ─── 24x7 identity ────────────────────────────────────────────────────────────

static void test_24x7_identity() {
    std::printf("test_24x7_identity\n");
    // UTC 15m grid Fri 2025-06-06 00:00 .. Mon 2025-06-09 04:00, with the
    // 23:45 bar of Friday missing so the 20:00-00:00 '240' bucket never
    // reaches its count.
    std::vector<int64_t> ts;
    for (int64_t t = utc_ms(2025, 6, 6); t < utc_ms(2025, 6, 9, 4, 0); t += k15m) {
        if (t == utc_ms(2025, 6, 6, 23, 45)) continue;
        ts.push_back(t);
    }
    const std::string forms[3][2] = {{"UTC", ""}, {"UTC", "24x7"}, {"", ""}};
    for (const auto& f : forms) {
        TimeframeAggregator w(std::string("W"), std::string("15"), f[0], f[1]);
        auto cw = drive(w, ts);
        // Projection rule: Sun 23:45 + 15m crosses into Monday.
        CHECK(cw.size() == 1);
        CHECK(has_completion_at(cw, utc_ms(2025, 6, 8, 23, 45)));
        CHECK(!has_completion_at(cw, utc_ms(2025, 6, 6, 23, 30)));

        TimeframeAggregator r(std::string("240"), std::string("15"), f[0], f[1]);
        auto cr = drive(r, ts);
        // The short bucket completes lazily on the next bucket's first bar.
        const Completion* lazy = completion_at(cr, utc_ms(2025, 6, 7, 0, 0));
        CHECK(lazy != nullptr);
        if (lazy) { CHECK(lazy->subs == 15); CHECK_EQ_MS(lazy->bucket_ts, utc_ms(2025, 6, 6, 20, 0)); }
        CHECK(!has_completion_at(cr, utc_ms(2025, 6, 6, 23, 30)));
    }
    // tz-less constructor: same completions as the UTC forms.
    TimeframeAggregator w0("W", "15");
    auto c0 = drive(w0, ts);
    CHECK(c0.size() == 1 && has_completion_at(c0, utc_ms(2025, 6, 8, 23, 45)));
    TimeframeAggregator r0("240", "15");
    auto cr0 = drive(r0, ts);
    CHECK(has_completion_at(cr0, utc_ms(2025, 6, 7, 0, 0)));
    CHECK(!has_completion_at(cr0, utc_ms(2025, 6, 6, 23, 30)));
}

int main() {
    test_last_traded_close_helper();
    test_rth_240_completes_at_1545();
    test_rth_60_and_45_complete_at_1545();
    test_rth_week_completes_friday_1545();
    test_rth_month_ending_on_weekend_completes_last_friday();
    test_rth_month_ending_on_weekday_unchanged();
    test_rth_daily_rule_unchanged();
    test_fx_week_completes_friday_1645();
    test_fx_month_ending_on_weekend_completes_friday_1645();
    test_fx_month_ending_on_weekday_unchanged();
    test_fx_daily_unchanged();
    test_fx_240_unchanged();
    test_24x7_identity();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
