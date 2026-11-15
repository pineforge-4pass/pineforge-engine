#include <pineforge/series.hpp>
#define main pineforge_ta_fixture_main
#include "test_ta_extremes_ring.cpp"
#undef main

static int pineforge_ta_fixture_main(int, char**) {
    return pineforge_ta_fixture_main();
}

#include "ta_cache_fixture_sweep.hpp"
