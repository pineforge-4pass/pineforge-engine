/*
 * test_native_script_bucket_completions.cpp -- R5 lane B-ADAPTER, item 4 (M26).
 *
 * A host that aggregates a finer feed into script bars may need, before the
 * first input arrives, how many script bars its span holds and which inputs
 * complete one: the Pine scheduler counts its expected source bars and keys
 * request.security's calling-bar completion on it (PineScheduler::run_begin).
 * It used to run a TimeframeAggregator of its own over the retained bars; the
 * kernel now answers the question for any host:
 * NativeExecutionConsumer::script_bucket_completions, the kernel's timeframe
 * aggregator (the completion rules ADR-0001 section B rules generic, and the
 * bucketing the kernel's own timeframe subscriptions run) under the running
 * spec's timeframes, zone and session.
 *
 * Section 1: the answer equals the preview the scheduler ran -- restated here
 * verbatim from src/source/pine_scheduler_native.cpp@db98990c:130-141 --
 * input for input, on the K1 zones (UTC, New York, Tokyo, London, the POSIX
 * EST5EDT rule, a fixed +05:30), across spring and fall DST, on 24x7, a
 * weekday RTH session, a session with a lunch break, a wrapped weekday
 * session and a three-window session, for intraday targets and 1D, 1W, 1M,
 * with holidays, dropped bars and a feed that ends inside a bucket.
 *
 * Section 2 measures why the answer is the aggregator's and not the
 * calendar's sealing: the per-input completion the consumer reports during a
 * run (NativeInputContext::completes_script_interval: the input interval's
 * next period open reaches the script interval's) differs from the
 * aggregator's on real shapes -- a declared session's last bar closes the
 * clipped bucket for the aggregator while the calendar waits for the next
 * tradable opening.
 */

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_execution_consumer.hpp"

using namespace pineforge;
namespace nc = pineforge::native_calendar;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

class Host : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

NativeRunSpec spec_for(const std::string& zone, const std::string& session,
                       const std::string& script_tf, const std::string& input_tf) {
    NativeRunSpec spec;
    spec.identity.session_key = "b-adapter-m26";
    spec.identity.run_number = 1;
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "stock";
    spec.currency = "USD";
    spec.basecurrency = "";
    spec.timezone = zone;
    spec.session = session;
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

// src/source/pine_scheduler_native.cpp@db98990c:130-141, the preview the
// scheduler ran before this lane, verbatim but for the names.
void preview(const std::vector<Bar>& bars, const NativeRunSpec& spec,
             std::vector<unsigned char>& completes, std::vector<unsigned char>& boundary) {
    completes.assign(bars.size(), 1U);
    boundary.assign(bars.size(), 0U);
    TimeframeAggregator preview(spec.script_tf, spec.input_tf, spec.timezone, spec.session);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const AggregatedBar aggregate = preview.feed(bars[i]);
        completes[i] = aggregate.is_complete ? 1U : 0U;
        boundary[i] = aggregate.is_complete
            && tf_change(aggregate.bar.timestamp, bars[i].timestamp, spec.script_tf,
                         spec.timezone, spec.session) ? 1U : 0U;
    }
}

// The feed: every input interval's open the calendar puts in session between
// `start` and `end`, less holidays (whole session days) and dropped bars. A
// minute input steps its own grid and keeps the in-session opens; a daily
// input asks the calendar once per civil day for the session day holding it.
std::vector<Bar> feed(const nc::SessionCalendar& calendar, const nc::Timeframe& input,
                      std::int64_t start, std::int64_t end, std::mt19937_64& rng, bool gaps) {
    std::vector<std::int64_t> opens;
    if (input.unit() == nc::TimeframeUnit::Minute) {
        const std::int64_t step = 60000LL * input.count();
        for (std::int64_t t = start; t < end; t += step) {
            if (nc::in_session(calendar, t)) opens.push_back(t);
        }
    } else {
        for (std::int64_t day = start; day < end; day += 86400000LL) {
            const auto interval = nc::interval_containing(calendar, input, day + 43200000LL);
            if (interval && interval->open_ms >= start && interval->open_ms < end
                && (opens.empty() || interval->open_ms > opens.back())) {
                opens.push_back(interval->open_ms);
            }
        }
    }
    std::vector<Bar> out;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::int64_t holiday = -1;
    for (const std::int64_t t : opens) {
        const std::int64_t day = t / 86400000LL;
        if (gaps && day != holiday && unit(rng) < 0.04) holiday = day;
        if (gaps && (day == holiday || unit(rng) < 0.03)) continue;
        Bar bar;
        bar.timestamp = t;
        bar.open = bar.high = bar.low = bar.close = 100.0;
        bar.volume = 1.0;
        out.push_back(bar);
    }
    return out;
}

struct Span {
    const char* name;
    std::int64_t start;
    std::int64_t end;
};

void the_kernel_answers_the_preview() {
    const std::vector<std::string> zones = {
        "UTC", "America/New_York", "Asia/Tokyo", "Europe/London",
        "EST5EDT,M3.2.0,M11.1.0", "UTC+05:30"};
    const std::vector<std::string> sessions = {
        "24x7", "0930-1600:23456", "0900-1130,1230-1500", "1800-1700:23456",
        "0000-0230,0330-0600,1300-2400"};
    const std::vector<std::pair<std::string, std::string>> tfs = {
        {"15", "5"}, {"60", "15"}, {"240", "15"}, {"D", "60"}, {"W", "D"}, {"M", "D"}};
    // Intraday targets: a few days around each switch (New York and the POSIX
    // rule 03-09, London 10-26: one switch each way). Calendar targets: six
    // weeks of daily or hourly inputs over both switches, so days, weeks and
    // months complete many times.
    const Span intraday_spans[] = {
        {"US spring", 1741305600000LL, 1741737600000LL},      // 03-07 .. 03-12
        {"UK fall", 1761264000000LL, 1761696000000LL},        // 10-24 .. 10-29
    };
    const Span calendar_spans[] = {
        {"spring", 1740787200000LL, 1744675200000LL},         // 03-01 .. 04-15
        {"fall", 1760486400000LL, 1764547200000LL},           // 10-15 .. 12-01
    };
    std::mt19937_64 rng(0xB0ADA77E5ULL);
    long configurations = 0;
    long inputs = 0;
    long completions = 0;
    long boundaries = 0;
    for (const auto& zone : zones) {
        for (const auto& session : sessions) {
            const auto calendar = nc::parse_session(session, zone);
            CHECK(calendar.has_value());
            if (!calendar) continue;
            for (const auto& tf : tfs) {
                const auto input = nc::parse_timeframe(tf.second);
                if (!input) continue;
                const auto target = nc::parse_timeframe(tf.first);
                const bool calendar_target = target && target->is_calendar();
                std::vector<Span> spans;
                if (calendar_target) spans.assign(std::begin(calendar_spans), std::end(calendar_spans));
                else spans.assign(std::begin(intraday_spans), std::end(intraday_spans));
                const NativeRunSpec spec = spec_for(zone, session, tf.first, tf.second);
                Host host;
                const auto setup = host.configure_native(spec);
                CHECK(setup.status == NativeSetupStatus::Applied);
                if (setup.status != NativeSetupStatus::Applied) {
                    std::printf("  configure_native %s %s %s<-%s: %s\n", zone.c_str(),
                                session.c_str(), tf.first.c_str(), tf.second.c_str(),
                                host.last_error().c_str());
                    continue;
                }
                for (const Span& span : spans) {
                    for (const bool gaps : {false, true}) {
                        auto bars = feed(*calendar, *input, span.start, span.end, rng, gaps);
                        // End inside a bucket: drop the feed's last few bars.
                        if (bars.size() > 12) bars.resize(bars.size() - bars.size() / 97 - 1);
                        if (bars.size() < 4) continue;
                        std::vector<unsigned char> want_complete, want_boundary;
                        preview(bars, spec, want_complete, want_boundary);
                        std::vector<unsigned char> got_complete, got_boundary;
                        const bool answered = NativeExecutionConsumer::bound(host)
                            .script_bucket_completions(bars.data(), bars.size(), got_complete,
                                                       got_boundary);
                        CHECK(answered);
                        CHECK(got_complete == want_complete);
                        CHECK(got_boundary == want_boundary);
                        if (got_complete != want_complete || got_boundary != want_boundary) {
                            std::printf("  mismatch %s '%s' %s<-%s %s gaps=%d\n", zone.c_str(),
                                        session.c_str(), tf.first.c_str(), tf.second.c_str(),
                                        span.name, gaps ? 1 : 0);
                        }
                        ++configurations;
                        inputs += static_cast<long>(bars.size());
                        for (std::size_t i = 0; i < bars.size(); ++i) {
                            completions += got_complete[i];
                            boundaries += got_boundary[i];
                        }
                    }
                }
            }
        }
    }
    std::printf("  %ld configurations, %ld inputs: %ld script bars completed, %ld of them at"
                " the next bucket's first bar, each equal to the preview\n",
                configurations, inputs, completions, boundaries);
    CHECK(configurations >= 500);
    CHECK(boundaries > 0);
    // A host with no spec gets no answer and keeps what it holds.
    Host bare;
    std::vector<unsigned char> untouched(3, 7U), also(3, 7U);
    const Bar bar{};
    CHECK(!NativeExecutionConsumer::bound(bare).script_bucket_completions(&bar, 1, untouched,
                                                                           also));
    CHECK(untouched == std::vector<unsigned char>(3, 7U));
}

// The calendar's sealing, as the consumer reports it per input during a run
// (native_execution_consumer.cpp: completes_script_interval = input next
// period open >= script next period open), against the aggregator on a UTC
// weekday 0930-1600 session with 15-minute inputs and 60-minute script bars:
// the 15:45 input is the session's last, and the aggregator completes the
// clipped 15:00 bucket there (the session-close rule), while the calendar
// reports the bucket open until the next tradable opening.
void the_calendar_sealing_is_not_the_answer() {
    const NativeRunSpec spec = spec_for("UTC", "0930-1600:23456", "60", "15");
    const auto calendar = nc::parse_session(spec.session, spec.timezone);
    const auto script = nc::parse_timeframe(spec.script_tf);
    const auto input = nc::parse_timeframe(spec.input_tf);
    CHECK(calendar && script && input);
    if (!calendar || !script || !input) return;
    std::mt19937_64 rng(1);
    // Monday 2025-03-03 .. Friday 2025-03-07, UTC.
    const auto bars = feed(*calendar, *input, 1740960000000LL, 1741392000000LL, rng, false);
    std::vector<unsigned char> complete, boundary;
    preview(bars, spec, complete, boundary);
    int differ = 0;
    std::int64_t first = 0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto in = nc::interval_containing(*calendar, *input, bars[i].timestamp);
        const auto sc = nc::interval_containing(*calendar, *script, bars[i].timestamp);
        if (!in || !sc) continue;
        const bool sealed = in->next_period_open_ms >= sc->next_period_open_ms;
        if (sealed != (complete[i] != 0U)) {
            if (!differ) first = bars[i].timestamp;
            ++differ;
        }
    }
    std::printf("  UTC 0930-1600 60<-15, %zu inputs: the calendar's per-input sealing differs"
                " from the aggregator's completion on %d of them (first at %lld)\n",
                bars.size(), differ, static_cast<long long>(first));
    CHECK(differ > 0);
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("the kernel answers the preview the scheduler ran", the_kernel_answers_the_preview);
    test("the calendar's sealing is not the answer", the_calendar_sealing_is_not_the_answer);
    std::printf("R5 B-ADAPTER script bucket completions: %d checks, %d failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
