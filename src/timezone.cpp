#include "timezone.hpp"
#include <pineforge/native_calendar.hpp>
#include <pineforge/run_failure.hpp>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace pineforge {
namespace tz_util {
namespace {

// The environment lock (timezone.hpp). In the engine's own sources every
// setenv, unsetenv and getenv, and every libc call that reads TZ, runs with
// it held; the one read outside it is the placement-audit build's log path
// at exit. Recursive: a read or a second guard under a guard cannot deadlock.
std::recursive_mutex& timezone_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

// glibc reads TZ, and TZDIR for a zone file, the first time any thread
// converts a time -- gmtime_r and localtime_r included -- and for those two
// never again. That first read is made inside libc, by a caller that need not
// hold the environment lock (the engine converts UTC instants with gmtime_r
// outside it). So the first guard that is about to write the environment asks
// for one conversion here: gmtime_r takes libc's own zone lock, and when it
// returns libc's zone state exists, read either by this call or by the one it
// waited for, before the environment is written. No later gmtime_r or
// localtime_r reads the environment. Called with the environment lock held,
// which is what guards `settled`.
void settle_libc_zone_state() noexcept {
    static bool settled = false;
    if (settled) return;
    const std::time_t epoch = 0;
    std::tm fields{};
    (void)::gmtime_r(&epoch, &fields);
    settled = true;
}

// Room for one TZ value kept across guards (below): a zone name, a POSIX
// rule or a file path. A guard that meets a longer one keeps nothing and
// restores as it ends.
constexpr std::size_t kKeptZoneSize = 256;

// The restore a ZoneRestoreScope owes (timezone.hpp): the value to put back,
// which is what TZ held before the scope's thread first switched it and left
// the switch in force, or what the caller has set TZ to since, and the zone
// that thread left in force last. One per engine image, read and written
// with the environment lock held. While `owed` is set it belongs to the one
// thread whose tl_restore_owed is set, and no other thread writes it.
struct OwedRestore {
    bool owed = false;
    bool caller_had_zone = false;
    char caller_zone[kKeptZoneSize] = {};
    char engine_zone[kKeptZoneSize] = {};
};
OwedRestore owed_restore;

// The ZoneRestoreScope objects open on the calling thread, and whether the
// outermost of them has the restore above to make when it closes.
thread_local int tl_restore_scopes = 0;
thread_local bool tl_restore_owed = false;

// The ScopedTimezone objects alive. A guard holds the environment lock for
// its whole life, so they all belong to the thread that holds the lock, and a
// guard that finds one alive is nested under it. Read and written with the
// lock held.
int live_guards = 0;

// Puts TZ back to `zone` (null: unset) and has libc read it, with the
// environment lock held. A failure leaves TZ switched and says so on
// standard error.
void restore_zone(const char* zone) noexcept {
    const int restored = zone != nullptr ? ::setenv("TZ", zone, 1) : ::unsetenv("TZ");
    if (restored == 0) {
        ::tzset();
        return;
    }
    if (zone == nullptr) zone = "<unset>";
    ::flockfile(stderr);
    std::fputs("pineforge: could not restore TZ to '", stderr);
    for (; *zone; ++zone) {
        const auto byte = static_cast<unsigned char>(*zone);
        if (byte >= 32 && byte < 127) std::fputc(byte, stderr);
        else std::fprintf(stderr, "\\x%02x", static_cast<unsigned>(byte));
    }
    std::fputs("'\n", stderr);
    ::funlockfile(stderr);
}

// Asked by a guard no other guard is alive under, with the environment lock
// held, right after it switched TZ from `previous` (null: unset) to
// `installed`: whether a ZoneRestoreScope of the calling thread makes the
// restore, so that the guard leaves TZ switched as it ends. False outside
// every scope, while the restore owed is another thread's, and for a value
// kKeptZoneSize has no room for: the guard then restores `previous` itself,
// and nothing recorded here changes.
bool restore_left_to_scope(const char* previous, const char* installed) noexcept {
    if (tl_restore_scopes == 0) return false;
    OwedRestore& debt = owed_restore;
    if (debt.owed && !tl_restore_owed) return false;
    const std::size_t previous_size = previous != nullptr ? std::strlen(previous) : 0;
    const std::size_t installed_size = std::strlen(installed);
    if (previous_size >= kKeptZoneSize || installed_size >= kKeptZoneSize) return false;
    // TZ held the caller's own value unless it held the zone this thread left
    // in force: at the thread's first switch, and whenever the caller has set
    // TZ since. That value is the one to put back.
    if (!debt.owed || previous == nullptr || std::strcmp(previous, debt.engine_zone) != 0) {
        debt.caller_had_zone = previous != nullptr;
        std::memcpy(debt.caller_zone, previous != nullptr ? previous : "", previous_size + 1);
    }
    std::memcpy(debt.engine_zone, installed, installed_size + 1);
    debt.owed = true;
    tl_restore_owed = true;
    return true;
}

// std::stoi of a UTC offset's hours or minutes. A timezone the script builds
// at run time can fail it (too many digits): the same exception type and text
// as ever, coded pine_invalid_argument. Valid input takes exactly the path it
// always took.
int timezone_offset_part(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (const std::invalid_argument& error) {
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_invalid_argument,
            {{"argument", "timezone"}, {"rule", "unparseable"}}, error.what());
    } catch (const std::out_of_range& error) {
        throw coded<std::out_of_range>(
            RunFailureCode::pine_invalid_argument,
            {{"argument", "timezone"}, {"rule", "unparseable"}}, error.what());
    }
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char ch : s) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

}  // namespace

bool timezone_failure_is_resource(const std::exception& error) noexcept {
    return dynamic_cast<const std::bad_alloc*>(&error) != nullptr
        || dynamic_cast<const std::length_error*>(&error) != nullptr
        || dynamic_cast<const std::system_error*>(&error) != nullptr
        || run_failure_code_class(classify_run_failure(error).code) == RunFailureClass::resource;
}

const std::string& resolve_script_timezone(const std::string& timezone, const char* function) {
    // Calendar accepts these spellings before any zone-file lookup. They
    // neither consult nor evict the accepted-name memo.
    if (timezone.empty() || timezone == "UTC" || timezone == "Etc/UTC"
        || timezone == "GMT" || timezone == "Etc/GMT") {
        return timezone;
    }
    // Match the local-calendar memo's fixed eight-slot shape. Store only
    // accepted names that fit; longer names are validated on every call.
    struct AcceptedTimezone {
        char name[48];
        std::size_t size;
        std::uint64_t last_read;
    };
    thread_local AcceptedTimezone accepted[8] = {};
    thread_local std::uint64_t clock = 0;
    for (auto& slot : accepted) {
        if (slot.size != 0 && slot.size == timezone.size()
            && std::memcmp(slot.name, timezone.data(), slot.size) == 0) {
            slot.last_read = ++clock;
            return timezone;
        }
    }
    // A failed lookup propagates before any memo write; only an absent or
    // invalid zone name is diagnosed as an incorrect argument.
    if (!native_calendar::timezone_lookup_accepted(timezone)) {
        const std::string english = std::string("Incorrect `timezone` value for the ") + function
            + "() function: \"" + timezone
            + "\". The value must be a string in either UTC/GMT notation "
              "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
              "(e.g. \"America/New_York\").";
        pine_timezone_stop(function, timezone.c_str(), english.c_str());
    }
    if (timezone.size() <= sizeof(accepted[0].name)) {
        auto* slot = &accepted[0];
        for (auto& candidate : accepted) {
            if (candidate.size == 0) {
                slot = &candidate;
                break;
            }
            if (candidate.last_read < slot->last_read) slot = &candidate;
        }
        std::memcpy(slot->name, timezone.data(), timezone.size());
        slot->size = timezone.size();
        slot->last_read = ++clock;
    }
    return timezone;
}

std::string normalize_timezone_for_posix(const std::string& tz) {
    if (tz.empty() || tz == "UTC" || tz == "Etc/UTC" || tz == "GMT" ||
        tz == "Etc/GMT") {
        return "UTC";
    }

    std::size_t prefix = std::string::npos;
    if (tz.rfind("GMT", 0) == 0) {
        prefix = 3;
    } else if (tz.rfind("UTC", 0) == 0) {
        prefix = 3;
    }
    if (prefix == std::string::npos || prefix >= tz.size()) {
        return tz;
    }

    char offset_sign = tz[prefix];
    if (offset_sign != '+' && offset_sign != '-') {
        return tz;
    }

    std::string body = tz.substr(prefix + 1);
    std::string hour_s;
    std::string minute_s;
    std::size_t colon = body.find(':');
    if (colon != std::string::npos) {
        hour_s = body.substr(0, colon);
        minute_s = body.substr(colon + 1);
    } else if (body.size() > 2) {
        hour_s = body.substr(0, body.size() - 2);
        minute_s = body.substr(body.size() - 2);
    } else {
        hour_s = body;
        minute_s = "0";
    }

    if (!all_digits(hour_s) || !all_digits(minute_s)) {
        return tz;
    }

    int hours = timezone_offset_part(hour_s);
    int minutes = timezone_offset_part(minute_s);
    if (hours > 23 || minutes > 59) {
        return tz;
    }
    if (hours == 0 && minutes == 0) {
        return "UTC";
    }

    char posix_sign = (offset_sign == '+') ? '-' : '+';
    std::ostringstream out;
    out << "UTC" << posix_sign << hours;
    if (minutes != 0) {
        out << ':' << (minutes < 10 ? "0" : "") << minutes;
    }
    return out.str();
}

// Hold this engine image's environment lock for the lifetime of the guard,
// including the caller's localtime_r / mktime decomposition. Independently
// linked engine images have separate locks; cross-image parallel atomicity
// remains open.
//
// Compare with the actual process environment so a preceding TZ switch by
// another engine image cannot leave an image-local last-zone cache stale.
// Skip setenv/tzset only when TZ already equals the normalized target: that
// is also what a guard finds under an outer guard of the same zone, and after
// an earlier guard of its thread's ZoneRestoreScope left that zone in force,
// so it writes nothing and its destructor restores nothing.
ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex()) {

    std::string target = normalize_timezone_for_posix(tz);

    const char* active_tz = std::getenv("TZ");
    if (active_tz != nullptr && target == active_tz) {
        ++live_guards;
        return;  // lock stays held until destruction
    }

    if (active_tz != nullptr) {
        previous_tz_.emplace(active_tz);
    }
    settle_libc_zone_state();
    // POSIX setenv leaves the environment unchanged on failure. In particular,
    // ENOMEM would otherwise make the caller read the previous zone.
    if (::setenv("TZ", target.c_str(), 1) != 0) {
        const int error = errno;
        if (error == ENOMEM) {
            throw coded<std::bad_alloc>(RunFailureCode::out_of_memory, {}, std::bad_alloc{});
        }
        throw coded<std::system_error>(
            RunFailureCode::engine_unclassified_error, {},
            std::system_error(error, std::generic_category(), "setenv(TZ) failed"));
    }
    // This guard restores as it ends. Only a guard nested under no other may
    // leave that to a ZoneRestoreScope of its thread: the guard above a nested
    // one goes on converting in its own zone when the nested one has ended.
    changed_ = live_guards != 0
        || !restore_left_to_scope(previous_tz_ ? previous_tz_->c_str() : nullptr,
                                  target.c_str());
    ::tzset();
    ++live_guards;
}

// The restore is made before lock_ is released (a member, destroyed after
// this body), so it is under the same lock as the switch it undoes and as
// every environment read of this image. A guard that switched nothing, or
// whose switch a ZoneRestoreScope undoes, has nothing to restore and leaves
// the environment alone.
ScopedTimezone::~ScopedTimezone() noexcept {
    --live_guards;
    if (!changed_) return;
    restore_zone(previous_tz_ ? previous_tz_->c_str() : nullptr);
}

ZoneRestoreScope::ZoneRestoreScope() noexcept {
    ++tl_restore_scopes;
}

// The thread's outermost scope makes the restore its guards left to it, under
// the environment lock like every other write. TZ is put back only while it
// still holds the zone this thread left in force: any other value is one the
// caller set after the thread's last switch, and it stands.
ZoneRestoreScope::~ZoneRestoreScope() noexcept {
    if (--tl_restore_scopes != 0 || !tl_restore_owed) return;
    tl_restore_owed = false;
    const std::lock_guard<std::recursive_mutex> lock(timezone_mutex());
    OwedRestore& debt = owed_restore;
    debt.owed = false;
    const char* const active_tz = std::getenv("TZ");
    if (active_tz == nullptr || std::strcmp(active_tz, debt.engine_zone) != 0) return;
    restore_zone(debt.caller_had_zone ? debt.caller_zone : nullptr);
}

EnvironmentLock::EnvironmentLock()
    : lock_(timezone_mutex()) {}

const char* environment_value(const char* name) noexcept {
    const std::lock_guard<std::recursive_mutex> lock(timezone_mutex());
    return std::getenv(name);
}

}  // namespace tz_util

std::string normalize_timezone_for_posix(const std::string& tz) {
    return tz_util::normalize_timezone_for_posix(tz);
}

} // namespace pineforge
