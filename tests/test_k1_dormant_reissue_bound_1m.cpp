#include "k1_dormant_retirement_fixture.hpp"

#include <exception>

int main() {
    try {
        for (int count : {100000, 1000000}) dormant_retirement::growth(count);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_k1_dormant_reissue_bound_1m: %s\n", error.what());
        return 1;
    }
}
