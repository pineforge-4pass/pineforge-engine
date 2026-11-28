// An empty request.security timeframe reads the chart's own series when the
// request is fed bars finer than the chart.
//
// Target (one): the bars a plain request.security site is evaluated on, and
// the requested bar each chart bar's script body reads, when the site's
// timeframe is the empty string at run time and the host feeds requests from
// a 1-minute feed beside a 15-minute chart.
//
// In Pine an empty request timeframe names the chart's timeframe: the
// requested series is the chart's, one requested bar per chart bar. A script
// reaches the engine with an empty string when its timeframe is an input left
// at its default, tf = input.timeframe(""): generated code registers such a
// site with the input's getter, which returns "" when no input is overridden.
//
// The run is the source host on a 15-minute chart with a 1-minute request
// feed attached, and the one request site such a script has
// (request.security(syminfo.tickerid, tf, <expression>), lookahead off, gaps
// off). Two tapes, both invented:
//   session  a symbol that trades 09:30 to 16:00 New York, two trading days;
//   allday   a symbol that trades all day, twelve hours across a UTC midnight.
// On each tape the site is registered twice, in two separate runs:
//   empty    the way generated code registers an input.timeframe("") left at
//            its default: the input's getter, here "";
//   15       the same request with the chart's own timeframe written out.
// The second is the control: both spellings name one Pine request, so both
// must read the same bars.
//
// Every minute of the feed has its own open, and a chart bar is exactly its
// fifteen minutes (first open, highest high, lowest low, last close, summed
// volume), so the bar a chart bar reads says which bars it was built from.
// The expected values are the chart's own bars; nothing is read back from the
// engine to form them.
//
// Each run ends with
//   summary tape=<name> spelling=<empty|15> chart_bars=.. evaluations=..
//           evaluated_once=.. reads_own_bar=.. first_other_bar=..
//           first_other_label_offset_ms=..
// evaluated_once counts chart bars the site was evaluated on exactly once,
// with a completed bar; reads_own_bar counts chart bars whose body read that
// chart bar itself; first_other_bar is the first chart bar that read another
// bar (-1: none) and first_other_label_offset_ms how far that bar's label
// lies after the chart bar's open.

#include <pineforge/pineforge.h>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the empty-request-timeframe test drives the auxiliary security feed"
#endif

namespace {

using namespace pineforge;
namespace nc = pineforge::native_calendar;

int failures = 0;
long long checks = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);        \
    }                                                                          \
} while (0)

constexpr std::int64_t kMinuteMs = 60000;
constexpr int kMinutesPerChartBar = 15;

struct Tape {
    const char* name;
    const char* timezone;
    const char* session;
    const char* type;
    // The request feed, and the chart built from it.
    std::vector<Bar> minutes;
    std::vector<Bar> chart;
};

// Minute g of a tape opens at 1000 + 2g and closes one above it. Every value
// is a multiple of one half, so sums and comparisons are exact.
Bar minute_bar(std::size_t g, std::int64_t open_ms) {
    const double first = 1000.0 + 2.0 * static_cast<double>(g);
    return Bar{first, first + 1.5, first - 0.5, first + 1.0,
               1.0 + static_cast<double>(g % 7), open_ms};
}

// `chart_bars` consecutive 15-minute chart bars from `open_ms`, each with its
// fifteen 1-minute bars.
void append_span(Tape& tape, std::int64_t open_ms, int chart_bars) {
    for (int j = 0; j < chart_bars; ++j) {
        Bar chart{};
        for (int k = 0; k < kMinutesPerChartBar; ++k) {
            const std::int64_t minute_ms = open_ms
                + (static_cast<std::int64_t>(j) * kMinutesPerChartBar + k) * kMinuteMs;
            const Bar minute = minute_bar(tape.minutes.size(), minute_ms);
            if (k == 0) {
                chart = minute;
            } else {
                if (minute.high > chart.high) chart.high = minute.high;
                if (minute.low < chart.low) chart.low = minute.low;
                chart.close = minute.close;
                chart.volume += minute.volume;
            }
            tape.minutes.push_back(minute);
        }
        tape.chart.push_back(chart);
    }
}

bool make_tapes(std::vector<Tape>& tapes) {
    // Wednesday 2025-06-04 and Thursday 2025-06-05, 26 chart bars a day.
    Tape session{"session", "America/New_York", "0930-1600", "stock", {}, {}};
    for (int day = 4; day <= 5; ++day) {
        const auto day_start = nc::resolve_civil(session.timezone, 2025, 6, day, 9, 30);
        CHECK(day_start.has_value());
        if (!day_start) return false;
        append_span(session, day_start->epoch_ms, 26);
    }
    // 2025-06-04 18:00 UTC to 2025-06-05 06:00 UTC.
    Tape allday{"allday", "UTC", "24x7", "crypto", {}, {}};
    const auto start = nc::resolve_civil(allday.timezone, 2025, 6, 4, 18, 0);
    CHECK(start.has_value());
    if (!start) return false;
    append_span(allday, start->epoch_ms, 48);
    tapes.push_back(std::move(session));
    tapes.push_back(std::move(allday));
    return true;
}

// The one request site, registered the way generated code registers it.
// Every evaluation is kept with the chart bar it arrived on, and each chart
// bar's body keeps the last completed requested bar, which is what the script
// reads there.
class RequestHost final : public source::PineStrategyHost {
public:
    struct Evaluation {
        long long chart_index = 0;
        bool complete = false;
    };
    struct Read {
        bool any = false;
        Bar bar{};
    };

    explicit RequestHost(bool chart_timeframe_written) : written_(chart_timeframe_written) {}

    std::vector<Evaluation> evaluations;
    std::vector<Read> reads;
    long long other_sites = 0;

private:
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        if (written_) {
            register_security_eval(0, "15", input_tf_, false, false);
        } else {
            // tf = input.timeframe("", "Request timeframe"), not overridden.
            register_security_eval(0, get_input_string("Request timeframe", ""), input_tf_,
                                   false, false);
        }
    }

    // A request's bar arrives before the body of the chart bar it belongs to:
    // bars_seen_ is that chart bar's index.
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0) {
            ++other_sites;
            return;
        }
        Evaluation evaluation;
        evaluation.chart_index = bars_seen_;
        evaluation.complete = is_complete;
        evaluations.push_back(evaluation);
        if (is_complete) {
            latest_.any = true;
            latest_.bar = bar;
        }
    }

    void clear_security(int) override {}

    void on_source_bar(const Bar&) override {
        reads.push_back(latest_);
        ++bars_seen_;
    }

    bool written_;
    long long bars_seen_ = 0;
    Read latest_{};
};

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

void run_case(const Tape& tape, bool chart_timeframe_written) {
    const char* spelling = chart_timeframe_written ? "15" : "empty";
    const std::size_t bars = tape.chart.size();
    // Not vacuous: whole chart bars, each with its fifteen minutes.
    CHECK(bars >= 48);
    CHECK(tape.minutes.size() == bars * static_cast<std::size_t>(kMinutesPerChartBar));

    RequestHost host(chart_timeframe_written);
    host.set_syminfo_timezone(tape.timezone);
    host.set_syminfo_session(tape.session);
    host.set_syminfo_type(tape.type);
    CHECK(host.set_aux_security_feed(tape.minutes.data(),
                                     static_cast<int>(tape.minutes.size()), "1"));
    host.run(tape.chart.data(), static_cast<int>(bars), "15", "15");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  run failed: %s\n", host.last_error().c_str());
    }
    CHECK(host.other_sites == 0);
    CHECK(host.reads.size() == bars);

    // Evaluations per chart bar, and whether every one of them was complete.
    std::vector<int> per_bar(bars, 0);
    std::vector<int> partial(bars, 0);
    for (const RequestHost::Evaluation& evaluation : host.evaluations) {
        if (evaluation.chart_index < 0
            || evaluation.chart_index >= static_cast<long long>(bars)) {
            continue;
        }
        const std::size_t i = static_cast<std::size_t>(evaluation.chart_index);
        ++per_bar[i];
        if (!evaluation.complete) ++partial[i];
    }
    std::size_t evaluated_once = 0;
    std::size_t reads_own_bar = 0;
    long long first_other_bar = -1;
    long long first_other_label_offset_ms = 0;
    for (std::size_t i = 0; i < bars; ++i) {
        if (per_bar[i] == 1 && partial[i] == 0) ++evaluated_once;
        const bool own = i < host.reads.size() && host.reads[i].any
            && same_bar(host.reads[i].bar, tape.chart[i]);
        if (own) {
            ++reads_own_bar;
        } else if (first_other_bar < 0) {
            first_other_bar = static_cast<long long>(i);
            if (i < host.reads.size() && host.reads[i].any) {
                first_other_label_offset_ms = static_cast<long long>(
                    host.reads[i].bar.timestamp - tape.chart[i].timestamp);
            }
        }
    }
    std::printf("summary tape=%s spelling=%s chart_bars=%zu evaluations=%zu "
                "evaluated_once=%zu reads_own_bar=%zu first_other_bar=%lld "
                "first_other_label_offset_ms=%lld\n", tape.name, spelling, bars,
                host.evaluations.size(), evaluated_once, reads_own_bar, first_other_bar,
                first_other_label_offset_ms);

    // One requested bar per chart bar: the site is evaluated once on every
    // chart bar, with that bar completed, and never in between.
    CHECK(host.evaluations.size() == bars);
    CHECK(evaluated_once == bars);
    // The requested series is the chart's: every chart bar's body reads that
    // chart bar itself, by its open time and its five values.
    CHECK(reads_own_bar == bars);
}

}  // namespace

int main() {
    std::vector<Tape> tapes;
    const bool built = make_tapes(tapes);
    CHECK(built);
    CHECK(tapes.size() == 2);
    int cases = 0;
    for (const Tape& tape : tapes) {
        // The control first: the chart's own timeframe written out.
        run_case(tape, true);
        run_case(tape, false);
        cases += 2;
    }
    std::printf("test_empty_request_timeframe: %d cases, %lld checks, %d failed\n", cases, checks,
                failures);
    return failures == 0 ? 0 : 1;
}
