#include "native_tail_window_fixture.hpp"

int main() {
    try {
        using namespace native_tail_window_fixture;
        const auto reference = run({});
        const std::array<no::TailWindowConfig, 6> configurations{{
            {1, 64, 16}, {64, 1, 16}, {64, 64, 1},
            {0, 0, 0}, {1, 1, 1}, {2, 2, 2},
        }};
        for (const auto windows : configurations)
            require(equal(run(windows), reference),
                    "all complete native outputs are byte-identical at tiny and default tails");
        std::puts("native tail capacity sweep passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native tail capacity sweep: %s\n", error.what());
        return 1;
    }
}
