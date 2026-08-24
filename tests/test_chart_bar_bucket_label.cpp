// Aggregated bars are dated by the bucket they occupy on the symbol-clock
// grid, never by the first sub-bar that happened to trade in them
// (finding 473).
//
// OANDA's 1m EURUSD tape prints nothing for the first minutes of every
// 17:00 ET forex session (1263 of 1299 session-days open at 17:01-17:04),
// while TradingView's own 15m chart dates that bar 17:00. Aggregating the
// 15m chart from the 1m feed therefore labelled the bar 17:04, and every
// entry / exit booked on it missed exact closed-trade identity by four
// minutes with identical price and PnL.
//
// Rules pinned here:
//   A. a thin-open intraday bucket is labelled by its session-anchored grid
//      open (17:04 ET first sub-bar -> 17:00 ET bar), on 15m and on 240m;
//   B. gap-free 24x7 feeds are bit-identical to the first-sub-bar label,
//      whichever constructor built the aggregator;
//   C. a bucket whose grid-opening sub-bar is the ONLY one present keeps
//      that (already correct) label;
//   D. the label follows the exchange clock across a DST edge (EST 22:00Z
//      vs EDT 21:00Z session opens);
//   E. calendar buckets (D / W) are dated by the session OPEN of their first
//      traded session-day: the forex daily bar opening at 17:04 ET is the
//      17:00 ET bar, and a holiday-Monday equity week stays Tuesday's bar.
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

const std::string NY  = "America/New_York";
const std::string RTH = "0930-1600";
const std::string FX  = "1700-1700";
const int64_t k1m  = 60 * 1000;
const int64_t k15m = 15 * k1m;

Bar bar_at(int64_t ts, double px = 100.0) {
    Bar b;
    b.timestamp = ts;
    b.open = px; b.high = px + 1.0; b.low = px - 1.0; b.close = px + 0.5;
    b.volume = 1.0;
    return b;
}

// Feed `n` consecutive input bars of `step` ms starting at `from`; return the
// completed bars emitted along the way.
std::vector<AggregatedBar> feed_run(TimeframeAggregator& agg, int64_t from,
                                    int64_t step, int n, double px = 100.0) {
    std::vector<AggregatedBar> out;
    for (int i = 0; i < n; ++i) {
        AggregatedBar ab = agg.feed(bar_at(from + i * step, px + i));
        if (ab.is_complete) out.push_back(ab);
    }
    return out;
}

// A. Thin session open: the first present 1m bar is 17:04 ET (21:04Z EDT);
//    the 15m chart bar is the 17:00 ET bar (finalized on the 17:14 bar whose
//    end reaches the bucket end, finding 467), the following one 17:15 ET.
void test_thin_open_bucket_labelled_at_grid_start() {
    std::printf("A. thin-open 15m bucket -> grid start\n");
    // 2026-04-12 is a Sunday in EDT: session opens 17:00 ET = 21:00Z.
    const int64_t open_z = utc_ms(2026, 4, 12, 21, 0);
    TimeframeAggregator agg("15", "1", NY, FX);
    // 21:04 .. 21:14 (11 bars): the bucket completes on the 21:14 bar.
    std::vector<AggregatedBar> done = feed_run(agg, open_z + 4 * k1m, k1m, 11);
    CHECK(done.size() == 1);
    if (!done.empty()) {
        const AggregatedBar& first = done.front();
        CHECK_EQ_MS(first.bar.timestamp, open_z);        // 21:00Z, not 21:04Z
        CHECK(first.sub_bar_count == 11);
        CHECK(first.bar.open == 100.0);                  // OHLC from the 17:04 bar
        CHECK(first.bar.close == 110.5);                 // last present sub-bar
        CHECK(first.bar.high == 111.0);
        CHECK(first.bar.low == 99.0);
    }
    CHECK_EQ_MS(agg.last_completed().timestamp, open_z);
    // 21:15 opens the next bucket, labelled 21:15 (present), still open.
    AggregatedBar next = agg.feed(bar_at(open_z + k15m, 200.0));
    CHECK(!next.is_complete);
    CHECK_EQ_MS(next.bar.timestamp, open_z + k15m);
    CHECK_EQ_MS(agg.current().timestamp, open_z + k15m);
    // Fill it: 14 more bars complete it by count with the 21:15 label.
    done = feed_run(agg, open_z + k15m + k1m, k1m, 14, 201.0);
    CHECK(done.size() == 1);
    if (!done.empty()) {
        CHECK_EQ_MS(done.back().bar.timestamp, open_z + k15m);
        CHECK(done.back().sub_bar_count == 15);
    }

    // Same rule on the session-anchored 240m grid (17:00 / 21:00 / 01:00 ET):
    // 21:04Z .. 00:59Z completes on the 00:59Z bar, dated 21:00Z.
    TimeframeAggregator agg4h("240", "1", NY, FX);
    done = feed_run(agg4h, open_z + 4 * k1m, k1m, 236);
    CHECK(done.size() == 1);
    if (!done.empty()) {
        CHECK_EQ_MS(done.front().bar.timestamp, open_z);   // 17:00 ET bucket
        CHECK(done.front().sub_bar_count == 236);
    }
    AggregatedBar h4 = agg4h.feed(bar_at(utc_ms(2026, 4, 13, 1, 0)));
    CHECK(!h4.is_complete);
    CHECK_EQ_MS(agg4h.current().timestamp, utc_ms(2026, 4, 13, 1, 0));

    // A bucket whose gap is INSIDE the session (17:31 first present of the
    // 17:30 bucket, the finding's "21:31 variant") labels 17:30 too.
    TimeframeAggregator agg_mid("15", "1", NY, FX);
    done = feed_run(agg_mid, open_z + 31 * k1m, k1m, 14);   // 21:31 .. 21:44
    CHECK(done.size() == 1);
    if (!done.empty()) CHECK_EQ_MS(done.front().bar.timestamp, open_z + 30 * k1m);
    CHECK_EQ_MS(agg_mid.bar_label_ms(open_z + 31 * k1m), open_z + 30 * k1m);
}

// B. Gap-free 24x7 feed: labels are the first sub-bar's ts (== grid), and
//    the tz/session constructor is bit-identical to the tz-less one.
void test_gap_free_feed_identity() {
    std::printf("B. gap-free 24x7 identity\n");
    const int64_t from = utc_ms(2024, 1, 1, 0, 0);
    TimeframeAggregator plain("15", "1");
    TimeframeAggregator utc("15", "1", "UTC", "");
    TimeframeAggregator none("15", "1", "UTC", "24x7");
    std::vector<AggregatedBar> a = feed_run(plain, from, k1m, 6 * 60);
    std::vector<AggregatedBar> b = feed_run(utc, from, k1m, 6 * 60);
    std::vector<AggregatedBar> c = feed_run(none, from, k1m, 6 * 60);
    CHECK(a.size() == 24);
    CHECK(a.size() == b.size() && a.size() == c.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK_EQ_MS(a[i].bar.timestamp, from + static_cast<int64_t>(i) * k15m);
        CHECK_EQ_MS(b[i].bar.timestamp, a[i].bar.timestamp);
        CHECK_EQ_MS(c[i].bar.timestamp, a[i].bar.timestamp);
        CHECK(b[i].bar.open == a[i].bar.open && b[i].bar.close == a[i].bar.close);
        CHECK(b[i].sub_bar_count == a[i].sub_bar_count);
    }
    // Session symbol, gap-free RTH 1m tape: 09:30 ET bucket labelled 09:30.
    TimeframeAggregator rth("15", "1", NY, RTH);
    const int64_t rth_open = utc_ms(2026, 4, 13, 13, 30);   // Mon 09:30 EDT
    std::vector<AggregatedBar> r = feed_run(rth, rth_open, k1m, 45);
    CHECK(r.size() == 3);
    for (size_t i = 0; i < r.size(); ++i) {
        CHECK_EQ_MS(r[i].bar.timestamp, rth_open + static_cast<int64_t>(i) * k15m);
    }
    // bar_label_ms is the identity on a present grid-opening sub-bar.
    CHECK_EQ_MS(rth.bar_label_ms(rth_open), rth_open);
    CHECK_EQ_MS(utc.bar_label_ms(from + 3 * k15m), from + 3 * k15m);
}

// C. Only the grid-opening sub-bar is present: label unchanged (17:00).
void test_first_sub_bar_only_bucket() {
    std::printf("C. bucket holding only its first sub-bar\n");
    const int64_t open_z = utc_ms(2026, 4, 12, 21, 0);
    TimeframeAggregator agg("15", "1", NY, FX);
    AggregatedBar p = agg.feed(bar_at(open_z));
    CHECK(!p.is_complete);
    CHECK_EQ_MS(agg.current().timestamp, open_z);
    AggregatedBar done = agg.feed(bar_at(open_z + k15m + 2 * k1m));   // 21:17Z
    CHECK(done.is_complete);
    CHECK_EQ_MS(done.bar.timestamp, open_z);
    CHECK(done.sub_bar_count == 1);
    // ...and the bucket the 21:17Z bar opened is the 21:15Z bucket.
    CHECK_EQ_MS(agg.current().timestamp, open_z + k15m);
}

// D. DST: the label follows the exchange clock. Sunday 2026-03-01 opens at
//    17:00 EST = 22:00Z; Sunday 2026-03-08 (EDT begins) at 17:00 EDT = 21:00Z.
void test_dst_session_open_label() {
    std::printf("D. DST session opens\n");
    {
        const int64_t est_open = utc_ms(2026, 3, 1, 22, 0);
        TimeframeAggregator agg("15", "1", NY, FX);
        std::vector<AggregatedBar> done = feed_run(agg, est_open + 4 * k1m, k1m, 11);
        CHECK(done.size() == 1);
        if (!done.empty()) CHECK_EQ_MS(done.front().bar.timestamp, est_open);
        CHECK_EQ_MS(agg.bar_label_ms(est_open + 4 * k1m), est_open);
        // 21:04Z on the EST Sunday is 16:04 ET, the previous session's last
        // 15m bucket (16:00 ET): the grid is on the local clock.
        CHECK_EQ_MS(agg.bar_label_ms(utc_ms(2026, 3, 1, 21, 4)),
                    utc_ms(2026, 3, 1, 21, 0));
    }
    {
        const int64_t edt_open = utc_ms(2026, 3, 8, 21, 0);
        TimeframeAggregator agg("15", "1", NY, FX);
        std::vector<AggregatedBar> done = feed_run(agg, edt_open + 3 * k1m, k1m, 12);
        CHECK(done.size() == 1);
        if (!done.empty()) CHECK_EQ_MS(done.front().bar.timestamp, edt_open);
        CHECK_EQ_MS(agg.bar_label_ms(edt_open + 3 * k1m), edt_open);
        // 240m on the EDT Sunday: 17:00 ET bucket.
        TimeframeAggregator agg4h("240", "1", NY, FX);
        CHECK_EQ_MS(agg4h.bar_label_ms(edt_open + 4 * k1m), edt_open);
        CHECK_EQ_MS(agg4h.bar_label_ms(utc_ms(2026, 3, 9, 1, 7)),
                    utc_ms(2026, 3, 9, 1, 0));
    }
}

// E. Calendar buckets: session-day open of the first traded session-day.
void test_calendar_bucket_label() {
    std::printf("E. calendar (D / W) labels\n");
    {
        // Forex daily from 1m: tape starts 17:04 ET Sunday; the daily bar is
        // the 17:00 ET (21:00Z) bar, completing when Monday 17:00 ET arrives.
        const int64_t open_z = utc_ms(2026, 4, 12, 21, 0);
        TimeframeAggregator agg("D", "1", NY, FX);
        feed_run(agg, open_z + 4 * k1m, k1m, 60);
        CHECK_EQ_MS(agg.current().timestamp, open_z);
        AggregatedBar d = agg.feed(bar_at(utc_ms(2026, 4, 13, 21, 3)));
        CHECK(d.is_complete);
        CHECK_EQ_MS(d.bar.timestamp, open_z);
        CHECK(d.sub_bar_count == 60);
        // The next session-day, itself thin-open, is the Monday 21:00Z bar.
        CHECK_EQ_MS(agg.current().timestamp, utc_ms(2026, 4, 13, 21, 0));
    }
    {
        // Equity week from 15m whose Monday (2026-01-19, MLK day) never
        // trades: the week is dated Tuesday 09:30 ET (14:30Z EST), not the
        // nominal Monday.
        const int64_t tue_open = utc_ms(2026, 1, 20, 14, 30);
        TimeframeAggregator agg("W", "15", NY, RTH);
        feed_run(agg, tue_open, k15m, 26);                // Tuesday session
        CHECK_EQ_MS(agg.current().timestamp, tue_open);
        AggregatedBar w = agg.feed(bar_at(utc_ms(2026, 1, 26, 14, 30)));
        CHECK(w.is_complete);
        CHECK_EQ_MS(w.bar.timestamp, tue_open);
        // A thin-open Tuesday (first 15m bar at 09:45) is still dated 09:30.
        TimeframeAggregator thin("W", "15", NY, RTH);
        thin.feed(bar_at(tue_open + k15m));
        CHECK_EQ_MS(thin.current().timestamp, tue_open);
    }
    {
        // 24x7 UTC daily from 1m with the 00:00 bar present: unchanged.
        const int64_t day = utc_ms(2024, 1, 1, 0, 0);
        TimeframeAggregator agg("D", "1", "UTC", "");
        feed_run(agg, day, k1m, 30);
        CHECK_EQ_MS(agg.current().timestamp, day);
        TimeframeAggregator plain("D", "1");
        feed_run(plain, day + 7 * k1m, k1m, 3);            // tz-less, thin open
        CHECK_EQ_MS(plain.current().timestamp, day);     // UTC midnight
    }
}

}  // namespace

int main() {
    std::printf("test_chart_bar_bucket_label\n");
    test_thin_open_bucket_labelled_at_grid_start();
    test_gap_free_feed_identity();
    test_first_sub_bar_only_bucket();
    test_dst_session_open_label();
    test_calendar_bucket_label();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
