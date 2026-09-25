#pragma once

// R5 lane V19-D: the test-only switch behind the adapter's re-issue binding.
//
// PineExecutionAdapter::submit_or_replace re-issues a source order through
// NativeStrategyHost::replace with ReplaceOptions::keep_binding set, so an
// exit leg bound to the position keeps its binding across the re-issue and the
// kernel records no CloseBoundEvent for it at its next point (the kernel binds
// it exactly as a plain replace would whenever the position has moved). The
// adapter witness (tests/test_adapter_live_state_equivalence.cpp,
// --reissue-binding) runs every script
// twice -- carrying, and with every re-issue a plain replace as before V19-D --
// and holds the two runs' transcripts equal bar for bar, CloseBoundEvents and
// the ordinals they shift aside. The shipped library never clears the switch.
// Not part of the installed API.

namespace pineforge::source::detail {

// While cleared, every re-issue is a plain replace (process-wide: set it only
// around single-threaded runs).
void set_carry_reissue_bindings(bool carry) noexcept;
bool carry_reissue_bindings() noexcept;

}  // namespace pineforge::source::detail
