#pragma once

// Lab-only observation, using the existing logical-charge registration.
#include "../src/command_scratch_production.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace command_scratch_test {

struct CommandScratchExtentProbe {
    std::uint64_t high_water_bytes = 0;
    std::uint64_t successful_writes = 0;
    std::uint64_t failed_writes = 0;
    bool invalid_extent = false;

    void observe(const pineforge::detail::ScratchIoCharge& io) noexcept {
        using namespace pineforge::detail;
        if (io.operation != ScratchIoOperation::write_page) return;
        if (io.status != ScratchStatus::ok) {
            ++failed_writes;
            return;
        }
        ++successful_writes;
        if (io.page == scratch_no_page || io.bytes == 0
                || io.page + 1 > std::numeric_limits<std::uint64_t>::max() / io.bytes) {
            invalid_extent = true;
            return;
        }
        high_water_bytes = std::max(high_water_bytes, (io.page + 1) * io.bytes);
    }
};

// Construct BEFORE every run backing/host and destroy AFTER them. Factories
// snapshot the observer, so registering after construction misses that store.
// With no failed writes, high_water_bytes is the largest logical extent of
// ANY observed fresh backing, including holes, undo pages and transient peaks.
// The interface has no backing identity: this is a maximum, not a sum or a
// per-store attribution. Any failed write makes the extent evidence incomplete
// (the file may have extended before returning io_error). This probe measures
// logical extent only; real syscall wrappers must separately measure traffic.
class ScopedCommandScratchExtentProbe {
public:
    ScopedCommandScratchExtentProbe() noexcept
        : previous_(pineforge::detail::set_command_scratch_io_observer({this, &charge})) {}
    ~ScopedCommandScratchExtentProbe() {
        pineforge::detail::set_command_scratch_io_observer(previous_);
    }
    ScopedCommandScratchExtentProbe(const ScopedCommandScratchExtentProbe&) = delete;
    ScopedCommandScratchExtentProbe& operator=(const ScopedCommandScratchExtentProbe&) = delete;

    CommandScratchExtentProbe extent;

private:
    static void charge(void* context, const pineforge::detail::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<ScopedCommandScratchExtentProbe*>(context);
        self.extent.observe(io);
        if (self.previous_.charge) self.previous_.charge(self.previous_.context, io);
    }
    pineforge::detail::ScratchIoObserver previous_;
};

} // namespace command_scratch_test
