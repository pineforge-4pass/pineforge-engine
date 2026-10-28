// The consumer's ring of certified cycles owns a fixed heap,
// and the cycles it keeps are the calendar's days.
//
// The L0b heap census (4,000 against 16,000 one-minute UTC bars) found a second
// growth site beside the memo's (test_native_calendar_memo_bound): the span
// vectors of NativeExecutionConsumer::certified_cycles_, +0.003 bytes a bar.
// The ring is four entries and was never unbounded; what grew was the heap its
// entries took one buffer at a time, each from the certificate it was made
// from, until all four were in use -- and a certificate of a closed day owns no
// buffer at all, so even a full ring held what the last four days happened to
// have. Lane L19n reserves every entry's span buffer at the first certificate,
// to the most spans a session day of the run's calendar has
// (native_calendar::session_day_span_bound), and copies each certificate into
// the entry it replaces, so the ring owns the same heap whatever days it holds.
//
// This row configures a bare native host (its calendar is built, nothing is
// run) and asks the consumer for the certified cycle of an instant of each of
// thousands of consecutive days, the ring wrapping at every fourth, then of
// instants well behind the front, whose entries the ring dropped long ago. The
// reference is the memo-free calendar of native_calendar.hpp: a certified cycle
// must be the session day that calendar answers for the instant, its origin,
// its end, its ordinal and its spans, bit for bit. After every ask the heap is
// read: the capacity of the ring's buffers, and of the calendar memo behind it,
// is what the first certificate left, and the memo holds no more days than its
// capacity. A UTC calendar certifies every cycle, and the sessions below give
// it days of no span (a weekday mask), two spans (a break) and four (four
// windows an hour long, whose bound is twelve); a POSIX rule zone certifies
// none, and a certificate refused leaves the ring as constant as one granted:
// it reserves all the same and asks the calendar no day.
//
// Fail-before: at the lane's base the memo has no kDayCapacity,
// retained_span_capacity or retained_days, and native_calendar_memo.hpp
// declares no session_day_span_bound, so this TU does not compile there (the
// lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_calendar_memo.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    // What certify_cycle answers for an instant, copied out of the ring entry.
    struct Cycle {
        bool certified = false;
        std::int64_t origin_ms = 0;
        std::int64_t next_origin_ms = 0;
        std::int64_t ordinal = 0;
        std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    };
    static Cycle certify(const BacktestEngine& host, std::int64_t ms) {
        Cycle out;
        const auto* entry = NativeExecutionConsumer::bound(host).certify_cycle(ms);
        if (entry == nullptr) return out;
        out.certified = true;
        out.origin_ms = entry->origin_ms;
        out.next_origin_ms = entry->next_origin_ms;
        out.ordinal = entry->ordinal;
        out.spans = entry->spans;
        return out;
    }
    // The ring: how many entries it keeps, how many of them hold a range, and
    // the capacity, in spans, of the buffers they own together.
    static std::size_t ring_entries(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).certified_cycles_.size();
    }
    static std::size_t ring_ranges(const BacktestEngine& host) {
        std::size_t held = 0;
        for (const auto& cycle : NativeExecutionConsumer::bound(host).certified_cycles_) {
            if (cycle.origin_ms < cycle.next_origin_ms) ++held;
        }
        return held;
    }
    static std::size_t ring_span_capacity(const BacktestEngine& host) {
        std::size_t capacity = 0;
        for (const auto& cycle : NativeExecutionConsumer::bound(host).certified_cycles_) {
            capacity += cycle.spans.capacity();
        }
        return capacity;
    }
    // The calendar memo behind it.
    static std::size_t memo_days(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_memo_.retained_days();
    }
    static std::size_t memo_span_capacity(const BacktestEngine& host) {
        return NativeExecutionConsumer::bound(host).calendar_memo_.retained_span_capacity();
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

using namespace pineforge;
namespace nc = pineforge::native_calendar;
using Probe = NativeExecutionConsumerProbe;

namespace {

int failures = 0;
long long compared = 0;
std::string scenario = "initialization";

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "CHECK FAIL [%s] %s:%d  %s\n", scenario.c_str(), \
                         __FILE__, __LINE__, #cond);                             \
        }                                                                        \
    } while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kDay = 1440 * kMinute;
constexpr std::int64_t kFrom = 1420416000000LL;  // 2015-01-05 00:00 UTC, a Monday

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::int64_t in(std::int64_t lo, std::int64_t hi) {  // [lo, hi)
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo);
        return lo + static_cast<std::int64_t>(next() % span);
    }
};

class Idle final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

NativeRunSpec spec_for(const char* zone, const char* session) {
    NativeRunSpec spec;
    spec.identity = {"l19n-certified-ring", 1};
    spec.input_tf = "60";
    spec.script_tf = "60";
    spec.tickerid = "TEST:L19N";
    spec.timezone = zone;
    spec.session = session;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// What the ring must say of `ms` when the zone certifies every cycle: the
// session day the memo-free calendar answers for it.
void expect_the_calendar_day(const nc::SessionCalendar& calendar, const Probe::Cycle& cycle,
                             std::int64_t ms) {
    ++compared;
    CHECK(cycle.certified);
    CHECK(cycle.origin_ms <= ms && ms < cycle.next_origin_ms);
    const auto day = nc::session_day_at(calendar, ms);
    CHECK(day.has_value());
    if (!day) return;
    CHECK(cycle.origin_ms == day->origin_ms);
    CHECK(cycle.next_origin_ms == day->next_origin_ms);
    CHECK(cycle.ordinal == day->ordinal);
    CHECK(cycle.spans == day->spans);
    CHECK(cycle.spans.size() <= nc::session_day_span_bound(calendar));
}

// One configured host and `days` consecutive days of instants, a random one in
// each: certify_cycle is asked for it, the ring and the memo behind it are
// read, and the cycle is compared with the calendar's day when the zone
// certifies. Then the instants behind the front, asked of a ring that dropped
// their entries long ago.
void sweep(const char* zone, const char* session, bool certifies, int days,
           std::uint64_t seed) {
    scenario = std::string(zone) + " '" + session + "' "
        + (certifies ? "certified" : "refused");
    Idle host;
    CHECK(host.configure_native(spec_for(zone, session)).status == NativeSetupStatus::Applied);
    const auto calendar = nc::parse_session(session, zone);
    CHECK(calendar.has_value());
    if (!calendar) return;
    const std::size_t bound = nc::session_day_span_bound(*calendar);
    CHECK(bound > 0);
    Rng rng{seed};

    std::size_t ring_reserved = 0;
    std::size_t memo_reserved = 0;
    for (int d = 0; d < days; ++d) {
        const std::int64_t ms = kFrom + static_cast<std::int64_t>(d) * kDay + rng.in(0, kDay);
        const Probe::Cycle cycle = Probe::certify(host, ms);
        if (certifies) {
            expect_the_calendar_day(*calendar, cycle, ms);
        } else {
            CHECK(!cycle.certified);
            // A refused cycle asked the calendar for no day.
            CHECK(Probe::memo_days(host) == 0);
        }
        if (d == 0) {
            // What the first certificate reserved: every entry, to the bound.
            ring_reserved = Probe::ring_span_capacity(host);
            memo_reserved = Probe::memo_span_capacity(host);
            CHECK(ring_reserved >= Probe::ring_entries(host) * bound);
            CHECK(memo_reserved >= nc::SessionDayMemo::kDayCapacity * bound);
        }
        CHECK(Probe::ring_span_capacity(host) == ring_reserved);
        CHECK(Probe::memo_span_capacity(host) == memo_reserved);
        CHECK(Probe::memo_days(host) <= nc::SessionDayMemo::kDayCapacity);
    }

    // The cap was reached and used: the ring turned over many times and every
    // entry holds a range; a certifying walk filled the memo's table.
    CHECK(days > static_cast<int>(Probe::ring_entries(host)));
    CHECK(Probe::ring_ranges(host) == Probe::ring_entries(host));
    if (certifies) CHECK(Probe::memo_days(host) == nc::SessionDayMemo::kDayCapacity);

    // Behind the front: the entries of these days were replaced long ago, and
    // the ring makes each again, to the same capacity.
    const std::int64_t front = kFrom + static_cast<std::int64_t>(days - 1) * kDay;
    for (const int back : {0, 1, 2, 3, 4, 5, 50, 500, 2000}) {
        if (back >= days) continue;
        const std::int64_t ms = front - static_cast<std::int64_t>(back) * kDay
            + rng.in(0, kDay);
        const Probe::Cycle cycle = Probe::certify(host, ms);
        if (certifies) {
            expect_the_calendar_day(*calendar, cycle, ms);
        } else {
            CHECK(!cycle.certified);
        }
        CHECK(Probe::ring_span_capacity(host) == ring_reserved);
        CHECK(Probe::memo_span_capacity(host) == memo_reserved);
    }
}

void the_ring_keeps_a_fixed_heap_and_the_calendars_days() {
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    // A day with no declared session, a break (two spans on a weekday, none on
    // a closed day), an overnight session whose origin is in the evening, and
    // four windows an hour long.
    for (const char* session : {"24x7", "0900-1130,1230-1500:23456", "1800-1700:23456",
                                "0000-0100,0200-0300,0400-0500,0600-0700"}) {
        sweep("UTC", session, true, 2500, seed += 0x1234567);
    }
    // A POSIX rule zone is never certified, and the ring is as constant.
    sweep("EST5EDT,M3.2.0,M11.1.0", "0930-1600:23456", false, 2500, seed += 0x1234567);
}

}  // namespace

int main() {
    the_ring_keeps_a_fixed_heap_and_the_calendars_days();
    std::printf("test_native_certified_cycle_bound: %lld certified cycles against the "
                "calendar, %d failures\n", compared, failures);
    return failures == 0 ? 0 : 1;
}
