#pragma once

#include <pineforge/exit_leg_lifecycle.hpp>

#include <limits>
#include <optional>

namespace pineforge::compat::pine {

struct ExitSuspensionContext {
    exit_legs::Frame cause{};
    int direction = 0;
    double position_entry_price = std::numeric_limits<double>::quiet_NaN();
    double tick = std::numeric_limits<double>::quiet_NaN();
    double open = std::numeric_limits<double>::quiet_NaN();
    double prior_best = std::numeric_limits<double>::quiet_NaN();
    bool open_slice_this_bar = false;
    bool standing = true;
};

std::optional<exit_legs::Operation> select_exit_suspension(
    const exit_legs::Lifecycle&, const ExitSuspensionContext&);
exit_legs::Operation select_pair_hold(const exit_legs::Lifecycle&, exit_legs::Frame);
exit_legs::Definition select_replacement_revival_definition(const exit_legs::Lifecycle&);
double select_margin_revival_stop(const exit_legs::Lifecycle&);
std::optional<exit_legs::Operation> select_exit_completion(
    const exit_legs::Lifecycle&, exit_legs::Frame completed);

} // namespace pineforge::compat::pine
