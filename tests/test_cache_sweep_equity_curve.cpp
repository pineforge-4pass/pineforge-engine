#include "tail_curve_cache_sweep.hpp"

int main() {
    try {
        cache_sweep::sweep("equity_curve", sizeof(pf_equity_point_t), 65536, 262144,
                           tail_curve_cache_sweep::equity_curve);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "equity curve acceptance: %s\n", error.what());
        return 1;
    }
}
