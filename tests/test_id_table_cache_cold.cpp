// This unit deliberately uses only the frozen header API. Disabled-cache
// cold reads are already supported; the baseline is expected GREEN, not red.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "id_table_cache_workload.hpp"

namespace {
using namespace id_table_cache_test;
void disabled_cold_and_backing_error() {
    Tree tree(0);
    tree.set_directory(".");
    const auto at = tree.insert("valid-cold-id", 137).first;
    sc412::Measure reads;
    const auto allocations = global_allocation::allocations;
    const auto found = tree.find("valid-cold-id");
    require(found == at && tree.get(*found) == 137, "disabled cache never refuses a cold valid key");
    const auto io = reads.finish()[sc412::ids];
    require(io.reads > 0 && io.read_requests == io.read_bytes && io.past_end == 0,
            "disabled cold key is proven by successful physical reads");
    require(global_allocation::allocations == allocations,
            "disabled short-key read allocates no optional cache");
    // Poison the backing after a successful read: bypass must not hide a real
    // I/O failure as a hit, an absent key or a cache-capacity refusal.
    require(::ftruncate(descriptor(tree), 0) == 0, "inject genuine backing EOF");
    bool failed = false;
    try { (void)tree.get(at); }
    catch (const AuxiliaryScratchIoError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        failed = error.code().value() == EIO
            && failure.code == pineforge::RunFailureCode::scratch_io_failed && !failure.args;
    }
    require(failed, "disabled cache preserves backing EIO and scratch_io_failed");
}
}

int main() {
    try {
        disabled_cold_and_backing_error();
        const auto direct = id_table_cache_test::workload(id_table_cache_test::ByteCapacity{0, 0});
        id_table_cache_test::identical(direct, id_table_cache_test::workload(
            id_table_cache_test::DefaultCapacity{}));
        id_table_cache_test::identical(direct, id_table_cache_test::workload(
            id_table_cache_test::ByteCapacity{4096, 4096}));
        id_table_cache_test::identical(direct, id_table_cache_test::workload(
            id_table_cache_test::ByteCapacity{64 * 1024, 32 * 1024}));
        std::puts("id table cache cold/default/one-line/adapter sweep: OK");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
