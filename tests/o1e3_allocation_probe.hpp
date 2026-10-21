#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>

namespace o1e3_test {
// Actual simultaneously live allocations, including temporary sort buffers,
// not merely the largest allocation or a container's element-size estimate.
struct AllocationProbe {
    std::size_t baseline = global_allocation::live_bytes;
    std::size_t peak = 0;
    inline static AllocationProbe* active = nullptr;
    static void observe(std::size_t) {
        if (global_allocation::live_bytes >= active->baseline)
            active->peak = std::max(active->peak, global_allocation::live_bytes - active->baseline);
    }
    AllocationProbe() {
        active = this;
        global_allocation::on_recorded = observe;
        global_allocation::recording = true;
    }
    ~AllocationProbe() { stop(); }
    std::size_t stop() {
        if (active == this) {
            global_allocation::recording = false;
            global_allocation::on_recorded = nullptr;
            active = nullptr;
        }
        return peak;
    }
};
}
