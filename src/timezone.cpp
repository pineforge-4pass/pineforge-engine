#include "timezone.hpp"
#include <pineforge/native_calendar.hpp>
#include <pineforge/run_failure.hpp>
#include <cerrno>
#include <cctype>
#include <cstdint>
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

std::mutex& timezone_mutex() {
    static std::mutex mutex;
    return mutex;
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
    if (!native_calendar::timezone_accepted(timezone)) {
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

// Hold this engine image's TZ mutex for the lifetime of the guard, including
// the caller's localtime_r / mktime decomposition. Independently linked engine
// images have separate mutexes; cross-image parallel atomicity remains open.
//
// Compare with the actual process environment so a preceding TZ switch by
// another engine image cannot leave an image-local last-zone cache stale.
// Skip setenv/tzset only when TZ already equals the normalized target.
ScopedTimezone::ScopedTimezone(const std::string& tz)
    : lock_(timezone_mutex()) {

    std::string target = normalize_timezone_for_posix(tz);

    const char* active_tz = std::getenv("TZ");
    if (active_tz != nullptr && target == active_tz) {
        return;  // lock stays held until destruction
    }

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
    ::tzset();
}

ScopedTimezone::~ScopedTimezone() {
    // lock_ releases the mutex here. TZ is intentionally NOT restored —
    // the next scope checks the process environment before skipping setenv/tzset.
}

}  // namespace tz_util

std::string normalize_timezone_for_posix(const std::string& tz) {
    return tz_util::normalize_timezone_for_posix(tz);
}

} // namespace pineforge
