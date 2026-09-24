#pragma once

// R5 lane V19-E: the test-only switch behind the retired-row erasure.
//
// PineExecutionAdapter::erase_retired_rows erases, at every bar open, the
// placement row of each request that is no longer working and that nothing
// can read again. The equivalence witness
// (tests/test_adapter_live_state_equivalence.cpp) runs every script twice --
// erasing, and with every row retained as before V19-E -- and holds the two
// runs' observable transcripts equal bar for bar. The shipped library never
// sets the switch. Not part of the installed API.

#include <cstdint>

namespace pineforge::source::detail {

// While set, no placement row is erased (process-wide: set it only around
// single-threaded runs).
void set_retain_retired_rows(bool retain) noexcept;
bool retain_retired_rows() noexcept;
// How many rows the process's adapters have erased (a statistic).
std::uint64_t retired_rows_erased() noexcept;

}  // namespace pineforge::source::detail
