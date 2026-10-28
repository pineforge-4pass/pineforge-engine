#include "ledger_cyclic_scan_fixture.hpp"

int main() {
    test("real read/map/unmap forwarding controls", forwarding_controls);
    test("85536 rows, eight complete numeric passes", [] { cyclic_scan(85536); });
    test("90000 rows, eight complete numeric passes", [] { cyclic_scan(90000); });
    std::printf("test_ledger_cyclic_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
