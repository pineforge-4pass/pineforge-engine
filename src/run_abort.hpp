#pragma once

namespace pineforge {

// Invocation-local signal ingress, installed explicitly by the child harness.
// Separate from request_abort(): that older flag is observed inside a bar.
bool run_sigterm_requested() noexcept;

// Signal-safe process latch, separate from the invocation's SIGTERM handler.
// A sandbox refusal must also be visible before the host installs SIGTERM.
void run_abort_request_sandbox() noexcept;
bool run_sandbox_abort_requested() noexcept;

} // namespace pineforge
