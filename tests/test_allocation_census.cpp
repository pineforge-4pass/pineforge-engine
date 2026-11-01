#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"
#include "acceptance_process_fixture.hpp"

#include <cstdint>

int main() {
    using namespace global_allocation;
    using acceptance_process::require;
    try {
        recording = true;
        void* first = ::operator new(257);
        void* second = ::operator new[](513, std::nothrow);
        void* aligned = ::operator new(1025, std::align_val_t{64});
        require(first && second && aligned, "allocation census control could not allocate");
        require(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0,
                "counting allocator lost over-alignment");
        require(live_bytes == 257 + 513 + 1025 && peak_bytes == live_bytes
            && live_blocks == 3 && peak_blocks == 3 && recorded_allocations == 3,
            "census missed an allocation form or its peak");
        require(system_live_bytes >= live_bytes + 3 * detail::kPrefix
            && system_peak_bytes == system_live_bytes, "allocator rounding/prefix charge was omitted");
        ::operator delete[](second);
        require(live_bytes == 257 + 1025 && peak_bytes == 257 + 513 + 1025,
                "released temporary bytes disappeared from the peak");
        recording = false;
        ::operator delete(first, std::size_t{257});
        ::operator delete(aligned, std::align_val_t{64});
        require(live_bytes == 0 && live_blocks == 0 && recorded_releases == 3 && system_live_bytes == 0,
                "census lost a release after the recording window");
        std::puts("allocation census controls PASS");
        return 0;
    } catch (const std::exception& error) {
        recording = false;
        std::fprintf(stderr, "allocation census controls FAIL: %s\n", error.what());
        return 1;
    }
}
