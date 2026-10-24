// F2: a reached POOC replacement fills at the calculation's None phase
// after a High fill, then sorts onto that High slot after a production seal.
// Reverting only the sort write guard must leave a stale High and split hashes.
#include "sealed_exit_phase_fixture.hpp"

int main() {
    try {
        sealed_exit_phase_fixture::exercise(sealed_exit_phase_fixture::Shape::SameBarNone);
        std::puts("PASS test_same_bar_sort_none_phase_after_seal");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_same_bar_sort_none_phase_after_seal: %s\n", error.what());
        return 1;
    }
}
