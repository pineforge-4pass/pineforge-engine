// A-1 red-first: A's two-unit global child cuts the oldest Z lot, then Z's
// child cuts the remainder and A. After sealing, the third row loses its
// High phase on the unfixed host's global-index read/write guards.
#include "sealed_exit_phase_fixture.hpp"

int main() {
    try {
        sealed_exit_phase_fixture::exercise(sealed_exit_phase_fixture::Shape::GlobalRecut);
        std::puts("PASS test_global_exit_recut_after_seal");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_global_exit_recut_after_seal: %s\n", error.what());
        return 1;
    }
}
