// R5 lane PERF-KEDGE: session-day facts on a UTC calendar, without resolving
// a calendar interval per bar.
//
// Wave G's KERNEL-EDGE made present_session_day read each instant -- the
// bar's label and its neighbours -- at the first eligible instant of its
// script interval, and resolved that interval from the calendar at every
// bar. On FeedTolerant labels nothing else in the run asks the calendar for
// an interval, so the Pine adapter's 100 public benchmark slots ran a median
// 11.9 % more instructions (INT25, finding 1). Lane PERF-KEDGE reads an
// instant that is itself in session directly when the calendar is UTC: its
// session days are the fixed daily cycles, which tile time, so the instant,
// its interval's open and that first eligible instant share the one session
// day holding the instant, and the eligible instant is in session on it. An
// instant out of session, and every instant of any other zone, is still read
// through its interval, in the order the lane's base read it.
//
// This witness checks that the shortcut changes no fact and keeps the
// calendar off the per-bar path:
//   1. utc_calendar is the calendar's own UTC decision: "", UTC, Etc/UTC,
//      GMT, Etc/GMT and the zero offsets that normalise to UTC are UTC; the
//      POSIX zone UTC0 -- the same clock through the libc zone path -- a
//      fixed offset and named zones are not.
//   2. The same tapes under "UTC" (the shortcut) and "UTC0" (every interval
//      resolved, as at the lane's base) present identical facts at every
//      decision point a host reads them at -- bar open, bar, fill
//      recalculation, applied execution and realtime print -- across 24x7,
//      plain, lunch-break (a 60-minute label inside the break reopens at
//      12:30), overnight, weekday-masked and late-origin sessions, script
//      timeframes over finer inputs, Canonical and FeedTolerant labels with
//      gaps, off-grid provider stamps and stamps inside a declared break,
//      batch runs, and confirmed-bar and printed streams -- and on the other
//      delivery paths: the magnifier's synthesized path, a bucket that does
//      not divide the session day, a seconds timeframe and an undetected one.
//      Both runs must also end with the same error text (usually none).
//   3. Cost: a UTC FeedTolerant batch -- the Pine adapter's shape -- resolves
//      at most one interval for its facts (its final bar's scheduled close),
//      where the UTC0 run of the same bars resolves at least one a bar, which
//      is what every UTC run cost at the lane's base.
//
// Fail-before: at the lane's base native_calendar::utc_calendar and
// SessionDayMemo::interval_resolutions do not exist, so this TU does not
// compile there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_calendar_memo.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    static std::uint64_t interval_resolutions(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_memo_.interval_resolutions();
    }
    static bool calendar_is_utc(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_is_utc();
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

using namespace pineforge;
namespace nc = pineforge::native_calendar;
namespace no = pineforge::native_order;
using Probe = NativeExecutionConsumerProbe;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr std::int64_t kSecond = 1000;
constexpr std::int64_t kMinute = 60 * kSecond;
constexpr std::int64_t kDay = 1440 * kMinute;
// Friday 2025-07-04 00:00 UTC: the tapes run Friday through Tuesday, so every
// one crosses a weekend and a weekday mask has days to drop.
constexpr std::int64_t kFri = 1751587200000LL;

// ── 1. the UTC decision ────────────────────────────────────────────────────

void test_utc_decision() {
    std::printf("test_utc_decision\n");
    for (const char* zone : {"", "UTC", "Etc/UTC", "GMT", "Etc/GMT", "UTC+0", "UTC-00:00"}) {
        const auto calendar = nc::parse_session("24x7", zone);
        CHECK(calendar.has_value());
        if (!calendar) continue;
        nc::SessionDayMemo memo;
        CHECK(nc::utc_calendar(*calendar, memo));
    }
    for (const char* zone : {"UTC0", "GMT0", "UTC+3", "UTC-05:30", "America/New_York",
                             "Asia/Tokyo", "Europe/London"}) {
        const auto calendar = nc::parse_session("24x7", zone);
        CHECK(calendar.has_value());
        if (!calendar) continue;
        nc::SessionDayMemo memo;
        CHECK(!nc::utc_calendar(*calendar, memo));
    }
}

// ── 2. the same facts, UTC against UTC0 ────────────────────────────────────

struct Facts {
    std::int64_t ts = 0;
    bool in = false;
    bool opens = false;
    bool closes = false;
    bool open_ended = false;
    bool operator==(const Facts& o) const {
        return ts == o.ts && in == o.in && opens == o.opens && closes == o.closes
            && open_ended == o.open_ended;
    }
};

Facts facts_of(const NativeDecisionContext& c) {
    return {c.script_bar_open_ms, c.in_session, c.opens_session_day, c.closes_session_day,
            c.closes_session_day_open_ended};
}

class Host final : public NativeStrategyHost {
public:
    std::vector<Facts> bar_opens, bars, refills, applied, ticks;
    int calculations = 0;
    void on_native_bar_open(const Bar&, const NativeDecisionContext& c) override {
        bar_opens.push_back(facts_of(c));
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& c) override {
        bars.push_back(facts_of(c));
        // A market order every fifth calculation, alternating sides, so fills
        // and their recalculations read facts too.
        if (++calculations % 5 == 0) {
            const double units = (calculations / 5) % 2 ? 1.0 : -1.0;
            (void)submit(no::Request{no::Transact{units}, "utc-" + std::to_string(calculations), ""});
        }
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& c,
                               NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        if (reason == NativeCalculationReason::OrderFill) {
            refills.push_back(facts_of(c));
            return;
        }
        NativeStrategyHost::on_native_recalculate(bar, c, reason, cause);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext& c) override {
        applied.push_back(facts_of(c));
    }
    void on_native_tick(const Bar&, const NativeTickContext& c) override {
        ticks.push_back(facts_of(c.decision));
    }
};

struct Session {
    const char* text;
    const char* name;
};

// Every shape the facts read differently: a whole day, a plain window, a
// declared break, a session crossing midnight, a weekday mask, and an origin
// half an hour before midnight.
const Session kSessions[] = {
    {"24x7", "24x7"},
    {"0930-1600", "plain"},
    {"0900-1130,1230-1500", "break"},
    {"1700-1600", "overnight"},
    {"0930-1600:23456", "weekdays"},
    {"2330-2300", "late-origin"},
};

struct Pairing {
    const char* input;
    const char* script;
    std::int64_t step;  // the input step
};

const Pairing kPairings[] = {
    {"5", "5", 5 * kMinute},    {"10", "10", 10 * kMinute}, {"3", "15", 3 * kMinute},
    {"30", "60", 30 * kMinute}, {"60", "60", 60 * kMinute}, {"15", "240", 15 * kMinute},
};

enum class Tape { Whole, Days, Gaps, OffGrid, InBreak };
const char* tape_name(Tape tape) {
    switch (tape) {
    case Tape::Whole: return "whole";
    case Tape::Days: return "whole-days";
    case Tape::Gaps: return "gaps";
    case Tape::OffGrid: return "off-grid";
    case Tape::InBreak: return "in-break";
    }
    return "?";
}

// Both streams end with stream_end(true), which also seals and presents a
// final bucket the calendar has closed.
enum class Mode { Batch, Stream, Prints };
const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Batch: return "batch";
    case Mode::Stream: return "stream";
    case Mode::Prints: return "prints";
    }
    return "?";
}

Bar bar_at(std::int64_t ts, int i) {
    const double price = 100.0 + 0.25 * static_cast<double>(i % 11);
    return {price, price + 0.75, price - 0.75, price + 0.25, 1.0, ts};
}

// Input bars on the input grid wherever the session is open, over four and a
// half days -- Days stops at Tuesday 00:00 UTC instead, so a batch's final bar
// and (see warmup_for) a stream's last warmup bar end a session day on the
// sessions that roll at midnight or close before it. Gaps drops every seventh
// bar and the whole of Monday; OffGrid moves every third stamp a little inside
// its own slot (FeedTolerant only); InBreak adds a stamp inside each day's
// closed time (FeedTolerant only).
std::vector<Bar> tape_for(const nc::SessionCalendar& calendar, const Pairing& pairing, Tape tape,
                          std::int64_t span = 4 * kDay + 12 * 60 * kMinute) {
    std::vector<Bar> bars;
    const std::int64_t first = kFri;
    const std::int64_t last = kFri + (tape == Tape::Days ? 4 * kDay : span);
    const std::int64_t nudge = pairing.step >= 5 * kMinute ? 2 * kMinute : 20 * kSecond;
    int i = 0;
    for (std::int64_t ts = first; ts < last; ts += pairing.step) {
        const bool open = nc::in_session(calendar, ts);
        if (!open) {
            // One stamp a day inside declared closed time, off the input grid.
            if (tape == Tape::InBreak && (ts / pairing.step) % 97 == 0) {
                bars.push_back(bar_at(ts + nudge, i++));
            }
            continue;
        }
        if (tape == Tape::Gaps && (i % 7 == 3 || (ts >= kFri + 3 * kDay && ts < kFri + 4 * kDay))) {
            ++i;
            continue;
        }
        const std::int64_t stamp = tape == Tape::OffGrid && i % 3 == 1 ? ts + nudge : ts;
        bars.push_back(bar_at(stamp, i++));
    }
    return bars;
}

// Input bars at the calendar's own slot labels for `tf` wherever the slot has
// an instant in session: the grid restarts at every session-day origin, which
// a timeframe that does not divide the day needs for Canonical labels.
std::vector<Bar> calendar_grid_tape(const nc::SessionCalendar& calendar, const char* tf,
                                    std::int64_t span) {
    const auto timeframe = nc::parse_timeframe(tf);
    std::vector<Bar> bars;
    int i = 0;
    for (std::int64_t t = kFri; timeframe && t < kFri + span;) {
        const auto interval = nc::interval_containing(calendar, *timeframe, t);
        if (!interval || interval->next_period_open_ms <= t) break;
        if (interval->last_traded_close_ms > interval->eligible_open_ms)
            bars.push_back(bar_at(interval->open_ms, i++));
        t = interval->next_period_open_ms;
    }
    return bars;
}

// A stream's warmup: half the tape, or on Days every bar before Sunday 00:00.
int warmup_for(const std::vector<Bar>& bars, Tape tape) {
    if (tape != Tape::Days) return static_cast<int>(bars.size()) / 2;
    int warmup = 0;
    while (warmup < static_cast<int>(bars.size())
           && bars[static_cast<std::size_t>(warmup)].timestamp < kFri + 2 * kDay) ++warmup;
    return warmup;
}

NativeRunSpec spec_for(const Session& session, const char* zone, const Pairing& pairing,
                       NativeSlotLabelPolicy labels) {
    NativeRunSpec spec;
    spec.identity = {"perf-kedge-utc", 1};
    spec.input_tf = pairing.input;
    spec.script_tf = pairing.script;
    spec.timeframe_undetected = spec.input_tf.empty();
    spec.tickerid = "TEST:KEDGE";
    spec.timezone = zone;
    spec.session = session.text;
    spec.slot_label_policy = labels;
    spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

struct Outcome {
    std::vector<Facts> bar_opens, bars, refills, applied, ticks;
    std::string error;
    std::uint64_t resolutions = 0;
    bool utc = false;
};

Outcome drive(const NativeRunSpec& spec, const std::vector<Bar>& bars, Mode mode,
              int warmup) {
    Host host;
    Outcome out;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        out.error = "configure: " + host.last_error();
        return out;
    }
    const int n = static_cast<int>(bars.size());
    switch (mode) {
    case Mode::Batch:
        host.run(bars.data(), n);
        break;
    case Mode::Stream:
        if (host.stream_begin(bars.data(), warmup, spec.input_tf, spec.script_tf)) {
            for (int i = warmup; i < n; ++i) {
                if (!host.stream_push_bar(bars[static_cast<std::size_t>(i)])) break;
            }
            (void)host.stream_end(true);
        }
        break;
    case Mode::Prints:
        if (host.stream_begin(bars.data(), warmup, spec.input_tf, spec.script_tf)) {
            // Thirty hours of prints: at least one session-day boundary on
            // every session, at a bounded cost.
            std::uint64_t sequence = 1;
            bool ok = true;
            const std::int64_t until = warmup < n
                ? bars[static_cast<std::size_t>(warmup)].timestamp + 30 * 60 * kMinute : 0;
            for (int i = warmup; ok && i < n && bars[static_cast<std::size_t>(i)].timestamp < until; ++i) {
                const Bar& b = bars[static_cast<std::size_t>(i)];
                std::int64_t offset = 0;
                for (const double price : {b.open, b.high, b.low, b.close}) {
                    ok = host.stream_push_tick(TradeTick{b.timestamp + offset, sequence++, price, 0.25});
                    if (!ok) break;
                    offset += 5 * kSecond;
                }
            }
            (void)host.stream_end(true);
        }
        break;
    }
    out.bar_opens = host.bar_opens;
    out.bars = host.bars;
    out.refills = host.refills;
    out.applied = host.applied;
    out.ticks = host.ticks;
    out.error = host.last_error();
    out.resolutions = Probe::interval_resolutions(host);
    out.utc = Probe::calendar_is_utc(host);
    return out;
}

struct Tally {
    int runs = 0;
    int completed = 0;
    std::uint64_t facts = 0;
    std::uint64_t in = 0, out = 0, opens = 0, closes = 0, ticks = 0, refills = 0;
    int mismatches = 0;
};

void count(Tally& tally, const Outcome& o) {
    for (const auto* seen : {&o.bar_opens, &o.bars, &o.refills, &o.applied, &o.ticks}) {
        for (const Facts& f : *seen) {
            ++tally.facts;
            f.in ? ++tally.in : ++tally.out;
            tally.opens += f.opens;
            tally.closes += f.closes;
        }
    }
    tally.ticks += o.ticks.size();
    tally.refills += o.refills.size();
}

void test_same_facts_as_the_zone_path() {
    std::printf("test_same_facts_as_the_zone_path\n");
    Tally tally;
    for (const Session& session : kSessions) {
        const auto calendar = nc::parse_session(session.text, "UTC");
        CHECK(calendar.has_value());
        if (!calendar) continue;
        for (const Pairing& pairing : kPairings) {
            for (const auto labels : {NativeSlotLabelPolicy::Canonical,
                                      NativeSlotLabelPolicy::FeedTolerant}) {
                for (const Tape tape : {Tape::Whole, Tape::Days, Tape::Gaps, Tape::OffGrid,
                                        Tape::InBreak}) {
                    if (labels == NativeSlotLabelPolicy::Canonical
                        && (tape == Tape::OffGrid || tape == Tape::InBreak)) continue;
                    const auto bars = tape_for(*calendar, pairing, tape);
                    const int warmup = warmup_for(bars, tape);
                    for (const Mode mode : {Mode::Batch, Mode::Stream, Mode::Prints}) {
                        const Outcome utc = drive(spec_for(session, "UTC", pairing, labels), bars,
                                                  mode, warmup);
                        const Outcome zone = drive(spec_for(session, "UTC0", pairing, labels), bars,
                                                   mode, warmup);
                        ++tally.runs;
                        const bool same = utc.bar_opens == zone.bar_opens && utc.bars == zone.bars
                            && utc.refills == zone.refills && utc.applied == zone.applied
                            && utc.ticks == zone.ticks && utc.error == zone.error;
                        CHECK(utc.utc);
                        CHECK(!zone.utc);
                        CHECK(same);
                        if (!same && ++tally.mismatches <= 5) {
                            std::fprintf(stderr, "  MISMATCH %s %s->%s %s %s %s: bars %zu/%zu error '%s' / '%s'\n",
                                         session.name, pairing.input, pairing.script,
                                         labels == NativeSlotLabelPolicy::Canonical ? "canonical" : "tolerant",
                                         tape_name(tape), mode_name(mode), utc.bars.size(),
                                         zone.bars.size(), utc.error.c_str(), zone.error.c_str());
                        }
                        if (utc.error.empty() && !utc.bars.empty()) ++tally.completed;
                        count(tally, utc);
                    }
                }
            }
        }
    }
    std::printf("  %d runs (%d completed with bars), %llu facts: %llu in session, %llu out, "
                "%llu opens, %llu closes, %llu prints, %llu fill recalculations; %d mismatches\n",
                tally.runs, tally.completed, static_cast<unsigned long long>(tally.facts),
                static_cast<unsigned long long>(tally.in), static_cast<unsigned long long>(tally.out),
                static_cast<unsigned long long>(tally.opens), static_cast<unsigned long long>(tally.closes),
                static_cast<unsigned long long>(tally.ticks), static_cast<unsigned long long>(tally.refills),
                tally.mismatches);
    // Not vacuous: most runs complete, and every kind of fact and decision
    // point is reached.
    CHECK(tally.completed * 4 >= tally.runs * 3);
    CHECK(tally.in > 0 && tally.out > 0);
    CHECK(tally.opens > 0 && tally.closes > 0);
    CHECK(tally.ticks > 0 && tally.refills > 0);
}

// The other paths a script bar is delivered on: the magnifier's synthesized
// path (deliver_intrabar_script), over a chart and an aggregated bar, a
// 7-minute bucket, which does not divide a session day, so the day's last
// bucket runs past the next origin (its bars at the calendar's own labels), a
// 30-second timeframe, and an undetected timeframe, which takes one bar and
// partitions by its raw label -- batch and stream (the undetected one batch
// only), UTC against UTC0.
void test_other_delivery_paths() {
    std::printf("test_other_delivery_paths\n");
    struct Variant {
        const char* name;
        Pairing pairing;
        bool magnifier;
        bool calendar_grid;
        std::int64_t span;
    };
    const Variant variants[] = {
        {"magnifier", {"5", "5", 5 * kMinute}, true, false, 2 * kDay + 12 * 60 * kMinute},
        {"magnifier-aggregated", {"5", "15", 5 * kMinute}, true, false, 2 * kDay + 12 * 60 * kMinute},
        {"7-minute", {"7", "7", 7 * kMinute}, false, true, 2 * kDay + 12 * 60 * kMinute},
        {"30-second", {"30S", "30S", 30 * kSecond}, false, false, kDay + 2 * 60 * kMinute},
        {"undetected", {"", "", 15 * kMinute}, false, false, 12 * 60 * kMinute},
    };
    int runs = 0, completed = 0, mismatches = 0;
    for (const Session& session : {kSessions[0], kSessions[2], kSessions[3]}) {
        const auto calendar = nc::parse_session(session.text, "UTC");
        CHECK(calendar.has_value());
        if (!calendar) continue;
        for (const Variant& variant : variants) {
            auto bars = variant.calendar_grid
                ? calendar_grid_tape(*calendar, variant.pairing.input, variant.span)
                : tape_for(*calendar, variant.pairing, Tape::Whole, variant.span);
            if (variant.pairing.input[0] == '\0' && bars.size() > 1) bars.resize(1);
            for (const auto labels : {NativeSlotLabelPolicy::Canonical,
                                      NativeSlotLabelPolicy::FeedTolerant}) {
                for (const Mode mode : {Mode::Batch, Mode::Stream}) {
                    if (variant.pairing.input[0] == '\0' && mode != Mode::Batch) continue;
                    auto utc_spec = spec_for(session, "UTC", variant.pairing, labels);
                    auto zone_spec = spec_for(session, "UTC0", variant.pairing, labels);
                    if (variant.magnifier) {
                        utc_spec.intrabar.value = IntrabarPath::synthesized{};
                        zone_spec.intrabar.value = IntrabarPath::synthesized{};
                    }
                    const int warmup = static_cast<int>(bars.size()) / 2;
                    const Outcome utc = drive(utc_spec, bars, mode, warmup);
                    const Outcome zone = drive(zone_spec, bars, mode, warmup);
                    ++runs;
                    const bool same = utc.bar_opens == zone.bar_opens && utc.bars == zone.bars
                        && utc.refills == zone.refills && utc.applied == zone.applied
                        && utc.error == zone.error;
                    CHECK(same);
                    if (!same && ++mismatches <= 5) {
                        std::fprintf(stderr, "  MISMATCH %s %s %s %s: bars %zu/%zu error '%s' / '%s'\n",
                                     session.name, variant.name,
                                     labels == NativeSlotLabelPolicy::Canonical ? "canonical" : "tolerant",
                                     mode_name(mode), utc.bars.size(), zone.bars.size(),
                                     utc.error.c_str(), zone.error.c_str());
                    }
                    if (utc.error.empty() && !utc.bars.empty()) ++completed;
                }
            }
        }
    }
    std::printf("  %d runs (%d completed with bars); %d mismatches\n", runs, completed, mismatches);
    CHECK(completed * 4 >= runs * 3);
}

// A 60-minute bar whose label sits in a declared break reads the break's
// reopening: in session, neither opening nor closing its day. On UTC that
// label is out of session, so it takes the interval path inside the shortcut.
void test_break_label_reads_its_reopening() {
    std::printf("test_break_label_reads_its_reopening\n");
    const Session session{"0900-1130,1230-1500", "break"};
    const Pairing pairing{"30", "60", 30 * kMinute};
    const auto calendar = nc::parse_session(session.text, "UTC");
    CHECK(calendar.has_value());
    if (!calendar) return;
    const auto bars = tape_for(*calendar, pairing, Tape::Whole);
    const Outcome o = drive(spec_for(session, "UTC", pairing, NativeSlotLabelPolicy::Canonical),
                            bars, Mode::Batch, 0);
    CHECK(o.error.empty());
    int noon = 0;
    for (const Facts& f : o.bars) {
        if (f.ts % kDay != 12 * 60 * kMinute) continue;
        ++noon;
        CHECK(f.in);
        CHECK(!f.opens);
        CHECK(!f.closes);
    }
    // Friday through Monday each have one (the session has no weekday mask).
    CHECK(noon >= 4);
}

// ── 3. cost ────────────────────────────────────────────────────────────────

void test_utc_tolerant_batch_resolves_no_interval_per_bar() {
    std::printf("test_utc_tolerant_batch_resolves_no_interval_per_bar\n");
    const Session session{"24x7", "24x7"};
    const Pairing pairing{"15", "15", 15 * kMinute};
    const auto calendar = nc::parse_session(session.text, "UTC");
    CHECK(calendar.has_value());
    if (!calendar) return;
    const auto bars = tape_for(*calendar, pairing, Tape::Whole);
    const Outcome utc = drive(spec_for(session, "UTC", pairing, NativeSlotLabelPolicy::FeedTolerant),
                              bars, Mode::Batch, 0);
    const Outcome zone = drive(spec_for(session, "UTC0", pairing, NativeSlotLabelPolicy::FeedTolerant),
                               bars, Mode::Batch, 0);
    std::printf("  %zu bars: UTC resolved %llu intervals, UTC0 %llu\n", bars.size(),
                static_cast<unsigned long long>(utc.resolutions),
                static_cast<unsigned long long>(zone.resolutions));
    CHECK(utc.error.empty() && zone.error.empty());
    CHECK(utc.bars.size() == bars.size());
    CHECK(utc.bars == zone.bars);
    CHECK(utc.resolutions <= 1);
    CHECK(zone.resolutions >= bars.size());
}

}  // namespace

int main() {
    test_utc_decision();
    test_same_facts_as_the_zone_path();
    test_other_delivery_paths();
    test_break_label_reads_its_reopening();
    test_utc_tolerant_batch_resolves_no_interval_per_bar();
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
