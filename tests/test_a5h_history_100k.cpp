#include "a5h_growth_fixture.hpp"
#include "a5h_prepared_growth_fixture.hpp"
#include "change_limit_fixture.hpp"
int main() {
    const int raw = history_growth_test::run(100000, true);
    if (raw == 2) return raw;
    const int prepared = history_growth_test::prepared_growth(100000);
    if (prepared == 2) return prepared;
    const int change = change_limit_test::growth(100000);
    return change == 2 ? change : (raw || prepared || change);
}
