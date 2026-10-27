#include "sha256_fixture.hpp"

int main() {
    sha256_fixture::check_differential(sha256_fixture::check_selection());
    return EXIT_SUCCESS;
}
