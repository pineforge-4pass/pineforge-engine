#include "trace_profile_fixture.hpp"

int main() {
    try {
        trace_after_admission();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
