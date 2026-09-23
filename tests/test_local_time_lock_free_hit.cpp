// R5 lane PERF-P5611, item P11. A zoned time read that falls inside an
// offset interval the calling thread has already established does not take
// the process-global timezone lock.
//
// Until this lane every hour(time, tz) / minute() / dayofweek() / session
// filter read in a zone other than UTC entered tz_util::ScopedTimezone: the
// process mutex, the zone's normalization strings and, whenever the previous
// read was in another zone, setenv + tzset (KI-35: a notifyd round trip on
// macOS). decompose_ms_local (src/session_time.cpp) now reads the thread's
// memoised offset intervals and asks localtime_r, under that lock, only for a
// stamp outside them.
//
// The witness is the lock itself, so it cannot pass by timing luck: a worker
// thread reads two stamps a minute apart in New York (the second read joins
// them and probes the interval twelve hours on), then the main thread takes
// the process timezone lock and holds it while the worker reads a third stamp
// between them. Before this lane that read blocks on the lock until the main
// thread gives up waiting (five seconds) and releases it; now it returns at
// once, with the hour libc gives. A UTC read never took the lock, before or
// after: it is the control.
#include <pineforge/session_time.hpp>

#include "../src/timezone.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace pineforge;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t kJuly = 1720000000000LL;  // 2024-07-03T09:46:40Z, far from a DST edge
const std::string kZone = "America/New_York";

int libc_hour(std::int64_t ms, const std::string& tz) {
    const time_t secs = static_cast<time_t>(ms / 1000);
    struct tm fields {};
    tz_util::ScopedTimezone guard(tz);
    localtime_r(&secs, &fields);
    return fields.tm_hour;
}

// Runs `read` on a worker thread that first warms the zone with `warm`, while
// the main thread holds the process timezone lock; true when `read` returned
// before the main thread's patience ran out.
template <class Warm, class Read>
bool returns_while_the_lock_is_held(Warm warm, Read read) {
    std::mutex gate;
    std::condition_variable signal;
    bool warmed = false;
    bool go = false;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        warm();
        {
            std::lock_guard<std::mutex> lock(gate);
            warmed = true;
        }
        signal.notify_all();
        {
            std::unique_lock<std::mutex> lock(gate);
            signal.wait(lock, [&] { return go; });
        }
        read();
        done.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        signal.wait(lock, [&] { return warmed; });
    }
    bool returned = false;
    {
        tz_util::ScopedTimezone held("Asia/Tokyo");  // the process timezone lock
        {
            std::lock_guard<std::mutex> lock(gate);
            go = true;
        }
        signal.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        returned = done.load();
    }
    worker.join();
    return returned;
}

void a_memoised_zoned_read_does_not_wait_for_the_lock() {
    const int expected = libc_hour(kJuly + 120000, kZone);
    int got = -1;
    const bool returned = returns_while_the_lock_is_held(
        [] {
            (void)local_hour(kJuly, kZone);
            (void)local_hour(kJuly + 60000, kZone);
        },
        [&] { got = local_hour(kJuly + 120000, kZone); });
    CHECK(returned);
    CHECK(got == expected);
}

void a_utc_read_never_waited_for_it() {
    int got = -1;
    const bool returned = returns_while_the_lock_is_held(
        [] {}, [&] { got = local_hour(kJuly, "UTC"); });
    CHECK(returned);
    CHECK(got == 9);
}

}  // namespace

int main() {
    a_utc_read_never_waited_for_it();
    a_memoised_zoned_read_does_not_wait_for_the_lock();
    if (failures == 0) std::printf("test_local_time_lock_free_hit: ok\n");
    return failures == 0 ? 0 : 1;
}
