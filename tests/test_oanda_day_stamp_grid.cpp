// The intraday request.security grid anchors at the symbol's DAY STAMP — the
// instant its daily bar starts — not at the first traded minute of its
// session. On OANDA that instant is the 17:00 ET forex roll for EVERY
// instrument: EURUSD (1700-1700) opens on it, XAUUSD (1800-1700) trades from
// one hour after it, yet TradingView aligns XAUUSD's "45" / "240" buckets at
// 17:00 ET all the same.
//
// Pinned with `lab tv` on OANDA:XAUUSD 15, 2025-04-01..04-15 (pin-xau-grid-240
// / -45 / -D: a strategy entering on ta.change(time("<tf>")) and closing the
// next bar, so every entry stamp is a boundary + 15 min; tapes in UTC+8):
//   "240": entries 09:15 / 13:15 / 17:15 / 21:15 (+8) = boundaries 01:00Z /
//          05:00Z / 09:00Z / 13:00Z, plus 17:00Z and the day's first traded
//          bar 22:00Z -> a 4h grid anchored at 21:00Z = 17:00 EDT (the
//          17:00-21:00 ET bucket holds only the 18:00-20:45 trading);
//   "45":  entries 09:00 / 09:45 / 10:30 / 11:15 (+8) = boundaries 00:45Z /
//          01:30Z / 02:15Z / 03:00Z -> 21:00Z + k*45m (an 18:00 ET anchor
//          would put them at 01:00Z / 01:45Z / 02:30Z);
//   "D":   the day's first bar is 22:00Z (18:00 ET), the daily bucket starts
//          at the 17:00 ET stamp and trading begins an hour later.
// Family evidence: every XAUUSD DCA probe (request.security "240"
// RSI gate) enters on the 17:00-ET grid on TV and on the 18:00-ET grid in the
// engine, the DCA legs agreeing to 1e-6 once the grid does.
//
// Rules pinned here:
//   A. "240" from 15m on NY 1800-1700 (EDT): buckets open 21:00Z / 01:00Z /
//      05:00Z / 09:00Z / 13:00Z / 17:00Z; the first holds the 12 bars
//      22:00Z-00:45Z and is labelled 21:00Z; tf_change agrees;
//   B. "45": buckets 21:00Z / 21:45Z / 22:30Z / 23:15Z / 00:00Z ...; the first
//      traded bar 22:00Z sits in the 21:45Z bucket;
//   C. the same in EST (17:00 EST = 22:00Z; first traded bar 23:00Z);
//   D. the 1800-1700 SESSION-DAY model: the D bar OPENS at the 17:00 ET stamp
//      (pin-time-hours, OANDA:XAUUSD 15, 2025-04-01..07-01: time("D") is
//      17:00 ET on 147/147 entries -- see test_pine_time_day_stamp_grid) and
//      closes at the next 17:00 ET; the week opens on the Sunday stamp and
//      the month on its first session-day's stamp (extrapolated from the D
//      pin: a period opens where its first D bar does, as 1700-1700 already
//      does -- no W/M tape); the day ordinal rolls between 16:45 and 18:00
//      and not at UTC midnight, and OANDA's 17:00-ET-stamped native daily
//      bars still route to the session they cover; the D bucket is labelled
//      by the stamp and completes on the session's last chart bar (16:45
//      ET), as it does on 1700-1700;
//   E. sessions whose open IS the stamp keep their grid: OANDA 1700-1700 (the
//      same grid as XAUUSD), CME 1700-1600 CT, NYSE 0930-1600, NSE 0915-1530,
//      24x7 UTC.
#include <cstdio>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/timeframe.hpp>

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

const std::string NY  = "America/New_York";
const std::string CHI = "America/Chicago";
const std::string IST = "Asia/Kolkata";
const std::string UTC = "UTC";
const std::string XAU = "1800-1700";          // OANDA metals / CFDs
const std::string FX  = "1700-1700";          // OANDA forex
const std::string CME = "1700-1600";          // CME Globex (Chicago)
const std::string RTH = "0930-1600";          // NYSE / NASDAQ
const std::string NSE = "0915-1530";          // NSE India
const int64_t k15m = 15 * 60 * 1000;
const int64_t k1h  = 60 * 60 * 1000;

Bar bar_at(int64_t ts) {
    Bar b;
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.0;
    b.volume = 1.0;
    return b;
}

struct Completion {
    int64_t at;         // input bar that finalized the bucket
    int64_t bucket_ts;  // label of the completed HTF bar
    int subs;
};

std::vector<Completion> drive(TimeframeAggregator& agg, const std::vector<int64_t>& ts) {
    std::vector<Completion> out;
    for (int64_t t : ts) {
        AggregatedBar r = agg.feed(bar_at(t));
        if (r.is_complete) out.push_back({t, r.bar.timestamp, r.sub_bar_count});
    }
    return out;
}

// One 1800-1700 session of 15m bars: `open_z` (18:00 ET) .. open_z + 22:45
// (16:45 ET), 92 bars; nothing trades in the 17:00-18:00 ET break.
void xau_session(std::vector<int64_t>& v, int64_t open_z) {
    for (int i = 0; i < 92; ++i) v.push_back(open_z + i * k15m);
}

// ─── A. "240" on OANDA:XAUUSD, EDT ────────────────────────────────────────────

void test_xau_240_grid_edt() {
    std::printf("A. XAUUSD '240' anchors at 17:00 EDT = 21:00Z\n");
    // Mon 2025-03-31 18:00 EDT = 22:00Z opens the Tuesday session; its day
    // stamp is 17:00 EDT = 21:00Z.
    const int64_t stamp = utc_ms(2025, 3, 31, 21, 0);
    const int64_t open  = utc_ms(2025, 3, 31, 22, 0);
    TimeframeAggregator agg("240", "15", NY, XAU);
    // Grid opens.
    CHECK_EQ_MS(agg.bucket_open_ms(open), stamp);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 0, 45)), stamp);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)), utc_ms(2025, 4, 1, 1, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 4, 45)), utc_ms(2025, 4, 1, 1, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 12, 15)), utc_ms(2025, 4, 1, 9, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 20, 45)), utc_ms(2025, 4, 1, 17, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 22, 0)), utc_ms(2025, 4, 1, 21, 0));
    // The label of the day's first bucket is the grid open, not the first
    // traded sub-bar (finding 473).
    CHECK_EQ_MS(agg.bar_label_ms(open), stamp);
    // tf_change fires on the pinned boundaries ...
    CHECK(tf_change(utc_ms(2025, 4, 1, 0, 45), utc_ms(2025, 4, 1, 1, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 4, 45), utc_ms(2025, 4, 1, 5, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 8, 45), utc_ms(2025, 4, 1, 9, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 12, 45), utc_ms(2025, 4, 1, 13, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 16, 45), utc_ms(2025, 4, 1, 17, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 20, 45), utc_ms(2025, 4, 1, 22, 0), "240", NY, XAU));
    // ... and not on the 18:00-ET grid (22:00Z / 02:00Z / ...).
    CHECK(!tf_change(utc_ms(2025, 4, 1, 1, 45), utc_ms(2025, 4, 1, 2, 0), "240", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 4, 1, 5, 45), utc_ms(2025, 4, 1, 6, 0), "240", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 4, 1, 1, 0), utc_ms(2025, 4, 1, 1, 15), "240", NY, XAU));
    // Driven over two sessions: six buckets per session, the 17:00-21:00 ET
    // bucket holding the 12 traded sub-bars (finalized on the 00:45Z bar whose
    // close reaches the bucket end, finding 467), the others 16.
    std::vector<int64_t> ts;
    xau_session(ts, open);
    xau_session(ts, utc_ms(2025, 4, 1, 22, 0));
    auto c = drive(agg, ts);
    CHECK(c.size() == 12);
    const int64_t want[12] = {
        utc_ms(2025, 3, 31, 21, 0), utc_ms(2025, 4, 1, 1, 0), utc_ms(2025, 4, 1, 5, 0),
        utc_ms(2025, 4, 1, 9, 0),   utc_ms(2025, 4, 1, 13, 0), utc_ms(2025, 4, 1, 17, 0),
        utc_ms(2025, 4, 1, 21, 0),  utc_ms(2025, 4, 2, 1, 0),  utc_ms(2025, 4, 2, 5, 0),
        utc_ms(2025, 4, 2, 9, 0),   utc_ms(2025, 4, 2, 13, 0), utc_ms(2025, 4, 2, 17, 0),
    };
    for (size_t i = 0; i < c.size() && i < 12; ++i) {
        CHECK_EQ_MS(c[i].bucket_ts, want[i]);
        CHECK(c[i].subs == (i % 6 == 0 ? 12 : 16));
        CHECK_EQ_MS(c[i].at, want[i] + 4 * k1h - k15m);   // bucket end - 15m
    }
}

// ─── B. "45" on OANDA:XAUUSD, EDT ─────────────────────────────────────────────

void test_xau_45_grid_edt() {
    std::printf("B. XAUUSD '45' grid 21:00Z / 21:45Z / 22:30Z / 23:15Z ...\n");
    const int64_t open = utc_ms(2025, 3, 31, 22, 0);
    TimeframeAggregator agg("45", "15", NY, XAU);
    CHECK_EQ_MS(agg.bucket_open_ms(open), utc_ms(2025, 3, 31, 21, 45));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 3, 31, 22, 15)), utc_ms(2025, 3, 31, 21, 45));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 3, 31, 22, 30)), utc_ms(2025, 3, 31, 22, 30));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 3, 31, 23, 15)), utc_ms(2025, 3, 31, 23, 15));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 0, 0)), utc_ms(2025, 4, 1, 0, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)), utc_ms(2025, 4, 1, 0, 45));
    CHECK_EQ_MS(agg.bar_label_ms(open), utc_ms(2025, 3, 31, 21, 45));
    CHECK(tf_change(utc_ms(2025, 3, 31, 22, 15), utc_ms(2025, 3, 31, 22, 30), "45", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 3, 31, 23, 0), utc_ms(2025, 3, 31, 23, 15), "45", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 3, 31, 23, 45), utc_ms(2025, 4, 1, 0, 0), "45", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 2, 0), utc_ms(2025, 4, 1, 2, 15), "45", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 2, 45), utc_ms(2025, 4, 1, 3, 0), "45", NY, XAU));
    // The 18:00-ET grid (22:00Z / 22:45Z / 23:30Z) is not a boundary.
    CHECK(!tf_change(utc_ms(2025, 3, 31, 22, 0), utc_ms(2025, 3, 31, 22, 15), "45", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 3, 31, 22, 30), utc_ms(2025, 3, 31, 22, 45), "45", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 3, 31, 23, 15), utc_ms(2025, 3, 31, 23, 30), "45", NY, XAU));
    // Driven over one session: 31 buckets (21:45Z .. 20:15Z next day), the
    // first holding two sub-bars (22:00Z, 22:15Z) and finalized on 22:15Z.
    std::vector<int64_t> ts;
    xau_session(ts, open);
    auto c = drive(agg, ts);
    CHECK(c.size() == 31);
    if (!c.empty()) {
        CHECK_EQ_MS(c.front().bucket_ts, utc_ms(2025, 3, 31, 21, 45));
        CHECK(c.front().subs == 2);
        CHECK_EQ_MS(c.front().at, utc_ms(2025, 3, 31, 22, 15));
        CHECK_EQ_MS(c.back().bucket_ts, utc_ms(2025, 4, 1, 20, 15));
        CHECK(c.back().subs == 3);
    }
    for (size_t i = 1; i < c.size(); ++i) {
        CHECK_EQ_MS(c[i].bucket_ts, utc_ms(2025, 3, 31, 21, 45) + static_cast<int64_t>(i) * 45 * 60000);
        CHECK(c[i].subs == 3);
    }
}

// ─── C. EST ───────────────────────────────────────────────────────────────────

void test_xau_240_grid_est() {
    std::printf("C. XAUUSD '240' in EST anchors at 22:00Z\n");
    // Mon 2025-01-13 18:00 EST = 23:00Z; stamp 17:00 EST = 22:00Z.
    const int64_t stamp = utc_ms(2025, 1, 13, 22, 0);
    const int64_t open  = utc_ms(2025, 1, 13, 23, 0);
    TimeframeAggregator agg("240", "15", NY, XAU);
    CHECK_EQ_MS(agg.bucket_open_ms(open), stamp);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 1, 14, 1, 45)), stamp);
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 1, 14, 2, 0)), utc_ms(2025, 1, 14, 2, 0));
    CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 1, 14, 21, 45)), utc_ms(2025, 1, 14, 18, 0));
    CHECK(tf_change(utc_ms(2025, 1, 14, 1, 45), utc_ms(2025, 1, 14, 2, 0), "240", NY, XAU));
    CHECK(tf_change(utc_ms(2025, 1, 14, 21, 45), utc_ms(2025, 1, 14, 23, 0), "240", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 1, 14, 2, 45), utc_ms(2025, 1, 14, 3, 0), "240", NY, XAU));
    TimeframeAggregator q("45", "15", NY, XAU);
    CHECK_EQ_MS(q.bucket_open_ms(open), utc_ms(2025, 1, 13, 22, 45));
    CHECK_EQ_MS(q.bucket_open_ms(utc_ms(2025, 1, 13, 23, 30)), utc_ms(2025, 1, 13, 23, 30));
}

// ─── D. The 1800-1700 session-day model is untouched ─────────────────────────

void test_xau_session_day_model_unchanged() {
    std::printf("D. 1800-1700 session-day anchors: D opens on the 17:00 ET stamp, completes on 16:45 ET\n");
    const int64_t tue_0300 = utc_ms(2025, 4, 1, 7, 0);   // Tue 03:00 EDT
    // The D bar opens at the stamp (pin-time-hours: time("D") == 17:00 ET),
    // an hour before the 18:00 ET session open, and closes at the next stamp.
    CHECK_EQ_MS(session_period_open_ms(tue_0300, NY, XAU, CalendarPeriod::DAY),
                utc_ms(2025, 3, 31, 21, 0));                // Mon 17:00 EDT
    CHECK_EQ_MS(session_period_close_ms(tue_0300, NY, XAU, CalendarPeriod::DAY),
                utc_ms(2025, 4, 1, 21, 0));                 // Tue 17:00 EDT
    // W / M open on their first session-day's stamp (extrapolated from the D
    // pin, the 1700-1700 relation; no W/M tape) and close on the next one's.
    CHECK_EQ_MS(session_period_open_ms(tue_0300, NY, XAU, CalendarPeriod::WEEK),
                utc_ms(2025, 3, 30, 21, 0));                // Sun 17:00 EDT
    CHECK_EQ_MS(session_period_close_ms(tue_0300, NY, XAU, CalendarPeriod::WEEK),
                utc_ms(2025, 4, 6, 21, 0));
    CHECK_EQ_MS(session_period_open_ms(tue_0300, NY, XAU, CalendarPeriod::MONTH),
                utc_ms(2025, 3, 31, 21, 0));                // April's first session
    CHECK_EQ_MS(session_period_close_ms(tue_0300, NY, XAU, CalendarPeriod::MONTH),
                utc_ms(2025, 4, 30, 21, 0));                // May 1 is a Thursday
    // The week's last traded close is still Friday 17:00 ET.
    CHECK_EQ_MS(session_period_last_traded_close_ms(tue_0300, NY, XAU, CalendarPeriod::WEEK),
                utc_ms(2025, 4, 4, 21, 0));
    // The day ordinal rolls between 16:45 and 18:00 ET, not at UTC midnight.
    CHECK(session_day_index(utc_ms(2025, 4, 1, 20, 45), NY, XAU)
          != session_day_index(utc_ms(2025, 4, 1, 22, 0), NY, XAU));
    CHECK(session_day_index(utc_ms(2025, 3, 31, 23, 45), NY, XAU)
          == session_day_index(utc_ms(2025, 4, 1, 0, 0), NY, XAU));
    CHECK(session_day_index(utc_ms(2025, 3, 31, 22, 0), NY, XAU)
          == session_day_index(utc_ms(2025, 4, 1, 20, 45), NY, XAU));
    CHECK(tf_change(utc_ms(2025, 4, 1, 20, 45), utc_ms(2025, 4, 1, 22, 0), "D", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 3, 31, 23, 45), utc_ms(2025, 4, 1, 0, 0), "D", NY, XAU));
    CHECK(!tf_change(utc_ms(2025, 3, 31, 22, 0), utc_ms(2025, 4, 1, 20, 45), "D", NY, XAU));
    // OANDA's native daily bars are stamped at the 17:00 ET break: the aux
    // routing key (engine_aux_security.cpp) still resolves them to the
    // session they cover, whether the stamp sits on the roll or inside the
    // break.
    for (int mi : {0, 30, 59}) {
        const int64_t stamp = utc_ms(2025, 3, 31, 21, mi);
        CHECK_EQ_MS(session_covered_instant_ms(stamp, NY, XAU), utc_ms(2025, 3, 31, 22, 0));
        CHECK_EQ_MS(session_period_open_ms(session_covered_instant_ms(stamp, NY, XAU),
                                           NY, XAU, CalendarPeriod::DAY),
                    utc_ms(2025, 3, 31, 21, 0));
        CHECK_EQ_MS(session_period_open_ms(session_covered_instant_ms(stamp, NY, XAU),
                                           NY, XAU, CalendarPeriod::WEEK),
                    utc_ms(2025, 3, 30, 21, 0));
        // The same key the 1m content bars of that session resolve to.
        CHECK_EQ_MS(session_period_open_ms(utc_ms(2025, 4, 1, 12, 0), NY, XAU, CalendarPeriod::DAY),
                    utc_ms(2025, 3, 31, 21, 0));
    }
    // A stamp past the close still rolls to the next session's open.
    CHECK_EQ_MS(session_covered_instant_ms(utc_ms(2025, 4, 1, 21, 0), NY, XAU),
                utc_ms(2025, 4, 1, 22, 0));
    CHECK_EQ_MS(session_covered_instant_ms(utc_ms(2025, 4, 1, 12, 0), NY, XAU),
                utc_ms(2025, 4, 1, 12, 0));
    // The D bucket is labelled by the 17:00 ET stamp (TradingView's D bar
    // time, which time("D") reads) and completes on the session's last
    // chart bar (16:45 ET, whose close is the next stamp), exactly as the
    // 1700-1700 forex day does — not a session late.
    TimeframeAggregator d("D", "15", NY, XAU);
    CHECK_EQ_MS(d.bar_label_ms(utc_ms(2025, 3, 31, 22, 0)), utc_ms(2025, 3, 31, 21, 0));
    CHECK_EQ_MS(d.bucket_open_ms(utc_ms(2025, 3, 31, 22, 0)), utc_ms(2025, 3, 31, 21, 0));
    std::vector<int64_t> ts;
    xau_session(ts, utc_ms(2025, 3, 31, 22, 0));
    xau_session(ts, utc_ms(2025, 4, 1, 22, 0));
    auto c = drive(d, ts);
    CHECK(c.size() == 2);
    if (c.size() == 2) {
        CHECK_EQ_MS(c[0].bucket_ts, utc_ms(2025, 3, 31, 21, 0));
        CHECK_EQ_MS(c[0].at, utc_ms(2025, 4, 1, 20, 45));
        CHECK(c[0].subs == 92);
        CHECK_EQ_MS(c[1].bucket_ts, utc_ms(2025, 4, 1, 21, 0));
        CHECK_EQ_MS(c[1].at, utc_ms(2025, 4, 2, 20, 45));
    }
    TimeframeAggregator dfx("D", "15", NY, FX);
    std::vector<int64_t> fx;
    for (int i = 0; i < 96; ++i) fx.push_back(utc_ms(2025, 3, 31, 21, 0) + i * k15m);
    auto cfx = drive(dfx, fx);
    CHECK(cfx.size() == 1);
    if (!cfx.empty()) CHECK_EQ_MS(cfx[0].at, utc_ms(2025, 4, 1, 20, 45));
    // Day-of-week suffix does not disturb the stamp.
    TimeframeAggregator w("240", "15", NY, "1800-1700:23456");
    CHECK_EQ_MS(w.bucket_open_ms(utc_ms(2025, 3, 31, 22, 0)), utc_ms(2025, 3, 31, 21, 0));
}

// ─── E. Sessions whose open is the stamp keep their grid ─────────────────────

void test_open_equals_stamp_grids_unchanged() {
    std::printf("E. OANDA 1700-1700 / CME 1700-1600 / NYSE / NSE / 24x7 grids unchanged\n");
    // OANDA forex: 17:00 EDT = 21:00Z anchors '240' and '45' — the very grid
    // XAUUSD now shares.
    {
        TimeframeAggregator fx("240", "15", NY, FX), xau("240", "15", NY, XAU);
        CHECK_EQ_MS(fx.bucket_open_ms(utc_ms(2025, 3, 31, 21, 0)), utc_ms(2025, 3, 31, 21, 0));
        CHECK_EQ_MS(fx.bucket_open_ms(utc_ms(2025, 4, 1, 0, 45)), utc_ms(2025, 3, 31, 21, 0));
        CHECK_EQ_MS(fx.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)), utc_ms(2025, 4, 1, 1, 0));
        CHECK(tf_change(utc_ms(2025, 4, 1, 0, 45), utc_ms(2025, 4, 1, 1, 0), "240", NY, FX));
        CHECK(!tf_change(utc_ms(2025, 4, 1, 1, 45), utc_ms(2025, 4, 1, 2, 0), "240", NY, FX));
        TimeframeAggregator fx45("45", "15", NY, FX), xau45("45", "15", NY, XAU);
        CHECK_EQ_MS(fx45.bucket_open_ms(utc_ms(2025, 3, 31, 22, 15)), utc_ms(2025, 3, 31, 21, 45));
        int same = 0;
        for (int64_t t = utc_ms(2025, 3, 30, 21, 0); t < utc_ms(2025, 4, 4, 21, 0); t += k15m) {
            if (fx.bucket_open_ms(t) == xau.bucket_open_ms(t)
                && fx45.bucket_open_ms(t) == xau45.bucket_open_ms(t)) ++same;
            CHECK(tf_change(t, t + k15m, "240", NY, FX) == tf_change(t, t + k15m, "240", NY, XAU));
        }
        CHECK(same == 5 * 96);
        // Winter: 17:00 EST = 22:00Z.
        CHECK_EQ_MS(fx.bucket_open_ms(utc_ms(2025, 1, 14, 1, 45)), utc_ms(2025, 1, 13, 22, 0));
    }
    // CME Globex: 17:00 CDT = 22:00Z anchors '240' (22:00Z / 02:00Z / ...).
    {
        TimeframeAggregator agg("240", "15", CHI, CME);
        CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 3, 31, 22, 0)), utc_ms(2025, 3, 31, 22, 0));
        CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 1, 45)), utc_ms(2025, 3, 31, 22, 0));
        CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 2, 0)), utc_ms(2025, 4, 1, 2, 0));
        CHECK_EQ_MS(agg.bucket_open_ms(utc_ms(2025, 4, 1, 20, 45)), utc_ms(2025, 4, 1, 18, 0));
        CHECK(tf_change(utc_ms(2025, 4, 1, 1, 45), utc_ms(2025, 4, 1, 2, 0), "240", CHI, CME));
        CHECK(!tf_change(utc_ms(2025, 4, 1, 0, 45), utc_ms(2025, 4, 1, 1, 0), "240", CHI, CME));
        TimeframeAggregator h("60", "15", CHI, CME);
        CHECK_EQ_MS(h.bucket_open_ms(utc_ms(2025, 3, 31, 22, 30)), utc_ms(2025, 3, 31, 22, 0));
        CHECK_EQ_MS(session_period_open_ms(utc_ms(2025, 4, 1, 7, 0), CHI, CME, CalendarPeriod::DAY),
                    utc_ms(2025, 3, 31, 22, 0));
        CHECK_EQ_MS(session_period_close_ms(utc_ms(2025, 4, 1, 7, 0), CHI, CME, CalendarPeriod::DAY),
                    utc_ms(2025, 4, 1, 21, 0));
    }
    // NYSE RTH: 09:30 EDT = 13:30Z anchors '60' / '240'.
    {
        TimeframeAggregator h("60", "15", NY, RTH), q("240", "15", NY, RTH);
        CHECK_EQ_MS(h.bucket_open_ms(utc_ms(2025, 4, 1, 14, 0)), utc_ms(2025, 4, 1, 13, 30));
        CHECK_EQ_MS(h.bucket_open_ms(utc_ms(2025, 4, 1, 13, 30)), utc_ms(2025, 4, 1, 13, 30));
        CHECK_EQ_MS(q.bucket_open_ms(utc_ms(2025, 4, 1, 17, 45)), utc_ms(2025, 4, 1, 17, 30));
        CHECK(tf_change(utc_ms(2025, 4, 1, 14, 15), utc_ms(2025, 4, 1, 14, 30), "60", NY, RTH));
        CHECK(!tf_change(utc_ms(2025, 4, 1, 13, 45), utc_ms(2025, 4, 1, 14, 0), "60", NY, RTH));
    }
    // NSE: 09:15 IST = 03:45Z anchors '60'.
    {
        TimeframeAggregator h("60", "15", IST, NSE);
        CHECK_EQ_MS(h.bucket_open_ms(utc_ms(2025, 4, 1, 4, 0)), utc_ms(2025, 4, 1, 3, 45));
        CHECK_EQ_MS(h.bucket_open_ms(utc_ms(2025, 4, 1, 4, 45)), utc_ms(2025, 4, 1, 4, 45));
        CHECK(tf_change(utc_ms(2025, 4, 1, 4, 30), utc_ms(2025, 4, 1, 4, 45), "60", IST, NSE));
        CHECK(!tf_change(utc_ms(2025, 4, 1, 3, 45), utc_ms(2025, 4, 1, 4, 0), "60", IST, NSE));
    }
    // 24x7 UTC: the epoch grid.
    {
        TimeframeAggregator q("240", "15", UTC, "24x7"), q0("240", "15");
        CHECK_EQ_MS(q.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)), utc_ms(2025, 4, 1, 0, 0));
        CHECK_EQ_MS(q.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)), q0.bucket_open_ms(utc_ms(2025, 4, 1, 1, 0)));
        CHECK(tf_change(utc_ms(2025, 4, 1, 3, 45), utc_ms(2025, 4, 1, 4, 0), "240", UTC, "24x7"));
        CHECK(!tf_change(utc_ms(2025, 4, 1, 0, 45), utc_ms(2025, 4, 1, 1, 0), "240", UTC, "24x7"));
    }
}

}  // namespace

int main() {
    std::printf("=== OANDA day-stamp intraday grid tests ===\n\n");
    test_xau_240_grid_edt();
    test_xau_45_grid_edt();
    test_xau_240_grid_est();
    test_xau_session_day_model_unchanged();
    test_open_equals_stamp_grids_unchanged();
    std::printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
