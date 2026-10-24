#include "k1_dormant_retirement_fixture.hpp"

#include <exception>

int main() {
    try {
        for (int held : {2, 32, 256}) dormant_retirement::margin_equivalence(held);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_k1_dormant_reissue_margin: %s\n", error.what());
        return 1;
    }
}
