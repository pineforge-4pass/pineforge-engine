// request.security intraday HTF buckets complete on the calling CHART bar
// whose close reaches the bucket end, and on an exchange session's last chart
// bar at an early close (round 8, family P: masayanfx multi-time-score
// strategy on the NYSE:F / CME_MINI:ES1! / NQ1! 15m lanes; lab tv
// famp-sense-f15full / es15full / nq15full vs the engine's @pf-trace,
// 2026-09-05).
//
// On the split-feed path (a 15m chart whose request.security evaluators are
// fed the lane's 1m auxiliary slice) the aggregator's count / real-end /
// session-close rules all key on the 1m bars. When a chart bar's slice does
// not reach its last minute (NYSE:F prints no 18:14Z minute on 2025-04-17: the
// slice ends 18:13Z) the "15" bucket of that chart bar, and a "60" / "240"
// bucket ending there, completed only when the NEXT chart bar's first minute
// crossed the boundary -- one chart bar after TradingView, whose chart bar
// closes at the bucket end regardless (the engine read highest(20)[1] over
// the window ending one bar earlier on 20 "15", 6 "60" and 5 "240" bars of
// F@15; every lagged bar's own slice lacked its final minute). On CME the
// same shape is the early close: the 240 bucket left open at the 12:00 CT
// Independence-Day / Juneteenth / Black-Friday / Christmas-Eve close, or the
// 12:15 CT Good-Friday close, read one session late.
//
// Rules pinned here (TimeframeAggregator::feed_ratio_mode):
//   1. chart-close completion: the input bar is the bucket's last (the next
//      input bar lies beyond the bucket) and the calling chart bar's nominal
//      close reaches the bucket end -> the bucket completes on that input bar,
//      on any session kind; without a calling close (single-feed runs pass 0)
//      the bucket waits for the boundary bar exactly as before;
//   2. early-close completion: on an exchange calendar (early_close_completes)
//      the next input bar opening a later session-day completes the bucket the
//      session leaves open; an OTC quote stream keeps waiting.
//   3. end to end: on the split feed the "60" bucket ending at a chart bar
//      whose slice lacks its last minute is published before that chart
//      bar's body runs.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/timeframe.hpp>
#include <pineforge/bar.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

constexpr int64_t kMinute = 60'000;
const std::string NY = "America/New_York";
const std::string RTH = "0930-1600";

Bar bar_at(int64_t ts, double px) { return Bar{px, px + 0.5, px - 0.5, px, 1.0, ts}; }

// One-minute bars from `from` (inclusive) to `to` (exclusive), skipping the
// minutes listed in `skip`.
std::vector<Bar> minutes(int64_t from, int64_t to, const std::vector<int64_t>& skip = {}) {
    std::vector<Bar> out;
    for (int64_t t = from; t < to; t += kMinute) {
        bool skipped = false;
        for (int64_t s : skip) if (s == t) skipped = true;
        if (!skipped) out.push_back(bar_at(t, 10.0 + static_cast<double>((t / kMinute) % 97) * 0.01));
    }
    return out;
}

// Feed the 1m bars through `agg`, handing each the next bar's timestamp
// (`tail_next` after the last one) and, when `chart_seconds` > 0, the
// calling chart bar's nominal close (the 15m grid bar holding the minute,
// plus 15 minutes). Returns the timestamps of the input bars on which a
// bucket completed, with the completed bucket's label.
std::vector<std::pair<int64_t, int64_t>> drive(TimeframeAggregator& agg,
                                               const std::vector<Bar>& bars,
                                               int64_t tail_next,
                                               int chart_seconds) {
    std::vector<std::pair<int64_t, int64_t>> completions;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const int64_t next = (i + 1 < bars.size()) ? bars[i + 1].timestamp : tail_next;
        int64_t calling_close = 0;
        if (chart_seconds > 0) {
            const int64_t grid = static_cast<int64_t>(chart_seconds) * 1000;
            calling_close = (bars[i].timestamp / grid) * grid + grid;
        }
        AggregatedBar ab = agg.feed(bars[i], next, calling_close);
        if (ab.is_complete) completions.emplace_back(bars[i].timestamp, ab.bar.timestamp);
    }
    return completions;
}

bool completed_on(const std::vector<std::pair<int64_t, int64_t>>& c,
                  int64_t bucket, int64_t input_ts) {
    for (const auto& p : c) if (p.second == bucket && p.first == input_ts) return true;
    return false;
}

int completions_of(const std::vector<std::pair<int64_t, int64_t>>& c, int64_t bucket) {
    int n = 0;
    for (const auto& p : c) if (p.second == bucket) ++n;
    return n;
}

// 1. NYSE:F 2025-05-14: the 12:30-13:30 ET hour (16:30-17:30Z) whose slice
//    lacks the 17:29Z minute. TradingView finalizes it on the 17:15Z chart
//    bar (its close 17:30Z is the bucket end); with the calling close the
//    aggregator completes it on the 17:28Z minute, the slice's last; without
//    it (single-feed shape) the bucket waits for the 17:30Z boundary bar.
void test_chart_close_completes_thin_hour() {
    std::printf("1. chart-close completion of a '60' bucket whose slice lacks its last minute\n");
    const int64_t h0 = utc_ms(2025, 5, 14, 16, 30);
    const int64_t h1 = utc_ms(2025, 5, 14, 17, 30);
    const std::vector<Bar> bars = minutes(utc_ms(2025, 5, 14, 15, 30), utc_ms(2025, 5, 14, 18, 30),
                                          {utc_ms(2025, 5, 14, 17, 29)});
    {
        TimeframeAggregator h60("60", "1", NY, RTH);
        auto c = drive(h60, bars, utc_ms(2025, 5, 15, 13, 30), 900);
        CHECK(completed_on(c, h0, utc_ms(2025, 5, 14, 17, 28)));
        CHECK(completions_of(c, h0) == 1);
        // A dense bucket keeps its real-end completion on its last minute.
        CHECK(completed_on(c, utc_ms(2025, 5, 14, 15, 30), utc_ms(2025, 5, 14, 16, 29)));
        CHECK(completions_of(c, utc_ms(2025, 5, 14, 15, 30)) == 1);
        // The following hour is untouched: its first minute opens it, its
        // last minute completes it.
        CHECK(completed_on(c, h1, utc_ms(2025, 5, 14, 18, 29)));
        CHECK(completions_of(c, h1) == 1);
    }
    {
        TimeframeAggregator h60("60", "1", NY, RTH);
        auto c = drive(h60, bars, utc_ms(2025, 5, 15, 13, 30), 0);
        CHECK(!completed_on(c, h0, utc_ms(2025, 5, 14, 17, 28)));
        CHECK(completed_on(c, h0, utc_ms(2025, 5, 14, 17, 30)));
        CHECK(completions_of(c, h0) == 1);
    }
    // The chart timeframe's own bucket ("15" on a 15m chart): the 18:00Z
    // chart bar's slice ends 18:13Z; TradingView's same-timeframe series is
    // the chart bar itself, closed at 18:15Z.
    {
        const int64_t q = utc_ms(2025, 4, 17, 18, 0);
        const std::vector<Bar> slice = minutes(utc_ms(2025, 4, 17, 17, 45), utc_ms(2025, 4, 17, 18, 30),
                                               {utc_ms(2025, 4, 17, 18, 14)});
        TimeframeAggregator q15("15", "1", NY, RTH);
        auto c = drive(q15, slice, utc_ms(2025, 4, 17, 18, 30), 900);
        CHECK(completed_on(c, q, utc_ms(2025, 4, 17, 18, 13)));
        CHECK(completions_of(c, q) == 1);
        CHECK(completed_on(c, utc_ms(2025, 4, 17, 17, 45), utc_ms(2025, 4, 17, 17, 59)));
        CHECK(completed_on(c, utc_ms(2025, 4, 17, 18, 15), utc_ms(2025, 4, 17, 18, 29)));
        TimeframeAggregator q15_single("15", "1", NY, RTH);
        auto c0 = drive(q15_single, slice, utc_ms(2025, 4, 17, 18, 30), 0);
        CHECK(!completed_on(c0, q, utc_ms(2025, 4, 17, 18, 13)));
        CHECK(completed_on(c0, q, utc_ms(2025, 4, 17, 18, 15)));
    }
    // A minute missing INSIDE the slice (not its last) changes nothing: the
    // real-end rule fires on the last minute as before.
    {
        TimeframeAggregator h60("60", "1", NY, RTH);
        const std::vector<Bar> inner = minutes(utc_ms(2025, 5, 14, 16, 30), utc_ms(2025, 5, 14, 18, 30),
                                               {utc_ms(2025, 5, 14, 17, 3)});
        auto c = drive(h60, inner, utc_ms(2025, 5, 15, 13, 30), 900);
        CHECK(completed_on(c, h0, utc_ms(2025, 5, 14, 17, 29)));
        CHECK(completions_of(c, h0) == 1);
    }
    // 24x7 (BINANCE-shaped, UTC): the chart-close rule is TradingView's
    // chart geometry, not a session fact -- a 15m chart bar closes at the
    // bucket end whether or not its last minute printed.
    {
        TimeframeAggregator h60("60", "1", "UTC", "");
        const int64_t b0 = utc_ms(2025, 5, 14, 16, 0);
        const std::vector<Bar> thin = minutes(utc_ms(2025, 5, 14, 16, 0), utc_ms(2025, 5, 14, 18, 0),
                                              {utc_ms(2025, 5, 14, 16, 59)});
        auto c = drive(h60, thin, utc_ms(2025, 5, 14, 18, 0), 900);
        CHECK(completed_on(c, b0, utc_ms(2025, 5, 14, 16, 58)));
        CHECK(completions_of(c, b0) == 1);
        CHECK(completed_on(c, utc_ms(2025, 5, 14, 17, 0), utc_ms(2025, 5, 14, 17, 59)));
    }
}

// 2. Early close. NYSE:F Thu 2025-07-03 closes 13:00 ET (17:00Z): the '240'
//    bucket 13:30-17:30Z holds 3.5 hours; the next chart bar is Mon 07-07
//    09:30 ET. TradingView finalizes the bucket on the session's last chart
//    bar (16:45Z); the calling close 17:00Z does not reach the bucket end, so
//    it is the early-close rule that completes it -- on an exchange calendar
//    only: an OTC stream (set_early_close_completes(false)) keeps waiting for
//    the boundary bar as before.
void test_early_close_completes_on_exchange_only() {
    std::printf("2. early-close completion of a '240' bucket the session leaves open\n");
    const int64_t b0 = utc_ms(2025, 7, 3, 13, 30);
    const std::vector<Bar> bars = minutes(utc_ms(2025, 7, 3, 13, 30), utc_ms(2025, 7, 3, 17, 0));
    const int64_t monday = utc_ms(2025, 7, 7, 13, 30);
    {
        TimeframeAggregator h240("240", "1", NY, RTH);
        auto c = drive(h240, bars, monday, 900);
        CHECK(completed_on(c, b0, utc_ms(2025, 7, 3, 16, 59)));
        CHECK(completions_of(c, b0) == 1);
        // Monday's first minute opens a fresh bucket without re-emitting.
        AggregatedBar mon = h240.feed(bar_at(monday, 10.0), monday + kMinute, monday + 15 * kMinute);
        CHECK(!mon.is_complete);
    }
    {
        TimeframeAggregator h240("240", "1", NY, RTH);
        h240.set_early_close_completes(false);
        auto c = drive(h240, bars, monday, 900);
        CHECK(!completed_on(c, b0, utc_ms(2025, 7, 3, 16, 59)));
        CHECK(completions_of(c, b0) == 0);
        AggregatedBar mon = h240.feed(bar_at(monday, 10.0), monday + kMinute, monday + 15 * kMinute);
        CHECK(mon.is_complete && mon.bar.timestamp == b0);
    }
    // The single-feed shape (no calling close) completes on the same bar:
    // the next input bar opening a later session-day is the whole rule.
    {
        TimeframeAggregator h240("240", "1", NY, RTH);
        auto c = drive(h240, bars, monday, 0);
        CHECK(completed_on(c, b0, utc_ms(2025, 7, 3, 16, 59)));
    }
    // A regular close is unchanged: the session-close rule already fires on
    // the day's last minute, once.
    {
        TimeframeAggregator h240("240", "1", NY, RTH);
        const std::vector<Bar> full = minutes(utc_ms(2025, 7, 2, 13, 30), utc_ms(2025, 7, 2, 20, 0));
        auto c = drive(h240, full, utc_ms(2025, 7, 3, 13, 30), 900);
        CHECK(completed_on(c, utc_ms(2025, 7, 2, 17, 30), utc_ms(2025, 7, 2, 19, 59)));
        CHECK(completions_of(c, utc_ms(2025, 7, 2, 17, 30)) == 1);
        CHECK(completions_of(c, utc_ms(2025, 7, 2, 13, 30)) == 1);
    }
}

// 4. Early close, singleton bucket (round 8, family U). CME_MINI:NQ1! 15m,
//    session 1700-1600 America/Chicago: Thu 2025-07-03 closes 12:15 CT, so
//    the 12:00 CT chart bar (17:00Z) is the whole 12:00 "60" bucket -- its
//    first AND last bar. The nominal 16:00 CT close is hours away, so the
//    singleton session-final rule cannot see it; the next input bar is the
//    17:00 CT reopen (22:00Z), a later session-day. TradingView completes the
//    bucket on the 12:00 CT bar (lab tv u-lati-levels-nq15: the level machine's
//    ta.change(time) fires there, 2026-09-05); the engine used to wait for the
//    reopen bar. An OTC stream keeps the boundary completion.
void test_early_close_completes_singleton_bucket() {
    std::printf("4. early-close completion of a singleton '60' bucket on the session's last chart bar\n");
    const std::string CT = "America/Chicago";
    const std::string GLOBEX = "1700-1600";
    // 15m chart bars 08:30..12:00 CT (13:30Z..17:00Z inclusive) on 07-03.
    std::vector<Bar> bars;
    for (int64_t t = utc_ms(2025, 7, 3, 13, 30); t <= utc_ms(2025, 7, 3, 17, 0); t += 15 * kMinute)
        bars.push_back(bar_at(t, 10.0));
    const int64_t reopen = utc_ms(2025, 7, 3, 22, 0);       // 17:00 CT
    const int64_t noon = utc_ms(2025, 7, 3, 17, 0);         // 12:00 CT = the singleton bucket
    const int64_t eleven = utc_ms(2025, 7, 3, 16, 0);       // 11:00 CT, a full bucket
    {
        TimeframeAggregator h60("60", "15", CT, GLOBEX);
        auto c = drive(h60, bars, reopen, 0);
        CHECK(completed_on(c, eleven, utc_ms(2025, 7, 3, 16, 45)));
        CHECK(completions_of(c, eleven) == 1);
        CHECK(completed_on(c, noon, noon));
        CHECK(completions_of(c, noon) == 1);
        // The reopen bar opens a fresh bucket without re-emitting the noon one.
        AggregatedBar re = h60.feed(bar_at(reopen, 10.0), reopen + 15 * kMinute, 0);
        CHECK(!re.is_complete);
    }
    {
        TimeframeAggregator h60("60", "15", CT, GLOBEX);
        h60.set_early_close_completes(false);
        auto c = drive(h60, bars, reopen, 0);
        CHECK(!completed_on(c, noon, noon));
        CHECK(completions_of(c, noon) == 0);
        AggregatedBar re = h60.feed(bar_at(reopen, 10.0), reopen + 15 * kMinute, 0);
        CHECK(re.is_complete && re.bar.timestamp == noon);
    }
    // A singleton that is NOT the session's last bar keeps waiting: on a
    // regular day the 12:00 CT bar is followed by 12:15 CT in the same
    // session-day, and the bucket completes on its real end (12:45 CT).
    {
        TimeframeAggregator h60("60", "15", CT, GLOBEX);
        std::vector<Bar> regular;
        for (int64_t t = utc_ms(2025, 7, 2, 13, 30); t <= utc_ms(2025, 7, 2, 18, 0); t += 15 * kMinute)
            regular.push_back(bar_at(t, 10.0));
        auto c = drive(h60, regular, utc_ms(2025, 7, 2, 18, 15), 0);
        const int64_t noon2 = utc_ms(2025, 7, 2, 17, 0);
        CHECK(!completed_on(c, noon2, noon2));
        CHECK(completed_on(c, noon2, utc_ms(2025, 7, 2, 17, 45)));
        CHECK(completions_of(c, noon2) == 1);
    }
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
// 3. End to end on the split feed: the "60" bucket ending at the 17:15Z
//    chart bar (whose slice lacks 17:29Z) is published before that chart
//    bar's body runs, so the body's [1] window is TradingView's.
class OrderProbe final : public BacktestEngine {
public:
    std::vector<std::pair<std::string, int64_t>> events;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "60", input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id == 0 && is_complete) events.emplace_back("h60", bar.timestamp);
    }
    void on_bar(const Bar& bar) override { events.emplace_back("bar", bar.timestamp); }
};

void test_split_feed_publishes_before_the_chart_body() {
    std::printf("3. split feed: the thin hour is published before its last chart bar's body\n");
    std::vector<Bar> chart;
    for (int64_t t = utc_ms(2025, 5, 14, 13, 30); t < utc_ms(2025, 5, 14, 20, 0); t += 15 * kMinute)
        chart.push_back(bar_at(t, 10.0));
    for (int64_t t = utc_ms(2025, 5, 15, 13, 30); t < utc_ms(2025, 5, 15, 20, 0); t += 15 * kMinute)
        chart.push_back(bar_at(t, 10.0));
    std::vector<Bar> aux;
    for (const Bar& b : chart) {
        auto slice = minutes(b.timestamp, b.timestamp + 15 * kMinute, {utc_ms(2025, 5, 14, 17, 29)});
        aux.insert(aux.end(), slice.begin(), slice.end());
    }
    OrderProbe probe;
    strategy_set_syminfo_timezone(static_cast<pf_strategy_t>(&probe), NY.c_str());
    strategy_set_syminfo_session(static_cast<pf_strategy_t>(&probe), RTH.c_str());
    strategy_set_syminfo_type(static_cast<pf_strategy_t>(&probe), "stock");
    CHECK(strategy_set_aux_security_feed(static_cast<pf_strategy_t>(&probe),
                                         reinterpret_cast<const pf_bar_t*>(aux.data()),
                                         static_cast<int>(aux.size()), "1") == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) std::printf("   engine error: %s\n", probe.last_error().c_str());
    const int64_t thin_hour = utc_ms(2025, 5, 14, 16, 30);
    const int64_t last_bar = utc_ms(2025, 5, 14, 17, 15);
    int published_at = -1, body_at = -1;
    for (std::size_t i = 0; i < probe.events.size(); ++i) {
        if (probe.events[i].first == "h60" && probe.events[i].second == thin_hour && published_at < 0)
            published_at = static_cast<int>(i);
        if (probe.events[i].first == "bar" && probe.events[i].second == last_bar)
            body_at = static_cast<int>(i);
    }
    CHECK(published_at >= 0);
    CHECK(body_at >= 0);
    CHECK(published_at >= 0 && body_at >= 0 && published_at < body_at);
    int hours = 0;
    for (const auto& e : probe.events) if (e.first == "h60" && e.second == thin_hour) ++hours;
    CHECK(hours == 1);
}
#endif

}  // namespace

int main() {
    test_chart_close_completes_thin_hour();
    test_early_close_completes_on_exchange_only();
    test_early_close_completes_singleton_bucket();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    test_split_feed_publishes_before_the_chart_body();
#endif
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
