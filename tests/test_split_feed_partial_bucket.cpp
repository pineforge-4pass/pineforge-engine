// Split-feed request.security must finalize the calling chart bar's last
// partial finer bucket exactly once. Its previous slot remains the preceding
// bucket; lookahead_on latches the bucket at or before the chart open.
// Pinned against TradingView behaviour (recorded tapes are kept outside this
// repository). Invented winter and summer sessions construct dense,
// singleton, early-close and two-minute tails without market data.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "split-feed partial bucket test requires the auxiliary feed V1 probe"
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

constexpr int64_t kMinute = 60000;

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

struct SyntheticFeed {
    std::vector<Bar> daily;
    std::vector<Bar> minutes;
};

Bar flat_bar(int64_t ts, double price) {
    return Bar{price, price, price, price, 1.0, ts};
}

// Stable alternating closes 100,102 seed RSI(14) with seven gains and seven
// losses. After warmup its down-slot U=26/27, D=28/27, RSI=1300/27;
// its up-slot RSI=1400/27. Hundreds of slots make seed error <1e-9.
SyntheticFeed winter_feed() {
    SyntheticFeed out;
    // Daily OHLC agrees with each invented minute slice, including its
    // final close. The label precedes the 18:00 ET session by one hour.
    out.daily = {{100.0, 102.0, 100.0, 100.0, 1.0, utc(2000, 1, 10, 22, 0)},
                 {102.0, 106.0, 98.0, 106.0, 1.0, utc(2000, 1, 11, 22, 0)},
                 {102.0, 102.0, 100.0, 100.0, 1.0, utc(2000, 1, 12, 22, 0)}};
    int slot = 0;
    const auto dense = [&](int64_t start, int buckets) {
        for (int b = 0; b < buckets; ++b, ++slot) {
            const double price = 100.0 + 2.0 * (slot % 2);
            for (int m = 0; m < 3; ++m)
                out.minutes.push_back(flat_bar(start + (3*b + m)*kMinute, price));
        }
    };
    // 459 buckets: 23:03 through next day's 21:57, all three minutes.
    dense(utc(2000, 1, 10, 23, 3), 459);
    // 436 dense buckets, then two singleton buckets separated by a gap:
    // 21:56 opens the 21:54 bucket, 21:59 opens the 21:57 bucket.
    dense(utc(2000, 1, 11, 23, 3), 436);
    out.minutes.push_back(flat_bar(utc(2000, 1, 12, 21, 56), 98.0));
    out.minutes.push_back(flat_bar(utc(2000, 1, 12, 21, 59), 106.0));
    slot += 2;
    // 404 full buckets: 23:36 through next day's 19:45, early close 19:47.
    dense(utc(2000, 1, 12, 23, 36), 404);
    return out;
}

SyntheticFeed summer_feed() {
    SyntheticFeed out;
    out.daily.push_back({100.0, 102.0, 96.0, 96.0, 1.0,
                         utc(2000, 7, 10, 21, 0)});
    const int64_t start = utc(2000, 7, 10, 22, 3);
    for (int b = 0; b < 458; ++b) {
        const double price = 100.0 + 2.0 * (b % 2);
        for (int m = 0; m < 3; ++m)
            out.minutes.push_back(flat_bar(start + (3*b + m)*kMinute, price));
    }
    // Missing the last minute leaves a two-minute 20:57 bucket pending.
    out.minutes.push_back(flat_bar(utc(2000, 7, 11, 20, 57), 96.0));
    out.minutes.push_back(flat_bar(utc(2000, 7, 11, 20, 58), 96.0));
    return out;
}

const SyntheticFeed kWinter = winter_feed();
const SyntheticFeed kSummer = summer_feed();

bool within(double v, double expected, double tol) {
    return !std::isnan(v) && std::abs(v - expected) <= tol;
}

struct Publication {
    int64_t label = 0;   // the bucket's grid open (bar_label_ms)
    int subs = 0;        // sub-bars the bucket holds when published
    double close = 0.0;
    double rsi = 0.0;
};

// Mirrors the generated form of the pin's site and its lookahead_on twin:
//   sec 0: request.security(tickerid, "3", ta.rsi(close, 14), lookahead_off)
//   sec 1: request.security(tickerid, "3", ta.rsi(close, 14), lookahead_on)
// One RSI(14) per requested context, compute() on a new history slot and
// recompute() otherwise (security_series_slot_is_new); every published
// dispatch (is_complete) advances the exposed history, which the chart body
// reads through its latest slot -- ``r`` -- and the one before -- ``r[1]``
// in the requested context.
class FinerRsiProbe final : public pineforge::source::PineStrategyHost {
public:
    ta::RSI rsi_off{14};
    ta::RSI rsi_on{14};
    std::vector<Publication> off;   // sec 0: every published 3m bucket
    std::vector<Publication> on;    // sec 1: every (gated) publication
    int dispatches_off = 0;         // sec 0 evaluator calls, published or not

    struct ChartRead {
        int64_t chart_ts = 0;
        std::size_t off_count = 0;
        std::size_t on_count = 0;
        Publication last;   // r
        Publication prev;   // r[1]
        Publication last_on;
    };
    std::vector<ChartRead> reads;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "3", input_tf_, /*lookahead_on=*/false,
                               /*gaps_on=*/false);
        register_security_eval(1, "3", input_tf_, /*lookahead_on=*/true,
                               /*gaps_on=*/false);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        ta::RSI& rsi = sec_id == 0 ? rsi_off : rsi_on;
        const double v = security_series_slot_is_new(sec_id)
                             ? rsi.compute(bar.close)
                             : rsi.recompute(bar.close);
        if (sec_id == 0) ++dispatches_off;
        if (!is_complete) return;
        const SecurityEvalState& state =
            security_eval_states_[static_cast<std::size_t>(sec_id)];
        (sec_id == 0 ? off : on).push_back(
            {bar.timestamp, state.current_sub_bar_count, bar.close, v});
    }

    void on_source_bar(const Bar& bar) override {
        ChartRead r;
        r.chart_ts = bar.timestamp;
        r.off_count = off.size();
        r.on_count = on.size();
        if (!off.empty()) r.last = off.back();
        if (off.size() >= 2) r.prev = off[off.size() - 2];
        if (!on.empty()) r.last_on = on.back();
        reads.push_back(r);
    }
};

// Session labels are metadata only; the daily and minute prices are invented.
void run_xau_daily(FinerRsiProbe& probe, const Bar* daily, int n_daily,
                   const Bar* aux, int n_aux) {
    probe.set_syminfo_timezone("America/New_York");
    probe.set_syminfo_session("1800-1700");
    probe.set_syminfo_type("cfd");
    CHECK(probe.set_aux_security_feed(aux, n_aux, "1"),
          "the 1m auxiliary feed installs");
    probe.run(daily, n_daily, "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
}

int count(const std::vector<Bar>& bars) { return static_cast<int>(bars.size()); }

// Preserve the original rounding tolerance and the stricter history tolerance.
constexpr double kTape = 0.005;
constexpr double kRecon = 1e-3;


// Winter: down-slot RSI=1300/27=48.148148148148 on the dense tail.
// The sparse predecessor falls 100->98: U=169/189, D=209/189,
// RSI=16900/378=44.708994708995. The final singleton rises 98->106:
// U=3709/2646, D=2717/2646, RSI=370900/6426=57.718643012761.
// 404 alternating slots restore the down-slot RSI by the early-close tail.
// Counts are constructed: 459 + (436+2) + 404 = 1301 publications;
// index 897 = 459+438 is the next session's first publication.
void test_singleton_tail_reads_at_the_daily_close() {
    FinerRsiProbe probe;
    run_xau_daily(probe, kWinter.daily.data(), count(kWinter.daily), kWinter.minutes.data(),
                  count(kWinter.minutes));
    CHECK(probe.reads.size() == 3, "three daily bars dispatched");

    const auto& dense = probe.reads[0];
    CHECK(dense.chart_ts == utc(2000, 1, 10, 22, 0), "bar 0 is day 0 17:00 ET");
    CHECK(dense.off_count == 459, "459 3m buckets published inside the dense day");
    CHECK(dense.last.label == utc(2000, 1, 11, 21, 57), "dense tail = the 21:57 bucket");
    CHECK(dense.last.subs == 3, "the dense tail bucket holds its three minutes");
    CHECK(within(dense.last.close, 100.0, 1e-9), "its close is the 21:59 minute's");
    CHECK(within(dense.last.rsi, 48.148148148148, kTape), "synthetic RSI 48.148148148148 on the day 0 bar");
    CHECK(dense.prev.label == utc(2000, 1, 11, 21, 54), "r[1] = the 21:54 bucket");

    const auto& thanks = probe.reads[1];
    CHECK(thanks.chart_ts == utc(2000, 1, 11, 22, 0), "bar 1 is day 1 17:00 ET");
    CHECK(thanks.off_count == 459 + 438, "438 3m buckets published inside sparse day, the singleton tail included");
    CHECK(thanks.last.label == utc(2000, 1, 12, 21, 57), "the close reads the 21:57 bucket");
    CHECK(thanks.last.subs == 1, "the 21:57 bucket holds the 21:59 minute alone");
    CHECK(within(thanks.last.close, 106.0, 1e-9), "its close is the 21:59 minute's (the daily close)");
    CHECK(within(thanks.last.rsi, 57.718643012761, kTape), "synthetic RSI 57.718643012761 on the sparse day bar (was 44.708994708995)");
    CHECK(thanks.prev.label == utc(2000, 1, 12, 21, 54), "r[1] = the 21:54 bucket");
    CHECK(thanks.prev.subs == 1, "the 21:54 bucket holds the 21:56 minute alone");
    CHECK(within(thanks.prev.rsi, 44.708994708995, kTape), "r[1] = 44.708994708995, the pre-fix read");

    const auto& friday = probe.reads[2];
    CHECK(friday.chart_ts == utc(2000, 1, 12, 22, 0), "bar 2 is day 2 17:00 ET");
    CHECK(friday.off_count == 459 + 438 + 404, "404 3m buckets published inside early-close day");
    CHECK(friday.last.label == utc(2000, 1, 13, 19, 45), "the close reads the 19:45 bucket");
    CHECK(friday.last.subs == 3, "the 19:45 bucket is full");
    CHECK(within(friday.last.close, 100.0, 1e-9), "its close is the 19:47 minute's");
    CHECK(within(friday.last.rsi, 48.148148148148, kTape), "synthetic RSI 48.148148148148 on the early-close day bar");
    CHECK(friday.prev.label == utc(2000, 1, 13, 19, 42), "r[1] = the 19:42 bucket");

    for (std::size_t i = 1; i < probe.off.size(); ++i) {
        CHECK(probe.off[i].label > probe.off[i - 1].label,
              "published 3m buckets are strictly increasing (no re-publication)");
    }
    CHECK(probe.off.size() == 1301, "1301 = every 3m bucket of the three slices, each once");
    CHECK(probe.off[897].label >= utc(2000, 1, 12, 23, 0),
          "early-close day's first publication opens in its own session (18:00 ET)");
    CHECK(probe.dispatches_off == 1301, "one evaluator dispatch per published bucket");

    CHECK(probe.on.size() == probe.off.size(), "lookahead_on publishes every 3m bucket, like lookahead_off");
    for (std::size_t i = 0; i < probe.on.size(); ++i) {
        CHECK(probe.on[i].label == probe.off[i].label, "the lookahead_on publications are the same buckets, in order");
    }
    CHECK(dense.on_count == 0, "the range's first body runs before any 3m bucket");
    CHECK(thanks.on_count == 459, "the sparse day body runs after the dense day's 459 buckets, before its own");
    CHECK(friday.on_count == 459 + 438, "the early-close day body runs after sparse day's 438, the finalized singleton included");
    CHECK(thanks.last_on.label == probe.off[458].label, "sparse day reads the dense day's last bucket");
    CHECK(thanks.last_on.label == utc(2000, 1, 11, 21, 57), "the 16:57 ET bucket");
    CHECK(within(thanks.last_on.close, 100.0, 1e-9), "synthetic lookahead_on close on the sparse day bar");
    CHECK(friday.last_on.label == probe.off[459 + 437].label, "early-close day reads sparse day's last bucket");
    CHECK(friday.last_on.label == utc(2000, 1, 12, 21, 57), "the 21:57Z singleton");
    CHECK(within(friday.last_on.close, 106.0, 1e-9), "synthetic lookahead_on close on the early-close day bar");
    CHECK(within(friday.last_on.rsi, probe.off[459 + 437].rsi, 1e-9), "the singleton's RSI, as the lookahead_off twin published it");
}


// Summer: 458 full buckets plus one two-minute bucket = 459.
// Predecessor up-slot RSI=1400/27=51.851851851852. Tail falls 102->96:
// U=182/189, D=250/189, RSI=18200/432=42.129629629630.
// Reading the predecessor instead of the partial tail fails both RSI checks.
void test_two_minute_tail_reads_at_the_daily_close() {
    FinerRsiProbe probe;
    run_xau_daily(probe, kSummer.daily.data(), count(kSummer.daily), kSummer.minutes.data(),
                  count(kSummer.minutes));
    CHECK(probe.reads.size() == 1, "one daily bar dispatched");
    const auto& day = probe.reads[0];
    CHECK(day.chart_ts == utc(2000, 7, 10, 21, 0), "the bar is summer day 17:00 EDT");
    CHECK(day.off_count == 459, "459 3m buckets published, the 2-minute tail included");
    CHECK(day.last.label == utc(2000, 7, 11, 20, 57), "the close reads the 20:57 bucket");
    CHECK(day.last.subs == 2, "the 20:57 bucket holds 20:57 and 20:58");
    CHECK(within(day.last.close, 96.0, 1e-9), "its close is the 20:58 minute's (the daily close)");
    CHECK(within(day.last.rsi, 42.129629629630, kRecon), "the 20:57 bucket's RSI (Wilder recurrence 42.129629629630)");
    CHECK(day.prev.label == utc(2000, 7, 11, 20, 54) && day.prev.subs == 3,
          "r[1] = the full 20:54 bucket");
    CHECK(within(day.prev.rsi, 51.851851851852, kRecon), "r[1] = 51.851851851852, the pre-fix read");
    CHECK(probe.dispatches_off == 459, "one evaluator dispatch per published bucket");
}


// Without the auxiliary slice the sparse singleton waits for the following
// session. The body still sees slot 895 at 21:54, RSI=16900/378;
// only 459+437 buckets have been published at that close.
void test_without_an_auxiliary_slice_the_read_is_unchanged() {
    FinerRsiProbe probe;
    probe.set_syminfo_timezone("America/New_York");
    probe.set_syminfo_session("1800-1700");
    probe.set_syminfo_type("cfd");
    probe.run(kWinter.minutes.data(), count(kWinter.minutes), "1", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
    CHECK(probe.reads.size() >= 2, "the dense day and sparse day completed as script bars");
    const auto& thanks = probe.reads[1];
    CHECK(thanks.last.label == utc(2000, 1, 12, 21, 54),
          "legacy path: the sparse day close still reads the 21:54 bucket");
    CHECK(within(thanks.last.rsi, 44.708994708995, kTape), "legacy path: 44.708994708995 as before");
    CHECK(thanks.off_count == 459 + 437, "legacy path: the 21:57 singleton is not yet published");
}


// The primitive crosses a bucket boundary with one minute per bucket,
// finalizes once, then rejects re-publication even if later minutes merge.
void test_complete_pending_partial_finalizes_once() {
    TimeframeAggregator agg("3", "1", "America/New_York", "1800-1700");
    auto bar = [](int64_t ts, double px) {
        return Bar{px, px, px, px, 1.0, ts};
    };
    CHECK(!agg.has_pending_partial(), "nothing pending before the first bar");
    CHECK(!agg.complete_pending_partial().is_complete, "nothing to finalize before the first bar");

    AggregatedBar r = agg.feed(bar(utc(2000, 1, 12, 21, 56), 98.0));
    CHECK(!r.is_complete && agg.has_pending_partial(), "21:54 opened, partial");
    r = agg.feed(bar(utc(2000, 1, 12, 21, 59), 106.0));
    CHECK(r.is_complete && r.bar.timestamp == utc(2000, 1, 12, 21, 54) && r.sub_bar_count == 1,
          "the boundary emits the 21:54 singleton");
    CHECK(agg.has_pending_partial(), "the 21:57 singleton is pending");
    r = agg.complete_pending_partial();
    CHECK(r.is_complete && r.bar.timestamp == utc(2000, 1, 12, 21, 57) && r.sub_bar_count == 1,
          "finalized as the 21:57 bucket holding one minute");
    CHECK(within(r.bar.close, 106.0, 1e-9), "its close is the 21:59 minute's");
    CHECK(agg.last_completed().timestamp == utc(2000, 1, 12, 21, 57), "it is the last completed bucket");
    CHECK(!agg.has_pending_partial(), "nothing pending after the finalize");
    CHECK(!agg.complete_pending_partial().is_complete, "a second finalize is a no-op");
    r = agg.feed(bar(utc(2000, 1, 12, 23, 4), 108.0));
    CHECK(!r.is_complete && r.bar.timestamp == utc(2000, 1, 12, 23, 3) && r.sub_bar_count == 1,
          "the next boundary opens a fresh bucket, nothing re-emitted");

    TimeframeAggregator guard("3", "1", "America/New_York", "1800-1700");
    guard.feed(bar(utc(2000, 1, 12, 21, 57), 1.0));
    CHECK(guard.complete_pending_partial().is_complete, "finalized on one minute");
    r = guard.feed(bar(utc(2000, 1, 12, 21, 58), 2.0));
    CHECK(!r.is_complete && r.sub_bar_count == 2, "a later minute merges without completing");
    r = guard.feed(bar(utc(2000, 1, 12, 21, 59), 3.0));
    CHECK(!r.is_complete && r.sub_bar_count == 3, "the count is reached but the bucket was already emitted");
    CHECK(!guard.has_pending_partial(), "still nothing pending");

    TimeframeAggregator dense("3", "1", "America/New_York", "1800-1700");
    dense.feed(bar(utc(2000, 1, 11, 21, 57), 1.0));
    dense.feed(bar(utc(2000, 1, 11, 21, 58), 2.0));
    r = dense.feed(bar(utc(2000, 1, 11, 21, 59), 3.0));
    CHECK(r.is_complete && r.sub_bar_count == 3, "count completion");
    CHECK(!dense.has_pending_partial(), "no partial after a count completion");

    TimeframeAggregator daily("D", "1", "America/New_York", "1800-1700");
    daily.feed(bar(utc(2000, 1, 12, 21, 59), 1.0));
    CHECK(!daily.has_pending_partial() && !daily.complete_pending_partial().is_complete,
          "calendar buckets keep their own completion rules");
    TimeframeAggregator same("1", "1", "America/New_York", "1800-1700");
    same.feed(bar(utc(2000, 1, 12, 21, 59), 1.0));
    CHECK(!same.has_pending_partial(), "passthrough has no partial");
}

}  // namespace

int main() {
    test_complete_pending_partial_finalizes_once();
    test_singleton_tail_reads_at_the_daily_close();
    test_two_minute_tail_reads_at_the_daily_close();
    test_without_an_auxiliary_slice_the_read_is_unchanged();
    std::printf("test_split_feed_partial_bucket: OK\n");
    return 0;
}
