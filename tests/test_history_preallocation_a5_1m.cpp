#define PINEFORGE_A5_HISTORY_BARS 1000000U
#include "history_preallocation_a5_fixture.hpp"

int main() {
    return a5_history_fixture::run(PINEFORGE_A5_HISTORY_BARS);
}
