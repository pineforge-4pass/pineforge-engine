#include "tail_curve_cache_sweep.hpp"

int main() {
    try {
        cache_sweep::sweep("position_tail", 4096, 65536, 262144,
                           tail_curve_cache_sweep::position_tail);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "position tail acceptance: %s\n", error.what());
        return 1;
    }
}
