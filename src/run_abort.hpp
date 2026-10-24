#pragma once

namespace pineforge {

// Invocation-local signal ingress, installed explicitly by the child harness.
// Separate from request_abort(): that older flag is observed inside a bar.
bool run_sigterm_requested() noexcept;

} // namespace pineforge
