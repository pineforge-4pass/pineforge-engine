#include <pineforge/detail/resident_budget.hpp>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>

namespace pineforge::detail {
namespace {

[[noreturn]] void budget_error(const ResidentBudgetDescriptor& row, const char* value,
                              const char* reason, int error) {
    const auto message = std::string("resident budget kind=") + row.name
        + " value=\"" + value + "\" max=" + std::to_string(row.max_entries);
    // Code 68's current closed registry requires this legacy stage spelling.
    // Keep the actual kind/value/max in the message until its owner extends
    // the vocabulary; empty args or a new stage would become engine_invariant.
    throw coded<std::system_error>(RunFailureCode::scratch_configuration_invalid,
        {{"reason", reason}, {"stage", "command_scratch_byte_cap"}},
        std::system_error(error, std::generic_category(), message));
}

} // namespace

std::uint64_t resident_entries(ResidentKind kind) {
    const auto& row = resident_budget(kind);
    const char* selected = std::getenv(row.env_name);
    if (!selected) return row.default_entries;
    if (!*selected) budget_error(row, selected, "nondecimal", EINVAL);
    std::uint64_t entries = 0;
    for (const char* at = selected; *at; ++at) {
        if (*at < '0' || *at > '9') budget_error(row, selected, "nondecimal", EINVAL);
        const auto digit = static_cast<std::uint64_t>(*at - '0');
        if (entries > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            budget_error(row, selected, "overflow", ERANGE);
        entries = entries * 10 + digit;
    }
    if (entries == 0) budget_error(row, selected, "nonpositive", EINVAL);
    if (entries > row.max_entries) budget_error(row, selected, "overflow", ERANGE);
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
            budget_error(resident_budget(kind), value.c_str(), "overflow", ERANGE);
        }
        total += *bytes;
    }
    return total;
}

} // namespace pineforge::detail
