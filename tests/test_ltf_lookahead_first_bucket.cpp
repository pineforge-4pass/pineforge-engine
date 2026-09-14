// Split-feed request.security, finer than the chart, under lookahead_on: the
// calling chart bar reads the value evaluated on its FIRST intrabar bucket and
// holds it for the bar (round 7, family r7-ltf-lookahead; lab tv
// notrade-ltf-sample-btc1d, 2026-09-05, and cross-na-edge-btc1d, 2026-09-05).
//
// On the BINANCE:BTCUSDT 1D chart, ``request.security(syminfo.tickerid, "15",
// ta.rsi(close, 14)[1], lookahead = barmerge.lookahead_on)`` is served from the
// lane's 1m finer feed (the split-feed path every @1D lane runs). The pin
// reverses a position on every daily bar with qty = round(v * 100) / 100000,
// so TradingView's own per-bar read comes back as the tape's Size (qty),
// 18/18 over 2025-04-01..20:
//
//   * na on the range's first bar (04-01): rsi[1] on the day's first 15m
//     bucket has no bucket before it;
//   * 53.24 on 04-02, 27.53 on 04-03, 67.52 on 04-04, 50.15 on 04-05: each
//     the 15m RSI of the PREVIOUS day's last bucket -- rsi[1] evaluated on the
//     day's first bucket -- with the 15m series seeded at the range start.
//
// So a plain ``expr`` reads the calling bar's first bucket and ``expr[k]``
// the k-th bucket before it, at the requested cadence. The engine's gated
// publication (publish_gate_tf_seconds: one publication per calling bar, the
// LAST completion) read right for ``expr[1]`` alone and one bucket late for
// ``expr``. Rule as implemented (calling_open_latches_first): every completed
// bucket is published, and the chart body runs right after the calling bar's
// first bucket -- the slice's remaining auxiliary bars are held back and fed
// after dispatch_bar (feed_deferred_aux_security_for_chart_bar), so the TA
// state still sees every sub-bar in order before the next chart bar. The
// lookahead_off twin keeps reading the calling bar's LAST bucket at its close.
//
// The same tape window pins ta.crossover / ta.crossunder on the first valid
// bar after na: lab tv cross-na-edge-btc1d (same chart and range, qty =
// 100000 + flags) carries the real 27.53 -> 67.52 crossover on the 04-04 bar
// and NOTHING else -- neither crossover(v, 0) nor crossunder(v, 100) fires on
// 04-02 (na -> 53.24), nor their chart-level twins on bar 2. The engine's
// former "first-valid edge" (a > b with a na previous value) was wrong.
//
// Data: tests/test_ltf_lookahead_first_bucket_data.hpp -- the lane's 1D chart
// feed 14b8e066 and 1m finer feed 787fd2e1, 2025-04-01..05, so the requested
// series seeds exactly as TradingView's did. The expected first-bucket and
// second-last-bucket values are a 15m RSI(14) reconstruction from the same 1m
// bars (scratchpad r7ltf gen, Wilder RMA seeded by the first 14 changes); the
// previous-day-last values are the tape's.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "test_ltf_lookahead_first_bucket_data.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the first-bucket latch test requires the auxiliary feed V1 probe"
#endif

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n  %s\n",        \
                         __FILE__, __LINE__, #cond, (msg));                   \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

namespace {

// Unix ms of a UTC civil time (Howard Hinnant's days_from_civil).
int64_t utc(int y, int m, int d, int h, int mi) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return (days * 86400 + h * 3600 + mi * 60) * 1000;
}

bool within(double v, double expected, double tol) {
    return !std::isnan(v) && std::abs(v - expected) <= tol;
}

struct Publication {
    int64_t label = 0;
    double rsi = 0.0;
};

// Mirrors the generated form of the pin's site and its siblings:
//   sec 0: request.security(tickerid, "15", ta.rsi(close, 14)[1], lookahead_on)
//   sec 1: request.security(tickerid, "15", ta.rsi(close, 14),    lookahead_on)
//   sec 2: request.security(tickerid, "15", ta.rsi(close, 14)[2], lookahead_on)
//   sec 3: request.security(tickerid, "15", ta.rsi(close, 14),    lookahead_off)
// One RSI(14) per requested context, compute() on a new history slot and
// recompute() otherwise (security_series_slot_is_new). An offset site reads
// its exposed value from the published history BEFORE pushing the current
// evaluation (the codegen's ``_req = hist[k-1]; if (is_complete) hist.push``);
// a plain site exposes the current evaluation.
class FirstBucketProbe final : public pineforge::source::PineStrategyHost {
public:
    ta::RSI rsi[4]{ta::RSI(14), ta::RSI(14), ta::RSI(14), ta::RSI(14)};
    std::vector<double> hist[4];
    std::vector<Publication> published[4];
    double exposed[4] = {na<double>(), na<double>(), na<double>(), na<double>()};
    int dispatches[4] = {0, 0, 0, 0};

    struct ChartRead {
        int64_t chart_ts = 0;
        double v[4] = {0, 0, 0, 0};
        std::size_t published_count[4] = {0, 0, 0, 0};
        Publication last[4];
    };
    std::vector<ChartRead> reads;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "15", input_tf_, /*lookahead_on=*/true, false);
        register_security_eval(1, "15", input_tf_, /*lookahead_on=*/true, false);
        register_security_eval(2, "15", input_tf_, /*lookahead_on=*/true, false);
        register_security_eval(3, "15", input_tf_, /*lookahead_on=*/false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        const double v = security_series_slot_is_new(sec_id)
                             ? rsi[sec_id].compute(bar.close)
                             : rsi[sec_id].recompute(bar.close);
        ++dispatches[sec_id];
        auto& h = hist[sec_id];
        switch (sec_id) {
            case 0:
                exposed[0] = h.empty() ? na<double>() : h.back();
                break;
            case 2:
                exposed[2] = h.size() < 2 ? na<double>() : h[h.size() - 2];
                break;
            default:
                exposed[sec_id] = v;
                break;
        }
        if (!is_complete) return;
        h.push_back(v);
        published[sec_id].push_back({bar.timestamp, v});
    }

    void on_source_bar(const Bar& bar) override {
        ChartRead r;
        r.chart_ts = bar.timestamp;
        for (int s = 0; s < 4; ++s) {
            r.v[s] = exposed[s];
            r.published_count[s] = published[s].size();
            if (!published[s].empty()) r.last[s] = published[s].back();
        }
        reads.push_back(r);
    }
};

template <std::size_t N>
constexpr int count(const Bar (&)[N]) { return static_cast<int>(N); }

// The BINANCE:BTCUSDT 1D lane as the campaign declares it: crypto, 24x7 UTC,
// native daily bars as the chart, the 1m feed as the auxiliary slice.
void run_btc_daily(FirstBucketProbe& probe) {
    using namespace ltf_lookahead_first_bucket_data;
    probe.set_syminfo_timezone("UTC");
    probe.set_syminfo_session("24x7");
    probe.set_syminfo_type("crypto");
    CHECK(probe.set_aux_security_feed(kBtc1mApr, count(kBtc1mApr), "1"),
          "the 1m auxiliary feed installs");
    probe.run(kBtc1dApr, count(kBtc1dApr), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
}

// TradingView's qty step on the pin is 0.01; the tape rounds the read.
constexpr double kTape = 0.005;
// The reconstruction's precision for values the tape does not cover.
constexpr double kRecon = 1e-3;

constexpr int kBucketsPerDay = 96;   // 15m buckets in a 24x7 day
constexpr int kMinutesPerDay = 1440;

// ---- the pin: rsi[1] reads the previous day's last bucket ------------------

void test_offset_one_reads_the_previous_days_last_bucket() {
    FirstBucketProbe probe;
    run_btc_daily(probe);
    CHECK(probe.reads.size() == 5, "five daily bars dispatched");

    // The tape (lab tv notrade-ltf-sample-btc1d): na, 53.24, 27.53, 67.52,
    // 50.15 -- each the previous day's last 15m bucket, na on the first bar.
    const double tape[5] = {na<double>(), 53.24, 27.53, 67.52, 50.15};
    for (int d = 0; d < 5; ++d) {
        const auto& r = probe.reads[static_cast<std::size_t>(d)];
        CHECK(r.chart_ts == utc(2025, 4, 1 + d, 0, 0), "daily bars are stamped 00:00Z");
        if (d == 0) {
            CHECK(std::isnan(r.v[0]), "rsi[1] is na on the range's first bar (TV: na)");
        } else {
            CHECK(within(r.v[0], tape[d], kTape), "rsi[1] = the tape's value (the previous day's last bucket)");
        }
    }
    // And that value IS the previous day's last bucket, read through the
    // published history at the day's FIRST bucket: at each chart body the
    // lookahead_on evaluators have published exactly the previous days'
    // 96 buckets plus this day's first one.
    for (int d = 0; d < 5; ++d) {
        const auto& r = probe.reads[static_cast<std::size_t>(d)];
        const std::size_t expect = static_cast<std::size_t>(d) * kBucketsPerDay + 1;
        CHECK(r.published_count[0] == expect, "lookahead_on: the body runs after the day's first bucket, every earlier bucket published");
        CHECK(r.published_count[1] == expect, "lookahead_on (plain): same publication count");
        CHECK(r.published_count[2] == expect, "lookahead_on ([2]): same publication count");
        CHECK(r.last[0].label == r.chart_ts, "the latest lookahead_on publication is the day's 00:00 bucket");
        if (d > 0) {
            const auto& h = probe.published[0];
            const Publication& prev_last = h[expect - 2];   // the bucket before the day's first
            CHECK(prev_last.label == r.chart_ts - 15 * 60000, "rsi[1]'s bucket is the previous day's 23:45 bucket");
            CHECK(within(prev_last.rsi, r.v[0], 1e-12), "rsi[1] IS that bucket's RSI");
        }
    }
}

// ---- the plain and [2] shapes: the first bucket, and the one before rsi[1] --

void test_plain_reads_the_first_bucket_and_offset_two_the_bucket_before() {
    FirstBucketProbe probe;
    run_btc_daily(probe);
    // 15m RSI(14) reconstruction from the same 1m bars: the day's FIRST
    // bucket (00:00-00:14) and the previous day's second-to-last (23:30).
    const double first[5] = {na<double>(), 51.4546, 31.3303, 63.4821, 46.9334};
    const double second_last[5] = {na<double>(), 56.2018, 28.3775, 61.7660, 48.3445};
    for (int d = 0; d < 5; ++d) {
        const auto& r = probe.reads[static_cast<std::size_t>(d)];
        if (d == 0) {
            CHECK(std::isnan(r.v[1]), "plain rsi: na on the first bar (one bucket, RSI(14) unseeded)");
            CHECK(std::isnan(r.v[2]), "rsi[2]: na on the first bar");
            continue;
        }
        CHECK(within(r.v[1], first[d], kRecon), "plain rsi = the day's FIRST 15m bucket's RSI (TV's first intrabar), not the last");
        CHECK(within(r.v[1], r.last[1].rsi, 1e-12), "plain rsi IS the latest publication, the 00:00 bucket");
        CHECK(within(r.v[2], second_last[d], kRecon), "rsi[2] = the previous day's second-to-last bucket (23:30)");
    }
    // Every bucket is published once, in order, for every lookahead_on site
    // -- the requested cadence, not one per calling bar.
    for (int s = 0; s < 3; ++s) {
        CHECK(probe.published[s].size() == 5 * kBucketsPerDay, "480 = every 15m bucket of the five days, each once");
        for (std::size_t i = 1; i < probe.published[s].size(); ++i) {
            CHECK(probe.published[s][i].label == probe.published[s][i - 1].label + 15 * 60000,
                  "published 15m buckets are consecutive (no skip, no re-publication)");
        }
        // lookahead_on evaluates on every sub-bar (partial peeks) -- the
        // deferred remainder of each slice included -- so the TA state saw
        // every minute exactly once.
        CHECK(probe.dispatches[s] == 5 * kMinutesPerDay, "one dispatch per 1m sub-bar (partial peeks + completions)");
    }
}

// ---- the lookahead_off twin is untouched: the calling bar's LAST bucket -----

void test_lookahead_off_twin_still_reads_the_last_bucket() {
    FirstBucketProbe probe;
    run_btc_daily(probe);
    // The reconstruction's last bucket of each day (23:45): 53.2405,
    // 27.5298, 67.5177, 50.1489, 68.2631 -- the tape's next-day rsi[1].
    const double last[5] = {53.2405, 27.5298, 67.5177, 50.1489, 68.2631};
    for (int d = 0; d < 5; ++d) {
        const auto& r = probe.reads[static_cast<std::size_t>(d)];
        CHECK(within(r.v[3], last[d], kRecon), "lookahead_off plain rsi = the day's LAST 15m bucket at the close");
        CHECK(r.published_count[3] == static_cast<std::size_t>(d + 1) * kBucketsPerDay,
              "lookahead_off: the whole slice is published before the body");
        CHECK(r.last[3].label == r.chart_ts + 23 * 3600000 + 45 * 60000, "its latest publication is the 23:45 bucket");
        // The lookahead_on rsi[1] of the NEXT day is this very value.
        if (d + 1 < 5) {
            CHECK(within(probe.reads[static_cast<std::size_t>(d + 1)].v[0], r.v[3], 1e-12),
                  "next day's lookahead_on rsi[1] == this day's lookahead_off last bucket");
        }
    }
    CHECK(probe.dispatches[3] == 5 * kBucketsPerDay, "lookahead_off dispatches on completions only");
}

// ---- the cross pin: no first-valid edge --------------------------------------

void test_crossover_has_no_first_valid_edge() {
    FirstBucketProbe probe;
    run_btc_daily(probe);
    // lab tv cross-na-edge-btc1d: c1 = crossover(v, 30) fires on the 04-04
    // bar only (27.53 -> 67.52); c2 = crossover(v, 0) and c4 =
    // crossunder(v, 100) never fire -- na -> 53.24 on 04-02 is no edge.
    ta::Crossover over30, over0;
    ta::Crossunder under100;
    const bool expect_c1[5] = {false, false, false, true, false};
    for (int d = 0; d < 5; ++d) {
        const double v = probe.reads[static_cast<std::size_t>(d)].v[0];
        CHECK(over30.compute(v, 30.0) == expect_c1[d], "crossover(rsi[1], 30): the real 27.53 -> 67.52 cross on 04-04 only");
        CHECK(!over0.compute(v, 0.0), "crossover(rsi[1], 0) never fires: the first valid bar after na is no edge (TV)");
        CHECK(!under100.compute(v, 100.0), "crossunder(rsi[1], 100) never fires either");
    }
    // The chart-level twins: x = bar_index < 2 ? na : close, y = -x.
    ta::Crossover over_x;
    ta::Crossunder under_y;
    using namespace ltf_lookahead_first_bucket_data;
    for (int d = 0; d < 5; ++d) {
        const double x = d < 2 ? na<double>() : kBtc1dApr[d].close;
        const double y = d < 2 ? na<double>() : -kBtc1dApr[d].close;
        CHECK(!over_x.compute(x, 0.0), "crossover(x, 0) never fires on the first valid bar (TV, chart series)");
        CHECK(!under_y.compute(y, 0.0), "crossunder(y, 0) never fires on the first valid bar (TV, chart series)");
    }
}

}  // namespace

int main() {
    test_offset_one_reads_the_previous_days_last_bucket();
    test_plain_reads_the_first_bucket_and_offset_two_the_bucket_before();
    test_lookahead_off_twin_still_reads_the_last_bucket();
    test_crossover_has_no_first_valid_edge();
    std::printf("test_ltf_lookahead_first_bucket: OK\n");
    return 0;
}
