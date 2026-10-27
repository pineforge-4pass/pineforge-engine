#pragma once

#include <cstdint>

namespace pineforge {

// Invocation-local signal ingress, installed explicitly by the child harness.
// Separate from request_abort(): that older flag is observed inside a bar.
bool run_sigterm_requested() noexcept;

// Invocation-local admission/clock state, not strategy or broker state.
// One serialized host invocation; installation and checks must not race.
struct RunBudgetCheck {
    const char* kind = nullptr;
    std::int64_t limit = 0;
    std::int64_t used = 0;
    bool clock_failed = false;
};

// Read enabled clocks only at the same stable boundaries as SIGTERM.
RunBudgetCheck check_run_budget() noexcept;

} // namespace pineforge
