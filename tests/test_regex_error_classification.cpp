#include <cstdio>
#include <regex>

#include "../src/str_utils_internal.hpp"

int main() {
    using pineforge::detail::RegexOutcome;
    const struct {
        std::regex_constants::error_type code;
        RegexOutcome expected;
        const char* name;
    } rows[] = {
        {std::regex_constants::error_collate, RegexOutcome::invalid_pattern, "error_collate"},
        {std::regex_constants::error_ctype, RegexOutcome::invalid_pattern, "error_ctype"},
        {std::regex_constants::error_escape, RegexOutcome::invalid_pattern, "error_escape"},
        {std::regex_constants::error_backref, RegexOutcome::invalid_pattern, "error_backref"},
        {std::regex_constants::error_brack, RegexOutcome::invalid_pattern, "error_brack"},
        {std::regex_constants::error_paren, RegexOutcome::invalid_pattern, "error_paren"},
        {std::regex_constants::error_brace, RegexOutcome::invalid_pattern, "error_brace"},
        {std::regex_constants::error_badbrace, RegexOutcome::invalid_pattern, "error_badbrace"},
        {std::regex_constants::error_range, RegexOutcome::invalid_pattern, "error_range"},
        {std::regex_constants::error_space, RegexOutcome::resource_limit, "error_space"},
        {std::regex_constants::error_badrepeat, RegexOutcome::invalid_pattern, "error_badrepeat"},
        {std::regex_constants::error_complexity, RegexOutcome::resource_limit, "error_complexity"},
        {std::regex_constants::error_stack, RegexOutcome::resource_limit, "error_stack"},
    };
    const auto outcome_name = [](RegexOutcome outcome) {
        return outcome == RegexOutcome::invalid_pattern ? "invalid_pattern" : "resource_limit";
    };
    unsigned count = 0;
    unsigned wrong = 0;
    for (const auto& row : rows) {
        const RegexOutcome got = pineforge::detail::classify_regex_error(row.code);
        const bool ok = got == row.expected;
        ++count;
        if (!ok) ++wrong;
        std::printf("ROW %s expected=%s got=%s %s\n", row.name, outcome_name(row.expected),
                    outcome_name(got), ok ? "ok" : "WRONG");
    }
    std::printf("COUNT rows=%u wrong=%u\n", count, wrong);
    return count == 13 && wrong == 0 ? 0 : 1;
}
