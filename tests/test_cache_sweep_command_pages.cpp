#define PINEFORGE_CACHE_SWEEP_COMMAND_ONLY
#include "cache_sweep_page_adapters.hpp"
int main() {
    try {
        cache_sweep::sweep("command_pages", 512, 65536, 262144, cache_sweep::command_pages);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "command page sweep FAIL: %s\n", error.what());
        return 1;
    }
}
