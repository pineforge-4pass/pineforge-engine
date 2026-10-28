#include "ledger_cyclic_scan_fixture.hpp"

int main() {
    test("real read/map/unmap forwarding controls", forwarding_controls);
    test("262144 rows, eight complete numeric passes", [] { cyclic_scan(262144); });
    std::printf("test_ledger_cyclic_scan_large: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
