#pragma once
#include "../../exit_leg_lifecycle.hpp"
namespace pineforge::source { struct PendingOrder; }
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
    const source::PendingOrder&, const ExitSuspensionContext&);
exit_legs::Operation select_pair_hold(const source::PendingOrder&, exit_legs::Frame);
exit_legs::Definition select_replacement_revival_definition(const source::PendingOrder&);
double select_margin_revival_stop(const source::PendingOrder&);
std::optional<exit_legs::Operation> select_exit_completion(
    const source::PendingOrder&, exit_legs::Frame completed);
} // namespace pineforge::compat::pine
