#define PINEFORGE_TAIL_CACHE_ACCEPTANCE
#include "native_tail_window_fixture.hpp"

int main() {
    try {
        native_tail_window_fixture::acceptance();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native tail acceptance: %s\n", error.what());
        return 1;
    }
}
