#pragma once

#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace pineforge {

namespace native_calendar {
inline namespace native_calendar_v2 {
// Internal throwing form, deliberately absent from the installed headers.
bool timezone_lookup_accepted(std::string_view timezone);
}
}

namespace tz_util {

std::string normalize_timezone_for_posix(const std::string& tz);

// Resource failures cannot be treated as a request to use a fallback clock.
bool timezone_failure_is_resource(const std::exception& error) noexcept;

// Return the calendar-accepted spelling or raise pine_timezone_error through
// pine_timezone_stop with the function's timezone diagnostic.
const std::string& resolve_script_timezone(const std::string& timezone, const char* function);

// RAII guard: swaps the process ``TZ`` environment to the normalized ``tz``
// unless getenv("TZ") already matches, and holds this engine image's
// environment lock for the guard's ENTIRE lifetime, including localtime_r /
// mktime calls. Restores the previous TZ before unlocking, and only when this
// guard switched it. The exception is a guard whose restore a ZoneRestoreScope
// of its thread makes instead (below): that guard leaves its zone in force.
//
// The environment lock is the one lock every environment access in the
// engine's own sources takes: this guard's switch and restore, the libc
// conversions under it, and every other environment read (environment_value
// and EnvironmentLock below). It is one lock per engine image: a host's own
// code, and a second independently linked engine image with its own lock,
// are not serialized by it; cross-image parallel atomicity remains open.
//
// The lock is recursive. A guard taken under another guard of the same zone
// on the same thread finds that zone in force and neither switches nor
// restores, so a caller with many conversions in one zone holds one outer
// guard and pays one switch and one restore for all of them.
class ScopedTimezone {
public:
    explicit ScopedTimezone(const std::string& tz);
    ~ScopedTimezone() noexcept;

    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;

private:
    std::unique_lock<std::recursive_mutex> lock_;
    std::optional<std::string> previous_tz_;
    bool changed_ = false;
};

// Leaves the restore of the process TZ to the end of an engine call.
//
// A ScopedTimezone that switches TZ puts the caller's value back as it ends,
// and both the switch and the restore have libc read a zone file. A run asks
// for its zone again and again. So while a ZoneRestoreScope is open on a
// thread, a guard on that thread that switches TZ can leave its zone in force
// and the restore to the scope: the next guard for the same zone finds the
// zone there and switches nothing, and the thread's outermost scope puts the
// caller's value back once, as it closes. The entry points that feed input
// to a run open one each (engine_consumer.cpp). A batch run in one zone then
// switches once and restores once, when its call returns, and a streaming
// session does so once in each call that needs the zone.
//
// A guard leaves its restore to the scope only when all of this holds. Its
// thread has a scope open. No other guard is alive: a guard nested under
// another restores as it ends, because the outer one goes on converting in
// its own zone. No other thread's scope has a restore outstanding: what is
// owed is kept once per engine image and belongs to one thread at a time.
// And the value it replaced and the zone it installed are each shorter than
// 256 bytes. Any other guard restores what it found as it ends, as a guard
// outside every scope does. So once two runs on two threads have both
// returned the process has the caller's value, and while one of them holds
// the restore the other pays a switch and a restore at each guard for a
// different zone.
//
// The environment is still read and written under the environment lock only:
// the scope takes the lock to restore and holds it no longer than that.
// Between two guards of the thread that holds the restore, that thread does
// not hold the lock and TZ holds its zone, which other code in the process
// then sees.
//
// The value put back is the caller's own. Every guard reads TZ itself before
// it decides anything. When a guard of the thread that holds the restore has
// to switch and TZ is not the zone that thread left in force, the caller set
// TZ meanwhile and that value becomes the one put back. When TZ is not that
// zone as the scope closes, the caller set it last and it is left as it is.
// Two things cannot be told from the caller here. A caller that sets TZ to
// the very zone the thread left in force gets its earlier value back. And a
// zone left behind by a guard whose own restore failed, or by a second engine
// image running at the same time, is taken for the caller's.
class ZoneRestoreScope {
public:
    ZoneRestoreScope() noexcept;
    ~ZoneRestoreScope() noexcept;

    ZoneRestoreScope(const ZoneRestoreScope&) = delete;
    ZoneRestoreScope& operator=(const ZoneRestoreScope&) = delete;
};

// The environment lock with no zone switch, for a libc call that may read TZ
// where the caller has no zone to install (strftime on UTC fields).
class EnvironmentLock {
public:
    EnvironmentLock();

    EnvironmentLock(const EnvironmentLock&) = delete;
    EnvironmentLock& operator=(const EnvironmentLock&) = delete;

private:
    std::unique_lock<std::recursive_mutex> lock_;
};

// std::getenv(name) with the lookup made under the environment lock. For a
// name other than TZ: the engine writes TZ alone, so the text this returns is
// never rewritten by the engine and stays readable after the lock is
// released, as long as it did for a plain getenv. TZ itself is read by
// ScopedTimezone and ZoneRestoreScope only, under the lock for as long as
// either uses the text.
// Declared again by include/pineforge/detail/scratch_directory.hpp, whose
// inline TMPDIR read takes the same lock.
const char* environment_value(const char* name) noexcept;

}  // namespace tz_util

} // namespace pineforge
