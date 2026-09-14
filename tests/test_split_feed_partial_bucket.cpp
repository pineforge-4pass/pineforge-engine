// Split-feed request.security, finer than the chart: the calling chart bar's
// LAST intrabar is what the bar's close reads, whatever that bucket's sub-bar
// count (round 7, family r7-3commas-dca; lab tv dca-ltf-last-intrabar,
// 2026-09-05, ledger note tv-tape-dca-ltf-last-intrabar-b69fd96a).
//
// On the OANDA:XAUUSD 1D chart, ``request.security(syminfo.tickerid, "3",
// ta.rsi(close, 14), lookahead = barmerge.lookahead_off)`` is served from the
// lane's 1m finer feed (the split-feed path every @1D lane runs). The pin
// reverses a position on every daily bar with qty = the requested value, so
// TradingView's own per-bar read comes back as the tape's Size (qty):
//
//   * Thanksgiving, the 2025-11-26 22:00Z daily bar: the 1m feed's last 3m
//     buckets hold one minute each (21:54 <- 21:56, 21:57 <- 21:59). TV reads
//     72.64 -- the 21:57 bucket's RSI, the singleton. The aggregator's count
//     (3), real-end (21:59 + 1m < 22:00? no: the bucket END is 22:00 -- but
//     the boundary branch that opens the singleton never tests it) and
//     session-close rules leave that bucket partial until Black Friday's
//     first sub-bar, so the engine read the 21:54 bucket, 38.87, and missed
//     the family's RSI_TP exit on that bar (engine 8 trades vs TV 11).
//   * Black Friday, the 2025-11-27 bar (early close 19:47Z): 43.62, the full
//     19:45 bucket -- dense tails are untouched.
//   * The dense 2025-11-25 bar: 45.14, the full 21:57 bucket -- untouched.
//   * The 2-minute shape (2025-07-07 bar, 20:59 minute missing on 07-08):
//     the 20:57 bucket holds 20:57 + 20:58 and is what the close reads.
//
// Rule as implemented (engine_security.cpp feed_security_eval_state,
// TimeframeAggregator::complete_pending_partial): on the calling chart bar's
// last auxiliary bar, a finer-than-script lookahead_off evaluator whose
// aggregator still holds a partial bucket finalizes and publishes it, once.
// The next chart bar's first sub-bar resets the bucket without re-emitting
// it, the ``[1]`` history is the previous bucket exactly as on a dense tail,
// lookahead_on keeps its own gated publication untouched (one bucket behind
// on this shape, as before -- the tape pins lookahead_off only), and the
// legacy path without an auxiliary slice is byte-identical.
//
// Data: tests/test_split_feed_partial_bucket_data.hpp -- the registry's 1m
// finer feed b2389ad2 and 1D feed 79cdcfb671e5 (lab bars). The expected RSI
// values are TV's (tape, 0.01 qty step); a full-history 3m RSI(14)
// reconstruction from the same feed matches the tape to 0.01 on every bar of
// the pin window (scratchpad r7/dca sim.py, daily.py), and the embedded
// slices alone reproduce it to 1e-4 (459 buckets of warmup ahead of the
// first asserted bucket).

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "test_split_feed_partial_bucket_data.hpp"

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

// The OANDA:XAUUSD 1D lane as the campaign declares it: cfd on 1800-1700 ET,
// native daily bars (stamped 17:00 ET) as the chart, the 1m feed as the
// auxiliary request.security slice.
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

template <std::size_t N>
constexpr int count(const Bar (&)[N]) { return static_cast<int>(N); }

// TradingView's qty step on the pin is 0.01; the tape rounds the read.
constexpr double kTape = 0.005;
// The reconstruction's own precision for values the tape does not cover.
constexpr double kRecon = 1e-3;

// ---- the Thanksgiving singleton, the dense control, Black Friday ----------

void test_singleton_tail_reads_at_the_daily_close() {
    using namespace split_feed_partial_data;
    FinerRsiProbe probe;
    run_xau_daily(probe, kXau1dNov, count(kXau1dNov), kXau1mNov,
                  count(kXau1mNov));
    CHECK(probe.reads.size() == 3, "three daily bars dispatched");

    // (a) The dense 2025-11-25 bar (its slice ends on the 21:57, 21:58 and
    // 21:59 minutes): the 21:57 bucket completes on its count, exactly as
    // before, and the close reads it -- TV 45.14 (tape trade 7).
    const auto& dense = probe.reads[0];
    CHECK(dense.chart_ts == utc(2025, 11, 25, 22, 0), "bar 0 is 11-25 17:00 ET");
    CHECK(dense.off_count == 459, "459 3m buckets published inside the dense day");
    CHECK(dense.last.label == utc(2025, 11, 26, 21, 57), "dense tail = the 21:57 bucket");
    CHECK(dense.last.subs == 3, "the dense tail bucket holds its three minutes");
    CHECK(within(dense.last.close, 4163.575, 1e-9), "its close is the 21:59 minute's");
    CHECK(within(dense.last.rsi, 45.14, kTape), "TV reads 45.14 on the 11-25 bar");
    CHECK(dense.prev.label == utc(2025, 11, 26, 21, 54), "r[1] = the 21:54 bucket");

    // (b) Thanksgiving, the 2025-11-26 bar: the slice's last minutes are
    // 21:22, 21:56 and 21:59, so the 21:54 bucket (one minute) is emitted on
    // the 21:59 boundary and the 21:57 bucket (the 21:59 minute alone) is
    // the pending partial the close must read -- TV 72.64 (tape trade 8).
    const auto& thanks = probe.reads[1];
    CHECK(thanks.chart_ts == utc(2025, 11, 26, 22, 0), "bar 1 is 11-26 17:00 ET");
    CHECK(thanks.off_count == 459 + 438, "438 3m buckets published inside Thanksgiving, the singleton tail included");
    CHECK(thanks.last.label == utc(2025, 11, 27, 21, 57), "the close reads the 21:57 bucket");
    CHECK(thanks.last.subs == 1, "the 21:57 bucket holds the 21:59 minute alone");
    CHECK(within(thanks.last.close, 4158.8, 1e-9), "its close is the 21:59 minute's (the daily close)");
    CHECK(within(thanks.last.rsi, 72.64, kTape), "TV reads 72.64 on the Thanksgiving bar (was 38.87)");
    // r[1] in the requested context is the bucket before it: the 21:54
    // singleton, the value the engine used to read at this close.
    CHECK(thanks.prev.label == utc(2025, 11, 27, 21, 54), "r[1] = the 21:54 bucket");
    CHECK(thanks.prev.subs == 1, "the 21:54 bucket holds the 21:56 minute alone");
    CHECK(within(thanks.prev.rsi, 38.87, kTape), "r[1] = 38.87, the pre-fix read");

    // (c) Black Friday, the 2025-11-27 bar, early close 19:47Z: the 19:45
    // bucket holds 19:45-19:47 and completes on its count -- TV 43.62 (tape
    // trade 9). Nothing pending, nothing forced.
    const auto& friday = probe.reads[2];
    CHECK(friday.chart_ts == utc(2025, 11, 27, 22, 0), "bar 2 is 11-27 17:00 ET");
    CHECK(friday.off_count == 459 + 438 + 404, "404 3m buckets published inside Black Friday");
    CHECK(friday.last.label == utc(2025, 11, 28, 19, 45), "the close reads the 19:45 bucket");
    CHECK(friday.last.subs == 3, "the 19:45 bucket is full");
    CHECK(within(friday.last.close, 4215.82, 1e-9), "its close is the 19:47 minute's");
    CHECK(within(friday.last.rsi, 43.62, kTape), "TV reads 43.62 on the Black Friday bar");
    CHECK(friday.prev.label == utc(2025, 11, 28, 19, 42), "r[1] = the 19:42 bucket");

    // (d) Published once: the 21:57 bucket finalized at the Thanksgiving
    // close is not emitted again when Black Friday's first sub-bar crosses
    // the boundary -- every published label is strictly newer than the one
    // before, and Black Friday's first publication is its own session's.
    for (std::size_t i = 1; i < probe.off.size(); ++i) {
        CHECK(probe.off[i].label > probe.off[i - 1].label,
              "published 3m buckets are strictly increasing (no re-publication)");
    }
    CHECK(probe.off.size() == 1301, "1301 = every 3m bucket of the three slices, each once");
    CHECK(probe.off[897].label >= utc(2025, 11, 27, 23, 0),
          "Black Friday's first publication opens in its own session (18:00 ET)");
    // lookahead_off dispatches the evaluator on completions only: one per
    // bucket, the forced tail included, so no bucket advanced the RSI twice.
    CHECK(probe.dispatches_off == 1301, "one evaluator dispatch per published bucket");

    // (e) The lookahead_on twin reads the calling bar's FIRST intrabar
    // (calling_open_latches_first, round 7 r7-ltf-lookahead; lab tv
    // notrade-ltf-sample-btc1d): every 3m bucket is published, in the same
    // order as the lookahead_off twin's, and the chart body runs right
    // after the slice's first bucket -- so each daily bar reads its own
    // first bucket, not the previous day's last one, and the Thanksgiving
    // singleton tail (a lookahead_off pin) does not concern it.
    CHECK(probe.on.size() == probe.off.size(), "lookahead_on publishes every 3m bucket, like lookahead_off");
    for (std::size_t i = 0; i < probe.on.size(); ++i) {
        CHECK(probe.on[i].label == probe.off[i].label, "the lookahead_on publications are the same buckets, in order");
    }
    CHECK(dense.on_count == 1, "the 11-25 body runs after its slice's first 3m bucket");
    CHECK(thanks.on_count == 459 + 1, "the Thanksgiving body runs after the dense day's 459 buckets and its own first");
    CHECK(friday.on_count == 459 + 438 + 1, "the Black Friday body runs after its own first bucket");
    CHECK(dense.last_on.label == probe.off[0].label, "11-25 reads its first bucket");
    CHECK(thanks.last_on.label == probe.off[459].label, "Thanksgiving reads its first bucket");
    CHECK(friday.last_on.label == probe.off[459 + 438].label, "Black Friday reads its first bucket");
    CHECK(friday.last_on.label >= utc(2025, 11, 27, 23, 0), "Black Friday's first bucket opens in its own session (18:00 ET)");
    CHECK(within(friday.last_on.rsi, probe.off[459 + 438].rsi, 1e-9), "the first bucket's RSI, as the lookahead_off twin published it");
}

// ---- the 2-minute shape -----------------------------------------------------

void test_two_minute_tail_reads_at_the_daily_close() {
    using namespace split_feed_partial_data;
    FinerRsiProbe probe;
    run_xau_daily(probe, kXau1dJul, count(kXau1dJul), kXau1mJul,
                  count(kXau1mJul));
    CHECK(probe.reads.size() == 1, "one daily bar dispatched");
    // The 2025-07-07 bar (summer: 17:00 EDT = 21:00Z roll) ends on the 20:57
    // and 20:58 minutes of 07-08 -- 20:59 did not trade -- so the 20:57
    // bucket holds two minutes and neither its count nor its real end (its
    // bucket END is 21:00, the 20:58 minute ends 20:59) nor the session
    // close completes it. The close reads it: 46.04 by the reconstruction;
    // the engine used to read the 20:54 bucket, 46.97.
    const auto& day = probe.reads[0];
    CHECK(day.chart_ts == utc(2025, 7, 7, 21, 0), "the bar is 07-07 17:00 EDT");
    CHECK(day.off_count == 459, "459 3m buckets published, the 2-minute tail included");
    CHECK(day.last.label == utc(2025, 7, 8, 20, 57), "the close reads the 20:57 bucket");
    CHECK(day.last.subs == 2, "the 20:57 bucket holds 20:57 and 20:58");
    CHECK(within(day.last.close, 3301.72, 1e-9), "its close is the 20:58 minute's (the daily close)");
    CHECK(within(day.last.rsi, 46.0425, kRecon), "the 20:57 bucket's RSI (reconstruction 46.0425)");
    CHECK(day.prev.label == utc(2025, 7, 8, 20, 54) && day.prev.subs == 3,
          "r[1] = the full 20:54 bucket");
    CHECK(within(day.prev.rsi, 46.9703, kRecon), "r[1] = 46.9703, the pre-fix read");
    CHECK(probe.dispatches_off == 459, "one evaluator dispatch per published bucket");
}

// ---- scope: the legacy path without an auxiliary slice is byte-identical --

// The rule is scoped to evaluators served by the auxiliary finer feed. A
// run that aggregates the 1D script bar from the 1m chart input itself (no
// split feed) keeps the aggregator's own completion timing: the Thanksgiving
// 21:57 singleton stays pending until the next session's first sub-bar and
// that daily bar reads the 21:54 bucket, as before this change. A scope pin,
// not a semantic claim about that path.
void test_without_an_auxiliary_slice_the_read_is_unchanged() {
    using namespace split_feed_partial_data;
    FinerRsiProbe probe;
    probe.set_syminfo_timezone("America/New_York");
    probe.set_syminfo_session("1800-1700");
    probe.set_syminfo_type("cfd");
    probe.run(kXau1mNov, count(kXau1mNov), "1", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
    CHECK(probe.reads.size() >= 2, "the dense day and Thanksgiving completed as script bars");
    const auto& thanks = probe.reads[1];
    CHECK(thanks.last.label == utc(2025, 11, 27, 21, 54),
          "legacy path: the Thanksgiving close still reads the 21:54 bucket");
    CHECK(within(thanks.last.rsi, 38.87, kTape), "legacy path: 38.87 as before");
    CHECK(thanks.off_count == 459 + 437, "legacy path: the 21:57 singleton is not yet published");
}

// ---- the aggregator primitive ------------------------------------------------

void test_complete_pending_partial_finalizes_once() {
    // A "3" bucket grid from 1m on the OANDA session clock.
    TimeframeAggregator agg("3", "1", "America/New_York", "1800-1700");
    auto bar = [](int64_t ts, double px) {
        return Bar{px, px, px, px, 1.0, ts};
    };
    CHECK(!agg.has_pending_partial(), "nothing pending before the first bar");
    CHECK(!agg.complete_pending_partial().is_complete, "nothing to finalize before the first bar");

    // The Thanksgiving tail: the 21:56 minute opens the 21:54 bucket ...
    AggregatedBar r = agg.feed(bar(utc(2025, 11, 27, 21, 56), 4156.99));
    CHECK(!r.is_complete && agg.has_pending_partial(), "21:54 opened, partial");
    // ... the 21:59 minute crosses into 21:57: the boundary emits 21:54 and
    // leaves the 21:59 singleton pending, untested by the real-end and
    // session-close rules.
    r = agg.feed(bar(utc(2025, 11, 27, 21, 59), 4158.8));
    CHECK(r.is_complete && r.bar.timestamp == utc(2025, 11, 27, 21, 54) && r.sub_bar_count == 1,
          "the boundary emits the 21:54 singleton");
    CHECK(agg.has_pending_partial(), "the 21:57 singleton is pending");
    r = agg.complete_pending_partial();
    CHECK(r.is_complete && r.bar.timestamp == utc(2025, 11, 27, 21, 57) && r.sub_bar_count == 1,
          "finalized as the 21:57 bucket holding one minute");
    CHECK(within(r.bar.close, 4158.8, 1e-9), "its close is the 21:59 minute's");
    CHECK(agg.last_completed().timestamp == utc(2025, 11, 27, 21, 57), "it is the last completed bucket");
    CHECK(!agg.has_pending_partial(), "nothing pending after the finalize");
    CHECK(!agg.complete_pending_partial().is_complete, "a second finalize is a no-op");
    // The next session's first minute resets the bucket without re-emitting.
    r = agg.feed(bar(utc(2025, 11, 27, 23, 4), 4159.0));
    CHECK(!r.is_complete && r.bar.timestamp == utc(2025, 11, 27, 23, 3) && r.sub_bar_count == 1,
          "the next boundary opens a fresh bucket, nothing re-emitted");

    // The guard: a bucket finalized early that later receives more of its
    // own minutes merges them but never completes a second time, even on
    // its count -- as after any early (real-end / session-close) finalize.
    TimeframeAggregator guard("3", "1", "America/New_York", "1800-1700");
    guard.feed(bar(utc(2025, 11, 27, 21, 57), 1.0));
    CHECK(guard.complete_pending_partial().is_complete, "finalized on one minute");
    r = guard.feed(bar(utc(2025, 11, 27, 21, 58), 2.0));
    CHECK(!r.is_complete && r.sub_bar_count == 2, "a later minute merges without completing");
    r = guard.feed(bar(utc(2025, 11, 27, 21, 59), 3.0));
    CHECK(!r.is_complete && r.sub_bar_count == 3, "the count is reached but the bucket was already emitted");
    CHECK(!guard.has_pending_partial(), "still nothing pending");

    // A dense bucket completes on its count and leaves nothing pending.
    TimeframeAggregator dense("3", "1", "America/New_York", "1800-1700");
    dense.feed(bar(utc(2025, 11, 26, 21, 57), 1.0));
    dense.feed(bar(utc(2025, 11, 26, 21, 58), 2.0));
    r = dense.feed(bar(utc(2025, 11, 26, 21, 59), 3.0));
    CHECK(r.is_complete && r.sub_bar_count == 3, "count completion");
    CHECK(!dense.has_pending_partial(), "no partial after a count completion");

    // CALENDAR and PASSTHROUGH aggregators never report a pending partial.
    TimeframeAggregator daily("D", "1", "America/New_York", "1800-1700");
    daily.feed(bar(utc(2025, 11, 27, 21, 59), 1.0));
    CHECK(!daily.has_pending_partial() && !daily.complete_pending_partial().is_complete,
          "calendar buckets keep their own completion rules");
    TimeframeAggregator same("1", "1", "America/New_York", "1800-1700");
    same.feed(bar(utc(2025, 11, 27, 21, 59), 1.0));
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
