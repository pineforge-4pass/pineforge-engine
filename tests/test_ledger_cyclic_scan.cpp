// Sealed numeric reads preserve values below and across the publication base.
#include "p2cx_tledger_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
constexpr Size kBlock = TradeLedger::kResidentRows;
// Fit whole descriptor blocks in one scratch transfer: floor(65536/168/64)*64
// = 384 rows. The reader admits requested rows without numeric read-ahead.
constexpr Size kSpan =
    detail::ScratchFile::kCacheBytes / TradeLedger::kRecordBytes / kBlock * kBlock;

void reverse_numeric_values_below_base() {
    constexpr Size population = 2 * kSpan + kBlock;
    constexpr Size published = kSpan + kBlock / 2;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    for (Size index = 0; index < population; ++index) {
        expected.push_back(row(static_cast<int>(index)));
        ledger.push_back(expected.back());
    }
    ledger.seal(published);
    REQUIRE(ledger.base() == published);
    REQUIRE(ledger.cache_size() == 0);
    // Read a whole descriptor block plus two predecessors below base().
    // MUTANT: retaining the unsealed-only field check refuses the first read.
    constexpr Size reads = 2 + kBlock;
    for (Size offset = 0; offset < reads; ++offset) {
        const Size index = kSpan - 1 - offset;
        same_numeric_fields(ledger, index, expected[index]);
        CHECK(ledger.cache_size() == offset + 1);
    }
}

void descending_numeric_values_cross_base() {
    constexpr Size population = 2 * kSpan + kBlock;
    constexpr Size published = kSpan + kBlock / 2;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    for (Size index = 0; index < population; ++index) {
        expected.push_back(row(static_cast<int>(index)));
        ledger.push_back(expected.back());
    }
    ledger.seal(published);
    REQUIRE(ledger.base() == published);
    REQUIRE(ledger.cache_size() == 0);
    REQUIRE(population <= ledger.cache_rows());
    for (Size cursor = population; cursor != 0; --cursor) {
        const Size index = cursor - 1;
        same_numeric_fields(ledger, index, expected[index]);
        // All keys fit distinct slots. One admission per visited row makes
        // occupancy exactly population-index, including the sealed prefix.
        // MUTANT: unsealed-only reads refuse at published-1; skipping cache
        // admission or reading extra sealed keys breaks the exact count.
        CHECK(ledger.cache_size() == population - index);
        if (index == published)
            CHECK(ledger.cache_size() == population - published);
        if (index == published - 1)
            CHECK(ledger.cache_size() == population - published + 1);
    }
    CHECK(ledger.cache_size() == population);
}
} // namespace

int main() {
    test("reverse numeric values below base", reverse_numeric_values_below_base);
    test("descending numeric values cross base", descending_numeric_values_cross_base);
    std::printf("test_ledger_cyclic_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
