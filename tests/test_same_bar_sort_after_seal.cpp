// A-1 red-first: a real nonzero sealed base followed by High then Open
// bracket fills on one magnified chart bar. The unfixed host moves trades
// while reading/skipping phase slots with their global rather than local index.
#include "sealed_exit_phase_fixture.hpp"

int main() {
    try {
        sealed_exit_phase_fixture::exercise(sealed_exit_phase_fixture::Shape::SameBar);
        std::puts("PASS test_same_bar_sort_after_seal");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_same_bar_sort_after_seal: %s\n", error.what());
        return 1;
    }
}
