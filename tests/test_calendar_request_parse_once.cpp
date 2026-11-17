// A request's session calendar is parsed once, not once per input bar, and
// each request is answered with its own calendar.
//
// Target (one): how often the session calendar of a higher-timeframe request
// is parsed while its aggregator is fed, and whose calendar each aggregator
// is then answered with.
//
// The evaluator feed (the five-argument TimeframeAggregator::feed) resolves a
// requested period from the session calendar when the period is a calendar
// multiple: here three months, from one-hour bars. Parsing that calendar
// checks the timezone against the zone database, which opens the zone file.
// A feed that parses for every input bar opens it once per bar. A feed that
// parses a calendar once opens it once, however many bars follow.
//
// What is counted is the engine's own open() of the zone file, seen by
// interposing open in this executable. That needs ELF symbol interposition:
// the count rows run on Linux and say so where they cannot run. The unit of
// the count is measured here and not assumed: `one_parse` is what a single
// parse_session of the same text opens. No time is measured.
//
// Rows, each after an evaluator registration, which is where the engine lets
// go of what an earlier run's calendar lookups kept:
//   count      one aggregator fed N bars opens the zone file one_parse times;
//              one fed 2N bars opens it one_parse times as well; a second
//              aggregator of the same calendar right after opens it at most
//              one_parse times more.
//   calendars  three aggregators fed in turn, bar by bar, over one tape: UTC
//              all day, New York all day, and New York with the day running
//              from 17:00 to 17:00. The first two differ in timezone only,
//              the last two in session only. Each must hand over the
//              three-month buckets of its OWN calendar, and the two New York
//              calendars together open the zone file twice one_parse times.
//              A parse kept under less than the session and the timezone
//              would hand one of them another's calendar.
//
// The expected buckets come from civil arithmetic in this file. A three-month
// period opens when its first civil day does: at 00:00 local time for an
// all-day session. A session whose day runs from 17:00 names each session day
// by the civil date it ends on, so there the period opens at 17:00 on the day
// before. A completed bucket holds every bar from one such open to the next
// and is handed over on the last of them. Nothing is read back from the
// engine to form these values.
//
// Each count row prints
//   summary count calendar=<name> one_parse=.. fed=.. opens=.. fed_twice=..
//           opens_twice=.. second_aggregator=..
// and the calendars row prints one line per calendar and
//   summary calendars one_parse=.. zone_opens=..

#include <pineforge/bar.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <cstdarg>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
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

const std::string input_tf = "60";
const std::string target_tf = "3M";
constexpr std::int64_t hour_ms = 3600000;

struct Calendar {
    const char* name;
    const char* timezone;
    const char* session;
    // A three-month period opens at open_hour:00 local time on the civil day
    // open_day_shift days from the period's first civil day.
    int open_day_shift;
    int open_hour;
};

const Calendar utc_all_day{"utc_all_day", "UTC", "24x7", 0, 0};
const Calendar new_york_all_day{"new_york_all_day", "America/New_York", "24x7", 0, 0};
const Calendar new_york_from_17{"new_york_from_17", "America/New_York", "1700-1700", -1, 17};

#if defined(__linux__)
// The engine's opens, as this executable's open sees them. Static storage, so
// it is all zero before any open can arrive.
struct Watch {
    bool learning;
    bool counting;
    bool overflow;
    std::size_t learned;
    std::size_t counted;
    std::size_t known;
    char paths[4][PATH_MAX];
};
Watch watch;

using OpenFunction = int (*)(const char*, int, ...);
using CheckedOpenFunction = int (*)(const char*, int);

OpenFunction libc_open() noexcept {
    static const auto real = reinterpret_cast<OpenFunction>(::dlsym(RTLD_NEXT, "open"));
    return real;
}

#if defined(__GLIBC__)
CheckedOpenFunction libc_open_2() noexcept {
    static const auto real =
        reinterpret_cast<CheckedOpenFunction>(::dlsym(RTLD_NEXT, "__open_2"));
    return real;
}
#endif

bool known_path(const char* path) noexcept {
    for (std::size_t i = 0; i < watch.known; ++i) {
        if (std::strcmp(watch.paths[i], path) == 0) return true;
    }
    return false;
}

// While learning, every path opened is remembered. While counting, an open of
// a remembered path is counted.
void see_open(const char* path) noexcept {
    if (path == nullptr) return;
    if (watch.learning) {
        ++watch.learned;
        if (known_path(path)) return;
        if (watch.known == sizeof watch.paths / sizeof watch.paths[0]
            || std::strlen(path) >= sizeof watch.paths[0]) {
            watch.overflow = true;
            return;
        }
        std::strcpy(watch.paths[watch.known++], path);
    } else if (watch.counting && known_path(path)) {
        ++watch.counted;
    }
}

int seen_open(const char* path, int flags, bool has_mode, mode_t mode,
              CheckedOpenFunction checked = nullptr) {
    see_open(path);
    if (checked) return checked(path, flags);
    const auto real = libc_open();
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return has_mode ? real(path, flags, mode) : real(path, flags);
}
#endif

// An evaluator registration on a host that never runs.
class Registrar final : public source::PineStrategyHost {
public:
    void registration() { register_security_eval(0, "D", "", false, false); }

private:
    void on_source_bar(const Bar&) override {}
};

void evaluator_registration() {
    Registrar host;
    host.registration();
}

// A deterministic price walk; next() is in [-1, 1].
struct Walk {
    std::uint64_t state;
    double next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state % 20001u) / 10000.0 - 1.0;
    }
};

// One bar for every hour from 2025-03-17 00:00 UTC up to 2025-07-09 00:00
// UTC: no clock change falls inside it, and the periods that open on 1 April
// and on 1 July do.
std::vector<Bar> make_tape() {
    const std::int64_t first = nc::native_civil_days(2025, 3, 17) * 24 * hour_ms;
    const std::int64_t past = nc::native_civil_days(2025, 7, 9) * 24 * hour_ms;
    std::vector<Bar> tape;
    Walk walk{0x9E3779B97F4A7C15ULL};
    double price = 100.0;
    for (std::int64_t at = first; at < past; at += hour_ms) {
        const double first_price = price;
        const double last_price = first_price * (1.0 + 0.004 * walk.next());
        const double high = std::max(first_price, last_price)
            * (1.0 + 0.001 * std::fabs(walk.next()));
        const double low = std::min(first_price, last_price)
            * (1.0 - 0.001 * std::fabs(walk.next()));
        const double volume = 1000.0 + static_cast<double>(tape.size() % 17);
        tape.push_back(Bar{first_price, high, low, last_price, volume, at});
        price = last_price;
    }
    return tape;
}

struct Bucket {
    std::size_t handed_over_on = 0;
    std::size_t bars = 0;
    Bar values{};
};

Bar fold(const std::vector<Bar>& tape, std::size_t first, std::size_t past) {
    Bar out = tape[first];
    for (std::size_t i = first + 1; i < past; ++i) {
        out.high = std::max(out.high, tape[i].high);
        out.low = std::min(out.low, tape[i].low);
        out.close = tape[i].close;
        out.volume += tape[i].volume;
    }
    return out;
}

// The buckets this calendar's three-month periods close inside the tape: the
// one that ends where the period of 1 April opens and the one that ends where
// the period of 1 July opens.
std::vector<Bucket> expected_buckets(const Calendar& calendar, const std::vector<Bar>& tape) {
    std::vector<Bucket> out;
    std::size_t first = 0;
    for (const int month : {4, 7}) {
        const nc::NativeCivilDate day = nc::native_civil_date(
            nc::native_civil_days(2025, month, 1) + calendar.open_day_shift);
        const auto opens_at = nc::resolve_civil(calendar.timezone, static_cast<int>(day.year),
                                                day.month, day.day, calendar.open_hour, 0);
        CHECK(opens_at.has_value());
        if (!opens_at) return out;
        std::size_t next = first;
        while (next < tape.size() && tape[next].timestamp < opens_at->epoch_ms) ++next;
        // The period before this open holds bars, the tape goes on after it,
        // and the open falls on a bar's stamp.
        CHECK(next > first && next < tape.size());
        if (!(next > first && next < tape.size())) return out;
        CHECK(tape[next].timestamp == opens_at->epoch_ms);
        Bucket bucket;
        bucket.handed_over_on = next - 1;
        bucket.bars = next - first;
        bucket.values = fold(tape, first, next);
        out.push_back(bucket);
        first = next;
    }
    return out;
}

bool same_values(const Bar& a, const Bar& b) {
    return a.open == b.open && a.high == b.high && a.low == b.low && a.close == b.close
        && a.volume == b.volume;
}

std::int64_t next_stamp(const std::vector<Bar>& tape, std::size_t i) {
    return i + 1 < tape.size() ? tape[i + 1].timestamp : 0;
}

#if defined(__linux__)
// What a single parse of this calendar's text opens.
std::size_t one_parse_opens(const Calendar& calendar) {
    watch.learned = 0;
    watch.learning = true;
    const auto parsed = nc::parse_session(calendar.session, calendar.timezone);
    watch.learning = false;
    CHECK(parsed.has_value());
    CHECK(!watch.overflow);
    return watch.learned;
}

// The zone-file opens while a new aggregator of this calendar is fed the
// tape's first `bars` bars.
std::size_t opens_while_fed(const Calendar& calendar, const std::vector<Bar>& tape,
                            std::size_t bars) {
    TimeframeAggregator aggregator(target_tf, input_tf, calendar.timezone, calendar.session);
    watch.counted = 0;
    watch.counting = true;
    for (std::size_t i = 0; i < bars; ++i) {
        (void)aggregator.feed(tape[i], next_stamp(tape, i), 0, input_tf, target_tf);
    }
    watch.counting = false;
    return watch.counted;
}

void count_row(const Calendar& calendar, const std::vector<Bar>& tape) {
    const std::size_t fed = 240;
    CHECK(tape.size() >= 2 * fed);
    if (tape.size() < 2 * fed) return;
    const std::size_t one_parse = one_parse_opens(calendar);
    evaluator_registration();
    const std::size_t opens = opens_while_fed(calendar, tape, fed);
    evaluator_registration();
    const std::size_t opens_twice = opens_while_fed(calendar, tape, 2 * fed);
    const std::size_t second = opens_while_fed(calendar, tape, fed);
    std::printf("summary count calendar=%s one_parse=%zu fed=%zu opens=%zu fed_twice=%zu "
                "opens_twice=%zu second_aggregator=%zu\n", calendar.name, one_parse, fed, opens,
                2 * fed, opens_twice, second);
    // Not vacuous: parsing this calendar does open the zone file.
    CHECK(one_parse >= 1);
    CHECK(opens == one_parse);
    CHECK(opens_twice == one_parse);
    CHECK(second <= one_parse);
}
#endif

void calendars_row(const std::vector<Bar>& tape) {
    const Calendar* const calendars[] = {&utc_all_day, &new_york_all_day, &new_york_from_17};
    const std::size_t count = sizeof calendars / sizeof calendars[0];
    struct HandedOver {
        std::size_t on = 0;
        int bars = 0;
        Bar values{};
    };
    std::vector<TimeframeAggregator> aggregators;
    std::vector<std::vector<HandedOver>> handed_over(count);
    for (std::size_t c = 0; c < count; ++c) {
        aggregators.emplace_back(target_tf, input_tf, calendars[c]->timezone,
                                 calendars[c]->session);
    }
#if defined(__linux__)
    const std::size_t one_parse = one_parse_opens(new_york_all_day);
    CHECK(one_parse_opens(new_york_from_17) == one_parse);
#endif
    evaluator_registration();
#if defined(__linux__)
    watch.counted = 0;
    watch.counting = true;
#endif
    for (std::size_t i = 0; i < tape.size(); ++i) {
        for (std::size_t c = 0; c < count; ++c) {
            const AggregatedBar out =
                aggregators[c].feed(tape[i], next_stamp(tape, i), 0, input_tf, target_tf);
            if (out.is_complete) handed_over[c].push_back({i, out.sub_bar_count, out.bar});
        }
    }
#if defined(__linux__)
    watch.counting = false;
    const std::size_t zone_opens = watch.counted;
#endif
    for (std::size_t c = 0; c < count; ++c) {
        const std::vector<Bucket> expected = expected_buckets(*calendars[c], tape);
        const std::vector<HandedOver>& got = handed_over[c];
        std::size_t matched = 0;
        for (std::size_t k = 0; k < std::min(expected.size(), got.size()); ++k) {
            const bool same = got[k].on == expected[k].handed_over_on
                && static_cast<std::size_t>(got[k].bars) == expected[k].bars
                && same_values(got[k].values, expected[k].values);
            if (same) ++matched;
            std::printf("bucket calendar=%s n=%zu expected_bar=%zu handed_over_bar=%zu "
                        "expected_bars=%zu bars=%d %s\n", calendars[c]->name, k,
                        expected[k].handed_over_on, got[k].on, expected[k].bars, got[k].bars,
                        same ? "same" : "DIFFERENT");
        }
        std::printf("calendar %s: expected=%zu handed_over=%zu matched=%zu\n", calendars[c]->name,
                    expected.size(), got.size(), matched);
        CHECK(expected.size() == 2);
        CHECK(got.size() == expected.size());
        CHECK(matched == expected.size());
    }
    // The three calendars do not share a bucket boundary: a calendar answered
    // with another's would be seen above.
    const std::vector<Bucket> a = expected_buckets(utc_all_day, tape);
    const std::vector<Bucket> b = expected_buckets(new_york_all_day, tape);
    const std::vector<Bucket> c = expected_buckets(new_york_from_17, tape);
    CHECK(a.size() == 2 && b.size() == 2 && c.size() == 2);
    if (a.size() == 2 && b.size() == 2 && c.size() == 2) {
        for (std::size_t k = 0; k < 2; ++k) {
            CHECK(a[k].handed_over_on != b[k].handed_over_on);
            CHECK(b[k].handed_over_on != c[k].handed_over_on);
            CHECK(a[k].handed_over_on != c[k].handed_over_on);
        }
    }
#if defined(__linux__)
    std::printf("summary calendars one_parse=%zu zone_opens=%zu\n", one_parse, zone_opens);
    CHECK(one_parse >= 1);
    // Two calendars read the New York zone file, each parsed once.
    CHECK(zone_opens == 2 * one_parse);
#endif
}

}  // namespace

#if defined(__linux__)
// As in test_timezone_lookup_failure.cpp: glibc's optimized headers can route
// an open with nonconstant flags through __open_2, so both forms are seen. A
// separate C++ name for the plain export avoids redefining the header's
// inline wrapper; its ELF symbol remains open.
extern "C" int open_interposer(const char*, int, ...) __asm__("open");

extern "C" int open_interposer(const char* path, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = (flags & O_CREAT) != 0;
#if defined(O_TMPFILE)
    has_mode = has_mode || (flags & O_TMPFILE) == O_TMPFILE;
#endif
    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return seen_open(path, flags, has_mode, mode);
}

#if defined(__GLIBC__)
extern "C" int __open_2(const char* path, int flags) {
    return seen_open(path, flags, false, 0, libc_open_2());
}
#endif
#endif

int main() {
    const std::vector<Bar> tape = make_tape();
    CHECK(tape.size() == 114 * 24);
#if defined(__linux__)
    // Resolve the forwarding functions before anything is counted.
    CHECK(libc_open() != nullptr);
#if defined(__GLIBC__)
    (void)libc_open_2();
#endif
    count_row(new_york_all_day, tape);
    count_row(new_york_from_17, tape);
#else
    std::printf("count rows not run: they need ELF symbol interposition (Linux)\n");
#endif
    calendars_row(tape);
    std::printf("test_calendar_request_parse_once: %zu bars, %lld checks, %d failed\n",
                tape.size(), checks, failures);
    return failures == 0 ? 0 : 1;
}
