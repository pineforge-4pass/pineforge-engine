// R5 lane PERF-ZONED: session-day facts on an exchange zone without
// resolving a calendar interval per bar.
//
// Lane PERF-KEDGE recovered wave G KERNEL-EDGE's per-bar cost on UTC
// calendars only. On every other zone present_session_day still resolved each
// bar's script interval from the calendar, and read the bar's label and its
// neighbours through it, at every bar: on the INT25 harness a New York
// 5-minute FeedTolerant run cost 1.73x the instructions of 1a0e7ea1, a Tokyo
// 1-minute run 1.22x. Lane PERF-ZONED certifies a session day's cycle instead
// (native_calendar::cycle_certificate): a UTC or fixed-offset zone, or, under
// glibc or macOS, a TZif zone whose transitions within 22 days of the cycle
// are small, more than a week apart and never a backward move that keeps the
// daylight-saving flag, each read back from libc. On a certified cycle every lookup of every
// instant is that day and the calendar's answers do not depend on what it was
// asked before, so an instant in one of the day's spans reads in session on
// its ordinal, and one whose bucket meets no span reads out of session, with
// nothing resolved. Every other instant, and every instant of a cycle that is
// not certified, is read as at the lane's base.
//
// This witness checks:
//   1. Certificates at real transitions. New York's, Chicago's, London's,
//      Sydney's and Lord Howe's (a 30-minute change) 2024 changes are
//      certified day by day where the host's TZif table lists them (Ubuntu
//      24.04's and macOS's do), as are Tokyo, Kolkata, a fixed offset, Apia's
//      2012 change to +14 (the offset bound), Sitka under its Russian-era
//      +14:58:47 and Moscow after 2014. No instant within reach of Tokyo's
//      1887 change (a backward move that keeps the flag), Sitka's 1867 jump
//      back a day, Apia's 2011 jump forward a day, Kwajalein's 1969 jump
//      back, Moscow's 2014 move back, Recife's week-long daylight saving of
//      2000, New York past its table (2040), or a POSIX zone is certified,
//      and a refused certificate reads nothing. Throughout every certified
//      cycle the memo-free session_day_at answers the certified day.
//   2. The same tapes under a TZif zone (certified cycles) and under its
//      POSIX twin (the same clock, never certified, so every interval is
//      resolved as at the lane's base) present identical facts at every
//      decision point a host reads them at -- bar open, bar, fill
//      recalculation, applied execution and realtime print -- across New
//      York's March and November, London's October and Sydney's October 2024
//      changes, Havana's at midnight both ways, Kolkata and a fixed offset:
//      whole-day, plain, lunch-break, overnight and weekday-masked sessions
//      and one whose edges fall in New York's fold and gap, minute buckets
//      over finer inputs, a day's and a week's, Canonical and FeedTolerant
//      labels with extended hours, stamps at a span's close, gaps, off-grid
//      stamps and stamps in closed time, batch runs, and confirmed-bar and
//      printed streams; and on the magnifier's path, seconds timeframes and
//      an undetected one. Both runs must also end with the same error text.
//   3. Cost: New York and Tokyo FeedTolerant batches round the clock resolve
//      at most one interval for their facts and read libc's local time a few
//      times a session day, where their POSIX twins resolve an interval and
//      read the local time at least once a bar.
//
// Fail-before: at the lane's base native_calendar::cycle_certificate and
// SessionDayMemo::libc_local_reads do not exist, so this TU does not compile
// there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_calendar_memo.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    static std::uint64_t interval_resolutions(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_memo_.interval_resolutions();
    }
    static std::uint64_t libc_local_reads(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_memo_.libc_local_reads();
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
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// 00:00 UT of a civil date.
constexpr std::int64_t ut(int y, unsigned m, unsigned d) { return days_from_civil(y, m, d) * kDay; }

// Whether the host's TZif file for `zone` lists a transition at or after
// `seconds` in its 64-bit table. A "fat" file -- Ubuntu 24.04 and macOS ship
// them -- lists New York's changes through 2037; a "slim" one stops where its
// POSIX footer takes over (2007 for New York), and a cycle past the table is
// never certified. The file is looked for where the calendar looks.
bool tzif_lists_transition_from(const char* zone, std::int64_t seconds) {
#if defined(__APPLE__)
    const char* const roots[] = {"/var/db/timezone/zoneinfo", "/usr/share/zoneinfo"};
#else
    const char* env = std::getenv("TZDIR");
    const char* const roots[] = {env && *env ? env : "/usr/share/zoneinfo"};
#endif
    std::ifstream in;
    for (const char* root : roots) {
        in.open(std::string(root) + "/" + zone, std::ios::binary);
        if (in) break;
        in.clear();
    }
    const std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u32 = [&](std::size_t at) -> std::size_t {
        const auto* p = reinterpret_cast<const unsigned char*>(b.data()) + at;
        return (std::size_t{p[0]} << 24) | (std::size_t{p[1]} << 16) | (std::size_t{p[2]} << 8) | p[3];
    };
    if (b.size() < 44 || b.compare(0, 4, "TZif") != 0 || b[4] < '2') return false;
    // Skip the version 1 block: times and indices, types, abbreviations, leap
    // records, standard and UT indicators.
    const std::size_t v2 = 44 + u32(32) * 5 + u32(36) * 6 + u32(40) + u32(28) * 8 + u32(24) + u32(20);
    if (b.size() < v2 + 44) return false;
    const std::size_t count = u32(v2 + 32);
    if (b.size() < v2 + 44 + count * 8) return false;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t t = 0;
        for (std::size_t k = 0; k < 8; ++k) t = (t << 8) | static_cast<unsigned char>(b[v2 + 44 + i * 8 + k]);
        if (static_cast<std::int64_t>(t) >= seconds) return true;
    }
    return false;
}

// ── 1. certificates at real transitions ────────────────────────────────────

struct Witness {
    const char* zone;
    const char* session;
    std::int64_t from;
    int days;
    bool certified;
    // Certified only where the host's table lists the zone's 2024 changes.
    bool needs_table;
    const char* what;
};

void test_certificates_at_real_transitions() {
    std::printf("test_certificates_at_real_transitions\n");
    const std::int64_t k2025 = days_from_civil(2025, 1, 1) * 86400;
    const Witness witnesses[] = {
        {"America/New_York", "0930-1600", ut(2024, 3, 6), 8, true, true, "spring forward, 2024-03-10"},
        {"America/New_York", "0100-0400", ut(2024, 3, 8), 4, true, true, "a window across 02:00, spring"},
        {"America/New_York", "24x7", ut(2024, 10, 31), 6, true, true, "fall back, 2024-11-03"},
        {"America/New_York", "0100-0400", ut(2024, 11, 1), 4, true, true, "a window across 02:00, fall"},
        {"America/New_York", "1700-1600", ut(2024, 3, 7), 6, true, true, "overnight, spring"},
        {"America/Chicago", "1700-1600", ut(2024, 11, 1), 4, true, true, "fall back"},
        {"Europe/London", "0800-1630", ut(2024, 3, 29), 4, true, true, "spring forward, 01:00 UT"},
        {"Europe/London", "24x7", ut(2024, 10, 25), 4, true, true, "fall back, 01:00 UT"},
        {"Australia/Sydney", "1000-1600", ut(2024, 4, 5), 4, true, true, "southern fall back"},
        {"Australia/Sydney", "24x7", ut(2024, 10, 4), 4, true, true, "southern spring forward"},
        {"Australia/Lord_Howe", "24x7", ut(2024, 4, 5), 3, true, true, "a 30-minute fall back"},
        {"Asia/Tokyo", "0900-1130,1230-1500", ut(2024, 5, 1), 3, true, false, "JST, the footer's one offset"},
        {"Asia/Tokyo", "24x7", ut(1888, 3, 1), 2, true, false, "past its 1887 change's reach"},
        {"Asia/Kolkata", "0915-1530", ut(2024, 5, 1), 2, true, false, "IST"},
        {"America/Sitka", "24x7", ut(1867, 6, 1), 2, true, false, "+14:58:47, no change in reach"},
        {"Europe/Moscow", "24x7", ut(2015, 6, 1), 2, true, false, "past its 2014 change's reach"},
        {"Pacific/Apia", "24x7", ut(2012, 9, 28), 4, true, false, "+13 to +14, the offset bound"},
        {"UTC+3", "24x7", ut(2024, 5, 1), 2, true, false, "a fixed offset"},
        {"UTC", "0930-1600", ut(2024, 5, 1), 2, true, false, "UTC"},
        {"Asia/Tokyo", "24x7", ut(1887, 12, 20), 3, false, false, "LMT +9:18:59 to JST: back, flag kept"},
        {"America/Sitka", "24x7", ut(1867, 10, 17), 4, false, false, "Russian to American: a day back"},
        {"Pacific/Apia", "24x7", ut(2011, 12, 28), 5, false, false, "-10 to +14: a day forward"},
        {"Pacific/Kwajalein", "24x7", ut(1969, 9, 28), 4, false, false, "+11 to -12: a day back"},
        {"Europe/Moscow", "24x7", ut(2014, 10, 20), 12, false, false, "+4 to +3: back, flag kept"},
        {"America/Recife", "24x7", ut(2000, 10, 7), 10, false, false, "daylight saving for a week"},
        {"America/New_York", "24x7", ut(2040, 6, 1), 2, false, false, "past the table"},
        {"EST5EDT,M3.2.0,M11.1.0", "24x7", ut(2024, 3, 8), 4, false, false, "a POSIX rule"},
        {"JST-9", "24x7", ut(2024, 5, 1), 2, false, false, "a POSIX offset"},
    };
    int certified_cycles = 0;
    int probes = 0;
    for (const Witness& w : witnesses) {
        const auto calendar = nc::parse_session(w.session, w.zone);
        CHECK(calendar.has_value());
        if (!calendar) continue;
        const bool listed = !w.needs_table || tzif_lists_transition_from(w.zone, k2025);
        if (!listed) {
            std::printf("  %s: this host's TZif table stops before 2025 (a slim file); its"
                        " cycles take the calendar path\n", w.zone);
        }
        nc::SessionDayMemo memo;
        int asked = 0;
        int certified = 0;
        int cycles = 0;
        int mismatches = 0;
        std::int64_t last_origin = std::numeric_limits<std::int64_t>::min();
        for (std::int64_t t = w.from; t < w.from + w.days * kDay; t += 30 * kMinute) {
            ++asked;
            const auto c = nc::cycle_certificate(*calendar, t, memo);
            if (!c) continue;
            ++certified;
            CHECK(c->origin_ms <= t && t < c->next_origin_ms);
            if (c->origin_ms == last_origin) continue;
            last_origin = c->origin_ms;
            ++cycles;
            // The memo-free lookup, throughout the cycle and at every edge of
            // its spans, answers the certified day.
            std::vector<std::int64_t> at;
            for (std::int64_t x = c->origin_ms; x < c->next_origin_ms; x += 30 * kMinute) at.push_back(x);
            at.push_back(c->next_origin_ms - 1);
            for (const auto& span : c->spans) {
                for (const std::int64_t x : {span.first - 1, span.first, span.second - 1, span.second}) {
                    if (c->origin_ms <= x && x < c->next_origin_ms) at.push_back(x);
                }
            }
            for (const std::int64_t x : at) {
                const auto day = nc::session_day_at(*calendar, x);
                ++probes;
                const bool same = day && day->origin_ms == c->origin_ms
                    && day->next_origin_ms == c->next_origin_ms && day->ordinal == c->ordinal
                    && day->spans == c->spans;
                if (!same) ++mismatches;
            }
        }
        std::printf("  %-24s %-20s %-40s %d/%d instants certified, %d cycles\n", w.zone,
                    w.session, w.what, certified, asked, cycles);
        CHECK(w.certified && listed ? certified == asked && cycles >= w.days - 1 : certified == 0);
        CHECK(mismatches == 0);
        certified_cycles += cycles;
    }
    std::printf("  %d certified cycles, %d memo-free lookups in them\n", certified_cycles, probes);
    // A certificate refused asks the calendar nothing: under a POSIX zone the
    // memo reads libc's local time for no instant it was asked about.
    for (const char* zone : {"EST5EDT,M3.2.0,M11.1.0", "JST-9"}) {
        const auto calendar = nc::parse_session("0930-1600", zone);
        CHECK(calendar.has_value());
        if (!calendar) continue;
        nc::SessionDayMemo memo;
        int granted = 0;
        for (std::int64_t t = ut(2024, 3, 8); t < ut(2024, 3, 12); t += 30 * kMinute)
            granted += nc::cycle_certificate(*calendar, t, memo).has_value();
        CHECK(granted == 0);
        CHECK(memo.libc_local_reads() == 0);
    }
}

// ── 2. the same facts, a TZif zone against its POSIX twin ──────────────────

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
            (void)submit(no::Request{no::Transact{units}, "zoned-" + std::to_string(calculations), ""});
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

// A TZif zone, the POSIX rule that keeps its clock through the tape, and the
// tape's first day (00:00 UT): four and a half days across the change. Havana
// changes at midnight, so a whole-day session's origin falls in its gap.
struct Twin {
    const char* zone;
    const char* posix;
    const char* name;
    std::int64_t from;
};

// Tokyo's twin drives the cost pin, which compares its facts too.
const Twin kTokyo = {"Asia/Tokyo", "JST-9", "tokyo", ut(2024, 5, 9)};

const Twin kTwins[] = {
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0", "new-york-spring", ut(2024, 3, 7)},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0", "new-york-fall", ut(2024, 10, 31)},
    {"America/Havana", "CST5CDT,M3.2.0/0,M11.1.0/1", "havana-spring", ut(2024, 3, 7)},
    {"America/Havana", "CST5CDT,M3.2.0/0,M11.1.0/1", "havana-fall", ut(2024, 10, 31)},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0", "london-fall", ut(2024, 10, 24)},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3", "sydney-spring", ut(2024, 10, 3)},
    {"Asia/Kolkata", "IST-5:30", "kolkata", ut(2024, 5, 9)},
    {"UTC+3", "<+03>-3", "utc+3", ut(2024, 5, 9)},
};

struct Session {
    const char* text;
    const char* name;
};

// Every shape the facts read differently: a whole day, a plain window, a
// declared break, a session crossing midnight, a weekday mask without Monday
// (a week's bar then opens on a day out of session), and small-hours windows
// whose edges fall in New York's fall fold (01:30) and spring gap (02:30).
const Session kSessions[] = {
    {"24x7", "24x7"},
    {"0930-1600", "plain"},
    {"0900-1130,1230-1500", "break"},
    {"1700-1600", "overnight"},
    {"0930-1600:3456", "tue-fri"},
    {"0130-0200,0230-0400", "small-hours"},
};

struct Pairing {
    const char* input;
    const char* script;
    std::int64_t step;  // the input step
};

enum class Tape { Whole, Extended, Edges, Gaps, OffGrid, InBreak };
const char* tape_name(Tape tape) {
    switch (tape) {
    case Tape::Whole: return "whole";
    case Tape::Extended: return "extended";
    case Tape::Edges: return "edges";
    case Tape::Gaps: return "gaps";
    case Tape::OffGrid: return "off-grid";
    case Tape::InBreak: return "in-break";
    }
    return "?";
}

enum Mode { kBatch = 1, kStream = 2, kPrints = 4 };
const char* mode_name(int mode) {
    switch (mode) {
    case kBatch: return "batch";
    case kStream: return "stream";
    case kPrints: return "prints";
    }
    return "?";
}

// The runs each twin and session is driven through: the label policies, the
// ways a tape departs from the calendar's grid and the delivery modes, spread
// over minute buckets matching their inputs and coarser than them, a day's
// and a week's.
struct Case {
    Pairing pairing;
    NativeSlotLabelPolicy labels;
    Tape tape;
    int modes;
};

const Case kCases[] = {
    {{"5", "5", 5 * kMinute}, NativeSlotLabelPolicy::Canonical, Tape::Whole, kBatch | kStream},
    {{"15", "15", 15 * kMinute}, NativeSlotLabelPolicy::FeedTolerant, Tape::Extended,
     kBatch | kStream | kPrints},
    {{"5", "5", 5 * kMinute}, NativeSlotLabelPolicy::FeedTolerant, Tape::Gaps, kBatch},
    {{"30", "60", 30 * kMinute}, NativeSlotLabelPolicy::Canonical, Tape::Whole, kBatch},
    {{"30", "60", 30 * kMinute}, NativeSlotLabelPolicy::FeedTolerant, Tape::InBreak, kBatch | kStream},
    {{"5", "15", 5 * kMinute}, NativeSlotLabelPolicy::FeedTolerant, Tape::OffGrid, kBatch},
    {{"60", "60", 60 * kMinute}, NativeSlotLabelPolicy::FeedTolerant, Tape::Edges, kBatch | kStream},
    {{"60", "1D", 60 * kMinute}, NativeSlotLabelPolicy::Canonical, Tape::Whole, kBatch},
    {{"60", "1W", 60 * kMinute}, NativeSlotLabelPolicy::Canonical, Tape::Whole, kBatch},
};

Bar bar_at(std::int64_t ts, int i) {
    const double price = 100.0 + 0.25 * static_cast<double>(i % 11);
    return {price, price + 0.75, price - 0.75, price + 0.25, 1.0, ts};
}

// Input bars at `clock`'s own input labels wherever the slot has an instant in
// session, over four and a half days from the twin's first day: the run's
// calendar, or for Extended the zone's whole day (a feed with extended hours).
// Edges adds a stamp at the instant each span closes, where a provider's
// label sits out of session inside a bucket that was in session; Gaps drops
// every seventh bar and the whole of the fourth day; OffGrid moves every third
// stamp a little inside its own slot, and InBreak adds a stamp inside each
// stretch of closed time.
std::vector<Bar> tape_for(const nc::SessionCalendar& clock, const Pairing& pairing,
                          std::int64_t from, Tape tape) {
    const auto tf = nc::parse_timeframe(pairing.input);
    std::vector<Bar> bars;
    if (!tf) return bars;
    nc::SessionDayMemo memo;
    const std::int64_t until = from + 4 * kDay + 12 * kHour;
    const std::int64_t nudge = pairing.step >= 5 * kMinute ? 2 * kMinute : 20 * kSecond;
    int i = 0;
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    for (std::int64_t t = from; t < until;) {
        const auto interval = nc::interval_containing(clock, *tf, t, memo);
        if (!interval || interval->next_period_open_ms <= t) break;
        t = interval->next_period_open_ms;
        if (interval->last_traded_close_ms <= interval->eligible_open_ms) continue;
        const std::int64_t label = interval->open_ms;
        if (tape == Tape::InBreak && previous != std::numeric_limits<std::int64_t>::min()
            && label - previous > pairing.step + nudge) {
            bars.push_back(bar_at(previous + pairing.step + nudge, i++));
        }
        previous = label;
        if (tape == Tape::Gaps && (i % 7 == 3 || (label >= from + 3 * kDay && label < from + 4 * kDay))) {
            ++i;
            continue;
        }
        bars.push_back(bar_at(tape == Tape::OffGrid && i % 3 == 1 ? label + nudge : label, i));
        ++i;
    }
    if (tape == Tape::Edges) {
        std::vector<std::int64_t> closes;
        for (std::int64_t t = from; t < until; t += kHour) {
            const auto day = nc::session_day_at(clock, t, memo);
            if (!day) continue;
            for (const auto& span : day->spans) {
                if (span.second < until && (closes.empty() || closes.back() < span.second))
                    closes.push_back(span.second);
            }
        }
        std::vector<Bar> merged;
        std::size_t k = 0;
        for (const Bar& bar : bars) {
            for (; k < closes.size() && closes[k] <= bar.timestamp; ++k) {
                if (closes[k] < bar.timestamp) merged.push_back(bar_at(closes[k], i++));
            }
            merged.push_back(bar);
        }
        bars = std::move(merged);
    }
    return bars;
}

NativeRunSpec spec_for(const Session& session, const char* zone, const Pairing& pairing,
                       NativeSlotLabelPolicy labels) {
    NativeRunSpec spec;
    spec.identity = {"perf-zoned", 1};
    spec.input_tf = pairing.input;
    spec.script_tf = pairing.script;
    spec.timeframe_undetected = spec.input_tf.empty();
    spec.tickerid = "TEST:ZONED";
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
    std::uint64_t local_reads = 0;
};

Outcome drive(const NativeRunSpec& spec, const std::vector<Bar>& bars, int mode, int warmup) {
    Host host;
    Outcome out;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        out.error = "configure: " + host.last_error();
        return out;
    }
    const int n = static_cast<int>(bars.size());
    switch (mode) {
    case kBatch:
        host.run(bars.data(), n);
        break;
    case kStream:
        if (host.stream_begin(bars.data(), warmup, spec.input_tf, spec.script_tf)) {
            for (int i = warmup; i < n; ++i) {
                if (!host.stream_push_bar(bars[static_cast<std::size_t>(i)])) break;
            }
            (void)host.stream_end(true);
        }
        break;
    case kPrints:
        if (host.stream_begin(bars.data(), warmup, spec.input_tf, spec.script_tf)) {
            // Thirty hours of prints: at least one session-day boundary on
            // every session, at a bounded cost.
            std::uint64_t sequence = 1;
            bool ok = true;
            const std::int64_t until = warmup < n
                ? bars[static_cast<std::size_t>(warmup)].timestamp + 30 * kHour : 0;
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
    out.local_reads = Probe::libc_local_reads(host);
    return out;
}

bool same_facts(const Outcome& a, const Outcome& b) {
    return a.bar_opens == b.bar_opens && a.bars == b.bars && a.refills == b.refills
        && a.applied == b.applied && a.ticks == b.ticks && a.error == b.error;
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

void test_same_facts_as_the_posix_twin() {
    std::printf("test_same_facts_as_the_posix_twin\n");
    Tally tally;
    for (const Twin& twin : kTwins) {
        const auto day_clock = nc::parse_session("24x7", twin.zone);
        CHECK(day_clock.has_value());
        if (!day_clock) continue;
        for (const Session& session : kSessions) {
            const auto calendar = nc::parse_session(session.text, twin.zone);
            CHECK(calendar.has_value());
            if (!calendar) continue;
            for (const Case& c : kCases) {
                const auto bars = tape_for(c.tape == Tape::Extended ? *day_clock : *calendar,
                                           c.pairing, twin.from, c.tape);
                const int warmup = static_cast<int>(bars.size()) / 2;
                for (const int mode : {kBatch, kStream, kPrints}) {
                    if (!(c.modes & mode)) continue;
                    const Outcome zone = drive(spec_for(session, twin.zone, c.pairing, c.labels),
                                               bars, mode, warmup);
                    const Outcome posix = drive(spec_for(session, twin.posix, c.pairing, c.labels),
                                                bars, mode, warmup);
                    ++tally.runs;
                    const bool same = same_facts(zone, posix);
                    CHECK(same);
                    if (!same && ++tally.mismatches <= 5) {
                        std::fprintf(stderr, "  MISMATCH %s %s %s->%s %s %s %s: bars %zu/%zu"
                                     " error '%s' / '%s'\n", twin.name, session.name,
                                     c.pairing.input, c.pairing.script,
                                     c.labels == NativeSlotLabelPolicy::Canonical ? "canonical"
                                                                                  : "tolerant",
                                     tape_name(c.tape), mode_name(mode), zone.bars.size(),
                                     posix.bars.size(), zone.error.c_str(), posix.error.c_str());
                    }
                    if (zone.error.empty() && !zone.bars.empty()) ++tally.completed;
                    count(tally, zone);
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

// The other paths a script bar is delivered on, a TZif zone against its
// POSIX twin across New York's March change: the magnifier's synthesized path
// (deliver_intrabar_script), a seconds timeframe, seconds inputs under a
// minute bucket, and an undetected timeframe, which takes one bar and
// partitions by its raw label -- batch and stream (the undetected one batch
// only), a whole day and a lunch break, both label policies.
void test_other_delivery_paths() {
    std::printf("test_other_delivery_paths\n");
    struct Variant {
        const char* name;
        Pairing pairing;
        bool magnifier;
        std::int64_t span;
    };
    const Variant variants[] = {
        {"magnifier", {"5", "15", 5 * kMinute}, true, 4 * kDay + 12 * kHour},
        {"30-second", {"30S", "30S", 30 * kSecond}, false, kDay + 2 * kHour},
        {"15-second-to-minute", {"15S", "1", 15 * kSecond}, false, kDay + 2 * kHour},
        {"undetected", {"", "", 15 * kMinute}, false, 12 * kHour},
    };
    const Twin& twin = kTwins[0];
    int runs = 0, completed = 0, mismatches = 0;
    for (const Session& session : {kSessions[0], kSessions[2]}) {
        const auto calendar = nc::parse_session(session.text, twin.zone);
        CHECK(calendar.has_value());
        if (!calendar) continue;
        for (const Variant& variant : variants) {
            // The undetected run's one bar is the tape's first; the seconds
            // tapes start a day before the change and run past it.
            const std::int64_t from = variant.span < 2 * kDay ? twin.from + 2 * kDay + 12 * kHour
                                                              : twin.from;
            auto bars = tape_for(*calendar, variant.pairing.input[0] ? variant.pairing
                                                                     : Pairing{"15", "15", 15 * kMinute},
                                 from, Tape::Whole);
            while (!bars.empty() && bars.back().timestamp >= from + variant.span) bars.pop_back();
            if (variant.pairing.input[0] == '\0' && bars.size() > 1) bars.resize(1);
            for (const auto labels : {NativeSlotLabelPolicy::Canonical,
                                      NativeSlotLabelPolicy::FeedTolerant}) {
                for (const int mode : {kBatch, kStream}) {
                    if (variant.pairing.input[0] == '\0' && mode != kBatch) continue;
                    auto zone_spec = spec_for(session, twin.zone, variant.pairing, labels);
                    auto posix_spec = spec_for(session, twin.posix, variant.pairing, labels);
                    if (variant.magnifier) {
                        zone_spec.intrabar.value = IntrabarPath::synthesized{};
                        posix_spec.intrabar.value = IntrabarPath::synthesized{};
                    }
                    const int warmup = static_cast<int>(bars.size()) / 2;
                    const Outcome zone = drive(zone_spec, bars, mode, warmup);
                    const Outcome posix = drive(posix_spec, bars, mode, warmup);
                    ++runs;
                    const bool same = same_facts(zone, posix);
                    CHECK(same);
                    if (!same && ++mismatches <= 5) {
                        std::fprintf(stderr, "  MISMATCH %s %s %s %s: bars %zu/%zu error '%s' / '%s'\n",
                                     session.name, variant.name,
                                     labels == NativeSlotLabelPolicy::Canonical ? "canonical" : "tolerant",
                                     mode_name(mode), zone.bars.size(), posix.bars.size(),
                                     zone.error.c_str(), posix.error.c_str());
                    }
                    if (zone.error.empty() && !zone.bars.empty()) ++completed;
                }
            }
        }
    }
    std::printf("  %d runs (%d completed with bars); %d mismatches\n", runs, completed, mismatches);
    CHECK(completed * 4 >= runs * 3);
}

// ── 3. cost ────────────────────────────────────────────────────────────────

// A FeedTolerant batch round the clock, so most labels are out of session:
// the Pine adapter's shape on an exchange chart fed extended hours.
Outcome tolerant_batch(const Twin& twin, const char* zone, const Session& session,
                       const Pairing& pairing, std::size_t& bars_out) {
    const auto clock = nc::parse_session("24x7", twin.zone);
    if (!clock) return {};
    const auto bars = tape_for(*clock, pairing, twin.from, Tape::Whole);
    bars_out = bars.size();
    return drive(spec_for(session, zone, pairing, NativeSlotLabelPolicy::FeedTolerant), bars, kBatch, 0);
}

void test_zoned_tolerant_batch_resolves_no_interval_per_bar() {
    std::printf("test_zoned_tolerant_batch_resolves_no_interval_per_bar\n");
    const std::int64_t k2025 = days_from_civil(2025, 1, 1) * 86400;
    struct Pin {
        const Twin* twin;
        Session session;
        Pairing pairing;
        // Certified wherever the host's table lists the zone's 2024 changes;
        // Tokyo's footer is its one offset, so it is certified on any host.
        bool needs_table;
    };
    const Pin pins[] = {
        {&kTwins[0], {"0930-1600", "plain"}, {"5", "5", 5 * kMinute}, true},
        {&kTokyo, {"0900-1130,1230-1500", "break"}, {"1", "1", kMinute}, false},
    };
    for (const Pin& pin : pins) {
        std::size_t n = 0;
        const Outcome zone = tolerant_batch(*pin.twin, pin.twin->zone, pin.session, pin.pairing, n);
        const Outcome posix = tolerant_batch(*pin.twin, pin.twin->posix, pin.session, pin.pairing, n);
        std::printf("  %zu bars: %s resolved %llu intervals and read the local time %llu times,"
                    " %s %llu and %llu\n", n, pin.twin->zone,
                    static_cast<unsigned long long>(zone.resolutions),
                    static_cast<unsigned long long>(zone.local_reads), pin.twin->posix,
                    static_cast<unsigned long long>(posix.resolutions),
                    static_cast<unsigned long long>(posix.local_reads));
        CHECK(zone.error.empty() && posix.error.empty());
        CHECK(zone.bars.size() == n);
        CHECK(same_facts(zone, posix));
        if (!pin.needs_table || tzif_lists_transition_from(pin.twin->zone, k2025)) {
            CHECK(zone.resolutions <= 1);
            CHECK(zone.local_reads <= 64);
        }
        CHECK(posix.resolutions >= n);
        CHECK(posix.local_reads >= n);
    }
}

}  // namespace

int main() {
    test_certificates_at_real_transitions();
    test_same_facts_as_the_posix_twin();
    test_other_delivery_paths();
    test_zoned_tolerant_batch_resolves_no_interval_per_bar();
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
