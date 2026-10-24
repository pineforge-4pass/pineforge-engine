#include <pineforge/detail/resident_budget.hpp>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>

namespace pineforge::detail {

[[noreturn]] void throw_resident_budget_invalid(ResidentKind kind, std::string_view value,
                                             std::uint64_t max, std::string_view reason) {
    const auto& row = resident_budget(kind);
    const auto message = std::string("resident budget kind=") + row.name
        + " value=\"" + std::string(value) + "\" max=" + std::to_string(max);
    // Match run_json's 1 KiB string-argument cap, keeping valid UTF-8 whole.
    std::size_t length = value.size() < 1024 ? value.size() : 1024;
    if (length < value.size()) {
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
            --length;
    }
    RunFailureArgs args{{"reason", std::string(reason)}, {"stage", "resident_budget"},
                        {"kind", row.name}, {"value", std::string(value.substr(0, length))}};
    if (max <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        args.emplace_back("max", static_cast<std::int64_t>(max));
    else
        args.emplace_back("max", std::to_string(max));
    const int error = reason == "overflow" || reason == "above_maximum" ? ERANGE : EINVAL;
    throw coded<std::system_error>(RunFailureCode::scratch_configuration_invalid,
        args, std::system_error(error, std::generic_category(), message));
}

std::uint64_t resident_entries(ResidentKind kind) {
    const auto& row = resident_budget(kind);
    const char* selected = std::getenv(row.env_name);
    if (!selected) return row.default_entries;
    if (!*selected) throw_resident_budget_invalid(kind, selected, row.max_entries, "nondecimal");
    std::uint64_t entries = 0;
    for (const char* at = selected; *at; ++at) {
        if (*at < '0' || *at > '9')
            throw_resident_budget_invalid(kind, selected, row.max_entries, "nondecimal");
        const auto digit = static_cast<std::uint64_t>(*at - '0');
        if (entries > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw_resident_budget_invalid(kind, selected, row.max_entries, "overflow");
        entries = entries * 10 + digit;
    }
    if (entries == 0) throw_resident_budget_invalid(kind, selected, row.max_entries, "nonpositive");
    if (entries > row.max_entries)
        throw_resident_budget_invalid(kind, selected, row.max_entries, "above_maximum");
    return entries;
}

std::uint64_t planned_resident_bytes() {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kResidentBudgets.size(); ++i) {
        const auto kind = static_cast<ResidentKind>(i);
        const auto entries = resident_entries(kind);
        const auto bytes = resident_bytes(kind, entries);
        if (!bytes || *bytes > std::numeric_limits<std::uint64_t>::max() - total) {
            const auto value = std::to_string(entries);
            throw_resident_budget_invalid(kind, value, resident_budget(kind).max_entries, "overflow");
        }
        total += *bytes;
    }
    return total;
}

} // namespace pineforge::detail
