#include "trace_profile_fixture.hpp"

int main() {
    try {
        default_and_debug_bytes();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
