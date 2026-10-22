#pragma once

#include <pineforge/detail/scratch_backing.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pineforge::detail {

// The run owner installs its observer on the thread creating command scopes.
// Each backing snapshots that observer; its context must outlive the backing.
// Registration starts empty on each thread. Returns the previous registration
// so nested run owners can restore it. Changing it does not retarget live files.
pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept;

struct CommandScratchByteBudget;
using CommandScratchByteCap = std::shared_ptr<CommandScratchByteBudget>;

// Each numeric registration starts a fresh run budget on this thread; zero
// explicitly selects no cap. Live backings retain their shared budget when
// registration changes. Returns the previous registration for nested owners.
// Reinstall that handle with the overload below; an empty handle means unset.
CommandScratchByteCap set_command_scratch_byte_cap(std::uint64_t bytes);
CommandScratchByteCap set_command_scratch_byte_cap(CommandScratchByteCap registration) noexcept;

// Fresh, exclusive, initially empty backing. With an explicitly attached
// ScratchArena, borrow a region until destruction; the attachment must outlive
// it. No file is created or resized and the disk directory is not consulted.
// Arena mode supports buffered sync only; data/full return programming_error
// rather than promising durability the arena does not provide.
// Without an attachment, the disk directory comes from
// PINEFORGE_COMMAND_SCRATCH_DIR, defaulting to /tmp when unset or empty. There
// is no fallback when the selected directory fails. The unique file is unlinked
// before returning and its close-on-exec descriptor is closed on destruction.
// With no registration, PINEFORGE_COMMAND_SCRATCH_MAX_BYTES supplies a default
// shared across live backings created on this thread with that default. When
// set, it must be positive decimal uint64_t, even with an explicit registration;
// invalid values throw std::invalid_argument. Unset means no default cap.
// Extents consume the run budget until destruction; over-cap extensions return
// ScratchStatus::io_error before extending the backing in either mode.
// Zero page size and sizes beyond off_t throw std::invalid_argument.
// OS setup failures throw std::system_error.
std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size);

} // namespace pineforge::detail
