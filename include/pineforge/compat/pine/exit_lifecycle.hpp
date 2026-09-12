#pragma once
#include "../../exit_leg_lifecycle.hpp"
namespace pineforge { inline namespace engine_script_run_v12 { struct PendingOrder; } }
namespace pineforge::compat::pine {
struct ExitSuspensionContext {
    exit_legs::Frame cause;
    int direction;
    double position_entry_price;
    double tick;
    double open;
    double prior_best;
    bool open_slice_this_bar;
    bool standing;
};
std::optional<exit_legs::Operation> select_exit_suspension(
    const PendingOrder&, const ExitSuspensionContext&);
exit_legs::Operation select_pair_hold(const PendingOrder&, exit_legs::Frame);
exit_legs::Definition select_replacement_revival_definition(const PendingOrder&);
double select_margin_revival_stop(const PendingOrder&);
std::optional<exit_legs::Operation> select_exit_completion(
    const PendingOrder&, exit_legs::Frame completed);
} // namespace pineforge::compat::pine
