#pragma once

#include "../command_scratch_production.hpp"

// Source callers share the kernel run owner's production factory, observer
// and byte budget. Keep these spellings for existing source integrations.
namespace pineforge::source::detail {
using pineforge::detail::CommandScratchByteBudget;
using pineforge::detail::CommandScratchByteCap;
using pineforge::detail::make_command_scratch_backing;
using pineforge::detail::set_command_scratch_byte_cap;
using pineforge::detail::set_command_scratch_io_observer;
} // namespace pineforge::source::detail
