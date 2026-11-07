// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "json.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace pineforge::live {

inline constexpr double kDefaultLedgerWarnFreeFraction = 0.8;

inline double ledger_warn_free_fraction(const std::string& text) {
    try {
        const auto value = parse_json(text).real();
        if (value > 0.0 && value <= 1.0) return value;
    } catch (const std::exception&) {}
    throw std::runtime_error("ledger-warn-free-fraction must be finite and 0 < F <= 1");
}

// Advisory observations only. Filesystem measurement errors become nulls;
// this object has no connection to admission, delivery or readiness.
class LedgerObservability {
public:
    explicit LedgerObservability(std::string path,
            double warn_fraction = kDefaultLedgerWarnFreeFraction, bool track_growth = true)
        : path_(std::move(path)), warn_fraction_(warn_fraction),
          started_(Clock::now()), initial_bytes_(track_growth ? footprint() : std::nullopt) {}

    Json sample(std::uint64_t inputs) const {
        const auto used = footprint();
        auto parent = std::filesystem::path(path_).parent_path();
        if (parent.empty()) parent = ".";
        std::error_code error;
        const auto space = std::filesystem::space(parent, error);
        const bool have_free = !error &&
            space.available != std::numeric_limits<std::uintmax_t>::max();
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started_).count();
        const bool growing = used && initial_bytes_ && *used > *initial_bytes_ && elapsed > 0;
        const double growth = growing ? static_cast<double>(*used - *initial_bytes_) / elapsed : 0;
        const bool warning = used && have_free &&
            static_cast<long double>(*used) >= warn_fraction_ * static_cast<long double>(space.available);
        return Json::object({
            {"used_bytes", used ? integer(*used) : Json{}},
            {"bytes_per_input", used && inputs ? number(static_cast<double>(*used) / inputs) : Json{}},
            {"free_disk_bytes", have_free ? integer(space.available) : Json{}},
            {"growth_bytes_per_second", growing ? number(growth) : Json{}},
            {"seconds_to_disk_exhaustion", growing && have_free
                ? number(static_cast<double>(space.available) / growth) : Json{}},
            {"warn_free_fraction", number(warn_fraction_)},
            {"state", Json::string(!used || !have_free ? "UNKNOWN" : warning ? "WARNING" : "OK")},
            {"growth_start_bytes", initial_bytes_ ? integer(*initial_bytes_) : Json{}},
            {"growth_elapsed_seconds", number(elapsed)}});
    }

private:
    using Clock = std::chrono::steady_clock;
    static Json integer(std::uintmax_t value) { return Json::number(std::to_string(value)); }
    static Json number(double value) {
        if (!std::isfinite(value)) return Json{};
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(17) << value;
        return Json::number(text.str());
    }
    std::optional<std::uintmax_t> footprint() const {
        std::uintmax_t bytes = 0;
        for (const auto* suffix : {"", "-wal"}) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path_ + suffix, error);
            if (error == std::errc::no_such_file_or_directory && *suffix) continue;
            if (error || size > std::numeric_limits<std::uintmax_t>::max() - bytes)
                return std::nullopt;
            bytes += size;
        }
        return bytes;
    }
    std::string path_;
    double warn_fraction_;
    Clock::time_point started_;
    std::optional<std::uintmax_t> initial_bytes_;
};

}
