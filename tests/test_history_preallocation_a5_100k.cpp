#define PINEFORGE_A5_HISTORY_BARS 100000U
#include "history_preallocation_a5_fixture.hpp"

int main() {
    return a5_history_fixture::run(PINEFORGE_A5_HISTORY_BARS);
}
