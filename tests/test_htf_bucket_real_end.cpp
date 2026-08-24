// request.security intraday HTF buckets finalize on the input bar whose END
// reaches the bucket's real end, regardless of how many sub-bars traded
// (finding 467). TradingView exposes an HTF bar on the chart bar whose close
// reaches the bucket close; the engine's feed_ratio_mode used to finalize a
// thin bucket only when the NEXT bucket's first input bar crossed the
// boundary, which in the finer-tf harness (60m/240m buckets fed by 1m bars
// under a 15m chart) lands inside the next chart bar — one chart bar late.
//
//   1. thin bucket whose last sub-bar is present: completes on that sub-bar;
//   2. bucket whose last sub-bar is missing: completes on the boundary bar,
//      and — in the engine's feed order (security aggregators before the
//      chart aggregator) — is still visible on the chart bar whose close is
//      the bucket end;
//   3. full buckets: the real-end rule fires on exactly the bar the count
//      rule fires on (24x7 / UTC / session forms bit-identical);
//   4. DST-transition buckets on the exchange clock.
#include <cstdio>
#include <map>
#include <set>
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

#define CHECK_EQ_I64(actual, expected)                                         \
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
const std::string FX  = "1700-1700";
const std::string RTH = "0930-1600";
const int64_t k1m  = 60 * 1000;
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

std::vector<Completion> drive(TimeframeAggregator& agg, const std::vector<int64_t>& ts) {
    std::vector<Completion> out;
    for (int64_t t : ts) {
        AggregatedBar r = agg.feed(bar_at(t));
        if (r.is_complete) out.push_back({t, r.bar.timestamp, r.sub_bar_count});
    }
    return out;
}

// Regular grid [from, to) at `step`, minus the instants in `skip`.
std::vector<int64_t> grid(int64_t from, int64_t to, int64_t step,
                          const std::set<int64_t>& skip = {}) {
    std::vector<int64_t> v;
    for (int64_t t = from; t < to; t += step) {
        if (skip.count(t)) continue;
        v.push_back(t);
    }
    return v;
}

const Completion* completion_at(const std::vector<Completion>& c, int64_t at) {
    for (const auto& x : c) if (x.at == at) return &x;
    return nullptr;
}

bool has_completion_at(const std::vector<Completion>& c, int64_t at) {
    return completion_at(c, at) != nullptr;
}

bool distinct_buckets(const std::vector<Completion>& c) {
    std::set<int64_t> seen;
    for (const auto& x : c) if (!seen.insert(x.bucket_ts).second) return false;
    return true;
}

}  // namespace

// ─── 1. thin bucket, last sub-bar present ─────────────────────────────────────

static void test_fx_thin_60_completes_on_last_present_minute() {
    std::printf("test_fx_thin_60_completes_on_last_present_minute\n");
    // OANDA:EURUSD, Christmas 2025 (EST): the 17:00-18:00 ET bucket is
    // 22:00-23:00Z; the feed lacks 22:00-22:03Z (56 of 60 minutes).
    std::set<int64_t> skip;
    for (int i = 0; i < 4; ++i) skip.insert(utc_ms(2025, 12, 25, 22, i));
    auto ts = grid(utc_ms(2025, 12, 25, 20, 0), utc_ms(2025, 12, 26, 2, 30), k1m, skip);

    TimeframeAggregator agg("60", "1", NY, FX);
    auto c = drive(agg, ts);
    // Full buckets before and after complete by count on their :59 bar.
    const Completion* full = completion_at(c, utc_ms(2025, 12, 25, 21, 59));
    CHECK(full != nullptr);
    if (full) { CHECK(full->subs == 60); CHECK_EQ_I64(full->bucket_ts, utc_ms(2025, 12, 25, 21, 0)); }
    // The thin bucket completes on 22:59Z — its last present minute, whose
    // end is the bucket end — not on the 23:00Z boundary bar. It is dated by
    // its grid open (22:00Z), not by its first present minute (finding 473).
    const Completion* thin = completion_at(c, utc_ms(2025, 12, 25, 22, 59));
    CHECK(thin != nullptr);
    if (thin) { CHECK(thin->subs == 56); CHECK_EQ_I64(thin->bucket_ts, utc_ms(2025, 12, 25, 22, 0)); }
    CHECK(!has_completion_at(c, utc_ms(2025, 12, 25, 23, 0)));
    CHECK(has_completion_at(c, utc_ms(2025, 12, 25, 23, 59)));
    // 20:59, 21:59, 22:59, 23:59, 00:59, 01:59 — one completion per bucket.
    CHECK(c.size() == 6);
    CHECK(distinct_buckets(c));

    // '240' on the same feed: buckets anchor on the 17:00 ET session open
    // (22:00Z). The partial leading bucket (13:00-17:00 ET, fed from 20:00Z,
    // dated by its 18:00Z grid open) completes on 21:59Z with 120 sub-bars;
    // the thin 17:00-21:00 ET bucket (236 of 240, dated 22:00Z) completes on
    // 01:59Z, not on 02:00Z.
    TimeframeAggregator agg4("240", "1", NY, FX);
    auto c4 = drive(agg4, ts);
    const Completion* lead = completion_at(c4, utc_ms(2025, 12, 25, 21, 59));
    CHECK(lead != nullptr);
    if (lead) { CHECK(lead->subs == 120); CHECK_EQ_I64(lead->bucket_ts, utc_ms(2025, 12, 25, 18, 0)); }
    const Completion* thin4 = completion_at(c4, utc_ms(2025, 12, 26, 1, 59));
    CHECK(thin4 != nullptr);
    if (thin4) { CHECK(thin4->subs == 236); CHECK_EQ_I64(thin4->bucket_ts, utc_ms(2025, 12, 25, 22, 0)); }
    CHECK(!has_completion_at(c4, utc_ms(2025, 12, 25, 23, 0)));
    CHECK(!has_completion_at(c4, utc_ms(2025, 12, 26, 2, 0)));
    CHECK(c4.size() == 2);
}

static void test_utc_thin_bucket_last_minute_present() {
    std::printf("test_utc_thin_bucket_last_minute_present\n");
    // 24x7 / UTC forms: a gap inside the bucket (20:15 missing) with the
    // last sub-bar present completes on the last sub-bar (23:45), not lazily
    // on 00:00. The tz-less constructor behaves identically.
    std::set<int64_t> skip{utc_ms(2025, 6, 6, 20, 15)};
    auto ts = grid(utc_ms(2025, 6, 6, 16, 0), utc_ms(2025, 6, 7, 4, 15), k15m, skip);
    const std::string forms[3][2] = {{"UTC", ""}, {"UTC", "24x7"}, {"", ""}};
    for (const auto& f : forms) {
        TimeframeAggregator r(std::string("240"), std::string("15"), f[0], f[1]);
        auto c = drive(r, ts);
        const Completion* thin = completion_at(c, utc_ms(2025, 6, 6, 23, 45));
        CHECK(thin != nullptr);
        if (thin) { CHECK(thin->subs == 15); CHECK_EQ_I64(thin->bucket_ts, utc_ms(2025, 6, 6, 20, 0)); }
        CHECK(!has_completion_at(c, utc_ms(2025, 6, 7, 0, 0)));
        CHECK(has_completion_at(c, utc_ms(2025, 6, 6, 19, 45)));
        CHECK(has_completion_at(c, utc_ms(2025, 6, 7, 3, 45)));
        CHECK(c.size() == 3);
    }
    TimeframeAggregator r0("240", "15");
    auto c0 = drive(r0, ts);
    CHECK(has_completion_at(c0, utc_ms(2025, 6, 6, 23, 45)));
    CHECK(!has_completion_at(c0, utc_ms(2025, 6, 7, 0, 0)));
    CHECK(c0.size() == 3);
}

// ─── 2. last sub-bar missing ──────────────────────────────────────────────────

// Mirror of BacktestEngine::run_aggregation_bar_loop's per-input-bar order:
// every request.security aggregator is fed BEFORE the chart aggregator, so a
// bucket that can only finalize on the boundary bar is already committed when
// that same boundary bar completes the (equally thin) chart bar.
struct ChartView {
    int64_t chart_bucket_ts;
    int64_t sec_last_completed_bucket_ts;   // -1: none yet
};

static std::vector<ChartView> drive_engine_order(TimeframeAggregator& sec,
                                                 TimeframeAggregator& chart,
                                                 const std::vector<int64_t>& ts) {
    std::vector<ChartView> out;
    int64_t sec_done = -1;
    for (int64_t t : ts) {
        AggregatedBar s = sec.feed(bar_at(t));
        if (s.is_complete) sec_done = s.bar.timestamp;
        AggregatedBar c = chart.feed(bar_at(t));
        if (c.is_complete) out.push_back({c.bar.timestamp, sec_done});
    }
    return out;
}

static const ChartView* chart_view(const std::vector<ChartView>& v, int64_t chart_ts) {
    for (const auto& x : v) if (x.chart_bucket_ts == chart_ts) return &x;
    return nullptr;
}

static void test_last_minute_missing_completes_on_boundary_before_chart_close() {
    std::printf("test_last_minute_missing_completes_on_boundary_before_chart_close\n");
    // Same Christmas bucket, but now 22:57-22:59Z are the missing minutes:
    // no present sub-bar ends at 23:00Z, so the bucket completes on the
    // 23:00Z boundary bar (57 sub-bars).
    std::set<int64_t> skip;
    for (int i = 57; i < 60; ++i) skip.insert(utc_ms(2025, 12, 25, 22, i));
    auto ts = grid(utc_ms(2025, 12, 25, 20, 0), utc_ms(2025, 12, 26, 0, 30), k1m, skip);
    {
        TimeframeAggregator agg("60", "1", NY, FX);
        auto c = drive(agg, ts);
        CHECK(!has_completion_at(c, utc_ms(2025, 12, 25, 22, 56)));
        const Completion* lazy = completion_at(c, utc_ms(2025, 12, 25, 23, 0));
        CHECK(lazy != nullptr);
        if (lazy) { CHECK(lazy->subs == 57); CHECK_EQ_I64(lazy->bucket_ts, utc_ms(2025, 12, 25, 22, 0)); }
        CHECK(c.size() == 4);
        CHECK(distinct_buckets(c));
    }
    // Engine feed order: the 22:45Z chart bar (12 of 15 minutes) is also
    // thin and also completes on the 23:00Z bar — after the security
    // aggregator was fed that bar — so it sees the 22:00Z HTF bucket.
    {
        TimeframeAggregator sec("60", "1", NY, FX);
        TimeframeAggregator chart("15", "1", NY, FX);
        auto v = drive_engine_order(sec, chart, ts);
        const ChartView* cv = chart_view(v, utc_ms(2025, 12, 25, 22, 45));
        CHECK(cv != nullptr);
        if (cv) CHECK_EQ_I64(cv->sec_last_completed_bucket_ts, utc_ms(2025, 12, 25, 22, 0));
        // The preceding chart bar (22:30Z, full) still sees only the 21:00Z bucket.
        const ChartView* prev = chart_view(v, utc_ms(2025, 12, 25, 22, 30));
        CHECK(prev != nullptr);
        if (prev) CHECK_EQ_I64(prev->sec_last_completed_bucket_ts, utc_ms(2025, 12, 25, 21, 0));
    }
    // And with the last minute PRESENT (finding 467's shape: 22:00-22:03Z
    // missing) the 22:45Z chart bar — full, completing on 22:59Z by count —
    // sees the thin bucket through the real-end rule.
    {
        std::set<int64_t> skip2;
        for (int i = 0; i < 4; ++i) skip2.insert(utc_ms(2025, 12, 25, 22, i));
        auto ts2 = grid(utc_ms(2025, 12, 25, 20, 0), utc_ms(2025, 12, 26, 0, 30), k1m, skip2);
        TimeframeAggregator sec("60", "1", NY, FX);
        TimeframeAggregator chart("15", "1", NY, FX);
        auto v = drive_engine_order(sec, chart, ts2);
        const ChartView* cv = chart_view(v, utc_ms(2025, 12, 25, 22, 45));
        CHECK(cv != nullptr);
        if (cv) CHECK_EQ_I64(cv->sec_last_completed_bucket_ts, utc_ms(2025, 12, 25, 22, 0));
        const ChartView* next = chart_view(v, utc_ms(2025, 12, 25, 23, 0));
        CHECK(next != nullptr);
        if (next) CHECK_EQ_I64(next->sec_last_completed_bucket_ts, utc_ms(2025, 12, 25, 22, 0));
    }
}

// ─── 3. full-bucket identity ──────────────────────────────────────────────────

static void check_count_identity(const std::vector<Completion>& c, int ratio,
                                 int64_t input_ms, int64_t target_ms,
                                 size_t expected, int64_t grid_anchor_ms) {
    CHECK(c.size() == expected);
    CHECK(distinct_buckets(c));
    for (const auto& x : c) {
        // Every completion is a full bucket, finalized on its last sub-bar:
        // the bar's end sits on the target grid and is `ratio` bars past
        // the bucket's first sub-bar.
        CHECK(x.subs == ratio);
        CHECK(((x.at + input_ms) - grid_anchor_ms) % target_ms == 0);
        CHECK_EQ_I64(x.at + input_ms, x.bucket_ts + target_ms);
    }
}

static void test_full_bucket_identity() {
    std::printf("test_full_bucket_identity\n");
    // 24x7 / UTC, 1m -> '60' and 15m -> '240', gap-free: every bucket
    // completes by count on its last sub-bar; the real-end rule fires on the
    // same bar and nothing completes on a boundary bar.
    {
        auto ts = grid(utc_ms(2025, 6, 6, 0, 0), utc_ms(2025, 6, 7, 6, 0), k1m);
        TimeframeAggregator agg("60", "1", "UTC", "");
        auto c = drive(agg, ts);
        check_count_identity(c, 60, k1m, 3600000, 30, 0);
    }
    {
        auto ts = grid(utc_ms(2025, 6, 6, 0, 0), utc_ms(2025, 6, 9, 0, 0), k15m);
        TimeframeAggregator agg("240", "15", "UTC", "24x7");
        auto c = drive(agg, ts);
        check_count_identity(c, 16, k15m, 14400000, 18, 0);
        TimeframeAggregator agg0("240", "15");
        auto c0 = drive(agg0, ts);
        check_count_identity(c0, 16, k15m, 14400000, 18, 0);
    }
    // Forex (1700-1700, EST in December): '60' and '240' from 1m over two
    // full sessions; the 17:00 ET anchor puts the grid on 22:00Z.
    {
        auto ts = grid(utc_ms(2025, 12, 22, 22, 0), utc_ms(2025, 12, 24, 22, 0), k1m);
        TimeframeAggregator h("60", "1", NY, FX);
        auto ch = drive(h, ts);
        check_count_identity(ch, 60, k1m, 3600000, 48, utc_ms(2025, 12, 22, 22, 0));
        TimeframeAggregator q("240", "1", NY, FX);
        auto cq = drive(q, ts);
        check_count_identity(cq, 240, k1m, 14400000, 12, utc_ms(2025, 12, 22, 22, 0));
    }
    // Equities RTH (EDT): '60' from 15m over two sessions. Six full buckets
    // per session complete by count; the 15:30-16:30 bucket is clipped by
    // the session-close rule (2 sub-bars) on 15:45 — unchanged from before.
    {
        std::vector<int64_t> ts;
        for (int d = 2; d <= 3; ++d)
            for (int i = 0; i < 26; ++i) ts.push_back(utc_ms(2025, 6, d, 13, 30) + i * k15m);
        TimeframeAggregator agg("60", "15", NY, RTH);
        auto c = drive(agg, ts);
        CHECK(c.size() == 14);
        CHECK(distinct_buckets(c));
        int full = 0, clipped = 0;
        for (const auto& x : c) {
            if (x.subs == 4) ++full;
            else if (x.subs == 2) { ++clipped; CHECK_EQ_I64(x.at, x.bucket_ts + k15m); }
        }
        CHECK(full == 12);
        CHECK(clipped == 2);
        CHECK(!has_completion_at(c, utc_ms(2025, 6, 3, 13, 30)));
    }
}

// ─── 4. DST-transition buckets ────────────────────────────────────────────────

static void test_dst_fx_buckets() {
    std::printf("test_dst_fx_buckets\n");
    // Spring forward, Sun 2025-03-09 02:00 ET. Friday's session (opened Thu
    // 17:00 EST = 22:00Z) closes Fri 17:00 EST = 22:00Z; Sunday's opens 17:00
    // EDT = 21:00Z. 15m -> '60'.
    {
        std::vector<int64_t> ts;
        for (auto t : grid(utc_ms(2025, 3, 6, 22, 0), utc_ms(2025, 3, 7, 22, 0), k15m)) ts.push_back(t);
        for (auto t : grid(utc_ms(2025, 3, 9, 21, 0), utc_ms(2025, 3, 10, 21, 0), k15m)) ts.push_back(t);
        TimeframeAggregator agg("60", "15", NY, FX);
        auto c = drive(agg, ts);
        CHECK(c.size() == 48);
        CHECK(distinct_buckets(c));
        for (const auto& x : c) CHECK(x.subs == 4);
        // Friday's last bucket (16:00-17:00 EST) completes on the 16:45 bar.
        const Completion* fri = completion_at(c, utc_ms(2025, 3, 7, 21, 45));
        CHECK(fri != nullptr);
        if (fri) CHECK_EQ_I64(fri->bucket_ts, utc_ms(2025, 3, 7, 21, 0));
        // Sunday's first bucket (17:00-18:00 EDT) completes on 17:45 EDT.
        CHECK(!has_completion_at(c, utc_ms(2025, 3, 9, 21, 0)));
        const Completion* sun = completion_at(c, utc_ms(2025, 3, 9, 21, 45));
        CHECK(sun != nullptr);
        if (sun) CHECK_EQ_I64(sun->bucket_ts, utc_ms(2025, 3, 9, 21, 0));
    }
    // Fall back, Sun 2025-11-02 02:00 ET. Friday's session closes 17:00 EDT
    // = 21:00Z; Sunday's opens 17:00 EST = 22:00Z. 1m -> '240'.
    {
        std::vector<int64_t> ts;
        for (auto t : grid(utc_ms(2025, 10, 30, 21, 0), utc_ms(2025, 10, 31, 21, 0), k1m)) ts.push_back(t);
        for (auto t : grid(utc_ms(2025, 11, 2, 22, 0), utc_ms(2025, 11, 3, 22, 0), k1m)) ts.push_back(t);
        TimeframeAggregator agg("240", "1", NY, FX);
        auto c = drive(agg, ts);
        CHECK(c.size() == 12);
        CHECK(distinct_buckets(c));
        for (const auto& x : c) CHECK(x.subs == 240);
        const Completion* fri = completion_at(c, utc_ms(2025, 10, 31, 20, 59));
        CHECK(fri != nullptr);
        if (fri) CHECK_EQ_I64(fri->bucket_ts, utc_ms(2025, 10, 31, 17, 0));
        CHECK(!has_completion_at(c, utc_ms(2025, 11, 2, 22, 0)));
        const Completion* sun = completion_at(c, utc_ms(2025, 11, 3, 1, 59));
        CHECK(sun != nullptr);
        if (sun) CHECK_EQ_I64(sun->bucket_ts, utc_ms(2025, 11, 2, 22, 0));
    }
}

static void test_dst_24x7_exchange_clock_no_double_completion() {
    std::printf("test_dst_24x7_exchange_clock_no_double_completion\n");
    // A 24x7 feed keyed on the New York clock ('240' from 15m) across both
    // transitions. The aggregator's zone offset is resolved per UTC day, so
    // the local clock steps at 00:00Z of the transition Sunday:
    //  - spring forward: the 16:00-20:00 EST bucket loses its last hour and
    //    completes lazily on the 00:00Z boundary bar with 12 sub-bars;
    //  - fall back: the 16:00-20:00 EDT bucket completes by count on 23:45Z
    //    and the repeated hour (00:00-00:45Z) merges silently — the real-end
    //    rule must not finalize that bucket a second time.
    // Every other bucket is full, and no bucket ever completes twice. Both
    // feeds start on a local 4h grid line (Sat 01:00Z = 20:00 EST; Sat
    // 00:00Z = 20:00 EDT) so there is no partial leading bucket.
    {
        auto ts = grid(utc_ms(2025, 3, 8, 1, 0), utc_ms(2025, 3, 10, 12, 0), k15m);
        TimeframeAggregator agg("240", "15", NY, "24x7");
        auto c = drive(agg, ts);
        CHECK(distinct_buckets(c));
        const Completion* lazy = completion_at(c, utc_ms(2025, 3, 9, 0, 0));
        CHECK(lazy != nullptr);
        if (lazy) { CHECK(lazy->subs == 12); CHECK_EQ_I64(lazy->bucket_ts, utc_ms(2025, 3, 8, 21, 0)); }
        for (const auto& x : c) if (x.at != utc_ms(2025, 3, 9, 0, 0)) CHECK(x.subs == 16);
        CHECK(!has_completion_at(c, utc_ms(2025, 3, 8, 23, 45)));
    }
    {
        auto ts = grid(utc_ms(2025, 11, 1, 0, 0), utc_ms(2025, 11, 3, 12, 0), k15m);
        TimeframeAggregator agg("240", "15", NY, "24x7");
        auto c = drive(agg, ts);
        CHECK(distinct_buckets(c));
        for (const auto& x : c) CHECK(x.subs == 16);
        const Completion* sat = completion_at(c, utc_ms(2025, 11, 1, 23, 45));
        CHECK(sat != nullptr);
        if (sat) CHECK_EQ_I64(sat->bucket_ts, utc_ms(2025, 11, 1, 20, 0));
        for (int mi = 0; mi < 60; mi += 15) CHECK(!has_completion_at(c, utc_ms(2025, 11, 2, 0, mi)));
        CHECK(!has_completion_at(c, utc_ms(2025, 11, 2, 1, 0)));
        CHECK(has_completion_at(c, utc_ms(2025, 11, 2, 4, 45)));
    }
}

int main() {
    test_fx_thin_60_completes_on_last_present_minute();
    test_utc_thin_bucket_last_minute_present();
    test_last_minute_missing_completes_on_boundary_before_chart_close();
    test_full_bucket_identity();
    test_dst_fx_buckets();
    test_dst_24x7_exchange_clock_no_double_completion();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
