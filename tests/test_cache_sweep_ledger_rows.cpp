#include "cache_sweep_ledger_adapter.hpp"
int main() {
    try {
        cache_sweep::sweep("ledger_rows", 1, 65536, 131072, cache_sweep::ledger_rows);
        return 0;
    } catch (const std::exception& error) {
        cache_sweep_ledger_io::measuring = cache_sweep_ledger_io::inject_fault = false;
        std::fprintf(stderr, "ledger row sweep FAIL: %s\n", error.what());
        return 1;
    }
}
