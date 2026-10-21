#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <cstddef>
#include <memory>

namespace pineforge::source::detail {

// The run owner installs its observer on the thread creating command scopes.
// Each backing snapshots that observer; its context must outlive the backing.
// Registration starts empty on each thread. Returns the previous registration
// so nested run owners can restore it. Changing it does not retarget live files.
pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept;

// Fresh, exclusive, initially empty disk-file backing. The directory comes from
// PINEFORGE_COMMAND_SCRATCH_DIR, defaulting to /tmp when unset or empty. There
// is no fallback when the selected directory fails. The unique file is unlinked
// before returning and its close-on-exec descriptor is closed on destruction.
// Zero page size and sizes beyond off_t throw std::invalid_argument.
// OS setup failures throw std::system_error.
std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size);

} // namespace pineforge::source::detail
