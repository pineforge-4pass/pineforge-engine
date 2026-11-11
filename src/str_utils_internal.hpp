#pragma once

#include <regex>

namespace pineforge::detail {

enum class RegexOutcome { invalid_pattern, resource_limit };

inline RegexOutcome classify_regex_error(std::regex_constants::error_type code) noexcept {
    switch (code) {
    case std::regex_constants::error_space:
    case std::regex_constants::error_complexity:
    case std::regex_constants::error_stack:
        return RegexOutcome::resource_limit;
    default:
        return RegexOutcome::invalid_pattern;
    }
}

} // namespace pineforge::detail
