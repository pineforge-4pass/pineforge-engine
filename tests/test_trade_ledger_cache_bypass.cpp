// This unit uses only the previous header's public API, so the allocation
// failure assertion can run against the unchanged library as a red control.
#include "p2cx_tledger_fixture.hpp"
#include "trade_ledger_cache_io_fixture.hpp"
#include "global_allocation_replacement.hpp"

#include <array>
#include <limits>
#include <utility>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
using trade_ledger_cache_io::Measure;
constexpr Size page_slots = TradeLedger::kCachePageSlots;
constexpr Size table_bytes = ((TradeLedger::kDefaultCacheRows + page_slots - 1)
                              / page_slots) * sizeof(void*);
constexpr Size page_bytes = page_slots * TradeLedger::kLedgerEntryBytes;

struct FailNextAllocation {
    FailNextAllocation() { global_allocation::fail_after = 0; }
    ~FailNextAllocation() {
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    }
};

void append(TradeLedger& ledger, Size population) {
    for (Size i = 0; i < population; ++i) ledger.push_back(row(static_cast<int>(i)));
}

void forwarding_control() {
    detail::ScratchFile file;
    file.set_directory(".");
    file.open("ledger-cache-counter");
    const std::array<char, 7> expected{{'b', 'a', 'c', 'k', '\0', 'i', 'o'}};
    std::array<char, 7> actual{};
    file.write(0, expected.data(), expected.size());
    Measure measure;
    file.read(0, actual.data(), actual.size());
    const auto io = measure.finish();
    REQUIRE(actual == expected);
    REQUIRE(io.calls == 1 && io.bytes == expected.size() && io.failures == 0);
}

void first_cache_allocation_failure_reads_through() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    constexpr Size population = 4 * TradeLedger::kResidentRows + 1;
    append(ledger, population);
    REQUIRE(ledger.resident_base() > 0);
    const auto fixed = ledger.cache_resident_bytes();
    const auto expected = row(0);
    const auto refused = global_allocation::refused;
    bool allocation_escaped = false;
    double price = 0;
    Measure measure;
    {
        FailNextAllocation fail;
        try { price = ledger.entry_price_at(0); }
        catch (const std::bad_alloc&) { allocation_escaped = true; }
    }
    const auto io = measure.finish();
    CHECK(!allocation_escaped && "optional ledger cache allocation must bypass");
    REQUIRE(global_allocation::refused == refused + 1);
    if (allocation_escaped) return; // The assertion above is the red verdict.
    CHECK(same_bits(price, expected.entry_price));
    CHECK(io.calls >= 1 && io.bytes == TradeLedger::kRecordBytes && io.failures == 0);
    CHECK(ledger.cache_size() == 0 && ledger.cache_resident_bytes() == fixed);
    CHECK(ledger.cache_allocation_failed());
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
    CHECK(ledger.read_cache_stats().bypass_reason
          == TradeLedger::CacheBypassReason::allocation_failed);

    // Failure latches bypass until reset: numeric reads cannot retry a
    // population-sized cache allocation on every field access.
    const auto allocations = global_allocation::allocations;
    for (Size i = 0; i < population; ++i)
        CHECK(same_bits(ledger.entry_price_at(i), 100.0 + static_cast<double>(i)));
    CHECK(global_allocation::allocations == allocations);
    for (Size i = 0; i < population; ++i) {
        const auto wanted = row(static_cast<int>(i));
        same_trade(ledger.read(i), wanted);
        same_numeric_fields(ledger, i, wanted);
        same_string_fields(ledger, i, wanted);
    }
    CHECK(ledger.cache_size() == 0 && ledger.cache_resident_bytes() == fixed);
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);

    // The fixed fallback entry cannot escape through an owning Trade.
    const Trade retained = ledger.read(0);
    ledger.replace(0, row(900));
    same_trade(retained, expected);
    same_trade(ledger.read(0), row(900));
    ledger.clear();
    ledger.push_back(expected);
    same_trade(ledger.read(0), expected);
    CHECK(ledger.cache_size() == 1);
    CHECK(ledger.cache_resident_bytes() == fixed
          + table_bytes + page_bytes);
    CHECK(!ledger.cache_allocation_failed());
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 0);
}

void growth_allocation_failure_keeps_the_append() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    append(ledger, page_slots);
    const auto fixed = ledger.cache_resident_bytes();
    const auto first = row(0);
    same_trade(ledger.read(0), first);
    REQUIRE(ledger.cache_size() == 1);
    const auto one_page = fixed + table_bytes + page_bytes;
    REQUIRE(ledger.cache_resident_bytes() == one_page);
    const auto next = row(static_cast<int>(page_slots));
    const auto refused = global_allocation::refused;
    bool allocation_escaped = false;
    {
        FailNextAllocation fail;
        try { ledger.push_back(next); }
        catch (const std::bad_alloc&) { allocation_escaped = true; }
    }
    CHECK(!allocation_escaped && "optional ledger cache growth must keep the append");
    REQUIRE(global_allocation::refused == refused + 1);
    if (allocation_escaped) return;
    REQUIRE(ledger.size() == page_slots + 1);
    CHECK(ledger.cache_size() == 1 && ledger.cache_resident_bytes() == one_page);
    CHECK(ledger.cache_allocation_failed());
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
    CHECK(ledger.read_cache_stats().bypass_reason
          == TradeLedger::CacheBypassReason::allocation_failed);
    same_trade(ledger.read(0), first);
    same_trade(ledger.back(), next);
    ledger.resize(1);
    ledger.push_back(row(901));
    same_trade(ledger.read(1), row(901));
    CHECK(ledger.cache_size() == 0 && ledger.cache_resident_bytes() == one_page);
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
}

void zero_capacity_never_allocates_a_cache() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(0); // Red on the previous tree: setting_rejected.
    REQUIRE(ledger.cache_rows() == 0);
    const auto fixed = TradeLedger::resident_cache_bytes(0);
    constexpr Size population = 4 * TradeLedger::kResidentRows + 1;
    append(ledger, population);
    const auto allocations = global_allocation::allocations;
    Measure measure;
    {
        FailNextAllocation fail;
        for (Size i = 0; i < population; ++i)
            CHECK(same_bits(ledger.entry_price_at(i), 100.0 + static_cast<double>(i)));
        CHECK(global_allocation::fail_after == 0); // No allocation consumed the fault.
    }
    const auto io = measure.finish();
    const auto cold_rows = population - TradeLedger::kResidentRows;
    CHECK(io.calls >= cold_rows);
    CHECK(io.bytes == cold_rows * TradeLedger::kRecordBytes && io.failures == 0);
    CHECK(global_allocation::allocations == allocations);
    for (Size i = 0; i < population; ++i) {
        const auto expected = row(static_cast<int>(i));
        same_trade(ledger.read(i), expected);
        same_numeric_fields(ledger, i, expected);
        same_string_fields(ledger, i, expected);
    }
    CHECK(ledger.cache_size() == 0 && ledger.cache_resident_bytes() == fixed);
    CHECK(!ledger.cache_allocation_failed());
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 0);
    CHECK(ledger.read_cache_stats().bypass_reason == TradeLedger::CacheBypassReason::none);
    TradeLedger copy(ledger);
    CHECK(copy.cache_rows() == 0 && copy.cache_size() == 0);
    same_trade(copy.read(0), row(0));
    TradeLedger moved(std::move(copy));
    CHECK(moved.cache_rows() == 0 && moved.cache_size() == 0);
    same_trade(moved.read(population - 1), row(static_cast<int>(population - 1)));
    ledger.clear();
    CHECK(ledger.cache_rows() == 0 && ledger.cache_resident_bytes() == fixed);
}

void backing_failure_is_not_an_allocation_bypass() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    append(ledger, 4 * TradeLedger::kResidentRows + 1);
    const auto expected = row(0);
    const auto refused = global_allocation::refused;
    {
        FailNextAllocation fail;
        CHECK(same_bits(ledger.entry_price_at(0), expected.entry_price));
    }
    REQUIRE(global_allocation::refused == refused + 1);
    for (const int fault : {EIO, ENOMEM}) {
        bool caught = false;
        Measure measure;
        trade_ledger_cache_io::next_error = fault;
        try { (void)ledger.entry_price_at(1); }
        catch (const detail::ScratchIoError& error) {
            caught = true;
            CHECK(error.code().value() == fault);
            const auto failure = classify_run_failure(error);
            CHECK(failure.code == RunFailureCode::report_sink_failed);
            REQUIRE(failure.args);
            CHECK(*failure.args == "{\"reason\":\"scratch_io\"}");
        }
        const auto io = measure.finish();
        REQUIRE(caught);
        CHECK(io.calls == 1 && io.bytes == 0 && io.failures == 1);
    }
    same_trade(ledger.read(1), row(1)); // The original backing remains readable.
    CHECK(ledger.cache_size() == 0);
}

void owning_value_allocation_failure_still_fails() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    const auto expected = row(0);
    ledger.push_back(expected);
    same_trade(ledger.read(0), expected); // Warm the cache before the fault.
    const auto refused = global_allocation::refused;
    bool caught = false;
    Measure measure;
    {
        FailNextAllocation fail;
        try { (void)ledger.read(0); } // Owning string output requires allocation.
        catch (const std::bad_alloc&) { caught = true; }
    }
    const auto io = measure.finish();
    REQUIRE(caught && global_allocation::refused == refused + 1);
    CHECK(io.calls == 0);
    CHECK(ledger.cache_size() == 1);
    same_trade(ledger.read(0), expected);
}

void failed_shared_fill_preserves_the_typed_error_and_retry() {
    for (const int fault : {EIO, ENOMEM}) {
        TradeLedger ledger;
        ledger.set_cache_rows(1);
        ledger.set_scratch_directory(".");
        append(ledger, 4 * TradeLedger::kResidentRows + 1);
        same_trade(ledger.read(0), row(0));
        REQUIRE(ledger.cache_size() == 1);
        bool caught = false;
        Measure measure;
        trade_ledger_cache_io::next_error = fault;
        try { (void)ledger.entry_price_at(1); }
        catch (const detail::ScratchIoError& error) {
            caught = true;
            CHECK(error.code().value() == fault);
            const auto failure = classify_run_failure(error);
            CHECK(failure.code == RunFailureCode::report_sink_failed);
            REQUIRE(failure.args);
            CHECK(*failure.args == "{\"reason\":\"scratch_io\"}");
        }
        const auto failed = measure.finish();
        REQUIRE(caught);
        CHECK(failed.calls == 1 && failed.bytes == 0 && failed.failures == 1);
        // A failed replacement fill cannot publish bytes under the new key.
        same_trade(ledger.read(1), row(1));
        CHECK(ledger.cache_size() == 1);
        Measure warm_measure;
        same_trade(ledger.read(1), row(1));
        const auto warm = warm_measure.finish();
        CHECK(warm.calls == 0 && warm.bytes == 0);
        same_trade(ledger.read(0), row(0));
        CHECK(ledger.cache_size() == 1);
    }
}
} // namespace

int main() {
    test("backing counter forwards exact bytes", forwarding_control);
    test("first decoded cache allocation failure reads through",
         first_cache_allocation_failure_reads_through);
    test("decoded cache growth failure keeps the append",
         growth_allocation_failure_keeps_the_append);
    test("zero decoded cache capacity reads through", zero_capacity_never_allocates_a_cache);
    test("backing I/O failures survive allocation bypass",
         backing_failure_is_not_an_allocation_bypass);
    test("owning value allocation failure is preserved",
         owning_value_allocation_failure_still_fails);
    test("failed shared fill preserves typed backing error and healthy retry",
         failed_shared_fill_preserves_the_typed_error_and_retry);
    std::printf("test_trade_ledger_cache_bypass: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
