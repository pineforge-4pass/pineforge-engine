#define PINEFORGE_CACHE_SWEEP_ID_ONLY
#include "cache_sweep_page_adapters.hpp"
int main() {
    try {
        cache_sweep::sweep("id_pages", 4096, 0, 262144, cache_sweep::id_pages);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "id page sweep FAIL: %s\n", error.what());
        return 1;
    }
}
