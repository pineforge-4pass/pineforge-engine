// Behavioral RED control: this entire unit uses the 12148f96 public API.
// Compile/link it against that tree before applying the repair. A missing
// new declaration is not its RED. All executions are UNRUN in the author lane.
#include "p2cx_tledger_fixture.hpp"
#include "trade_ledger_cache_io_fixture.hpp"
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"
#include <pineforge/cache_policy.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <utility>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void retire_history(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
}

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
using trade_ledger_cache_io::Measure;
std::array<Size, 5> allocation_requests{};
Size allocation_request_count = 0;
void record_allocation(Size bytes) {
    if (allocation_request_count < allocation_requests.size())
        allocation_requests[allocation_request_count] = bytes;
    ++allocation_request_count;
}

void append_to(TradeLedger& ledger, Size end) {
    while (ledger.size() < end) ledger.push_back(row(static_cast<int>(ledger.size())));
}

void warm_value(const TradeLedger& ledger, Size index, const Trade& expected) {
    Measure measure;
    same_trade(ledger.read(index), expected);
    same_numeric_fields(ledger, index, expected);
    same_string_fields(ledger, index, expected);
    CHECK(ledger.entry_id_equals(index, expected.entry_id));
    CHECK(ledger.entry_comment_equals(index, expected.entry_comment));
    CHECK(ledger.exit_comment_equals(index, expected.exit_comment));
    CHECK(ledger.exit_id_equals(index, expected.exit_id));
    const auto work = measure.finish();
    CHECK(work.calls == 0 && work.bytes == 0 && work.failures == 0
          && "still-valid occupied rows must survive a geometry change warm");
}

void backing_counter_control() {
    TradeLedger ledger(0);
    ledger.set_scratch_directory(".");
    append_to(ledger, 4 * TradeLedger::kResidentRows);
    Measure measure;
    CHECK(same_bits(ledger.entry_price_at(0), row(0).entry_price));
    const auto work = measure.finish();
    CHECK(work.calls == 1 && work.bytes == TradeLedger::kRecordBytes);
    Measure payload;
    CHECK(ledger.entry_comment_equals(0, row(0).entry_comment));
    const auto text_work = payload.finish();
    CHECK(text_work.calls > 1 && text_work.bytes > TradeLedger::kRecordBytes);
}

void every_growth_boundary() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    append_to(ledger, TradeLedger::kDefaultCacheRows);
    // Warm every entry with independently constructed values, including NULs
    // and signed zero. This also checks scalar and full-value sharing.
    for (Size i = 0; i < ledger.size(); ++i)
        same_trade(ledger.read(i), row(static_cast<int>(i)));
    REQUIRE(ledger.cache_size() == ledger.size());
    // Includes the old finite setter maximum: AUTO must pass it. Each loop
    // doubles the geometry actually observed at the preceding boundary.
    for (Size boundary = TradeLedger::kDefaultCacheRows;
         boundary <= TradeLedger::kMaxCacheRows; boundary *= 2) {
        append_to(ledger, boundary);
        const auto occupied = ledger.cache_size();
        ledger.push_back(row(static_cast<int>(boundary)));
        // Predicted first 12148 behavioral RED, before any new API is used.
        CHECK(ledger.cache_rows() == boundary * 2
              && "default ledger must grow at the actual retained population");
        CHECK(ledger.cache_size() == occupied + 1);
        for (Size i = 0; i < boundary; ++i)
            warm_value(ledger, i, row(static_cast<int>(i)));
        warm_value(ledger, boundary, row(static_cast<int>(boundary)));
        const auto directory = ledger.cache_rows() / TradeLedger::kCachePageSlots
            * sizeof(void*);
        const auto resident_pages = (ledger.size() + TradeLedger::kCachePageSlots - 1)
            / TradeLedger::kCachePageSlots;
        CHECK(ledger.cache_resident_bytes() == TradeLedger::resident_cache_bytes(0)
            + directory + resident_pages * TradeLedger::kCachePageSlots
                * TradeLedger::kLedgerEntryBytes);
    }
}

void cold_population_and_explicit_controls() {
    const auto population = TradeLedger::kDefaultCacheRows + 3;
    for (const int mode : {0, 1, 2, 3}) {
        TradeLedger ledger;
        if (mode == 1) ledger.set_cache_rows(TradeLedger::kDefaultCacheRows);
        if (mode == 2) ledger.set_cache_capacity(
            CacheCapacity{TradeLedger::cache_capacity_bytes(TradeLedger::kDefaultCacheRows)});
        if (mode == 3) ledger.set_cache_rows(0);
        ledger.set_scratch_directory(".");
        append_to(ledger, population);
        CHECK(ledger.cache_size() == 0); // Write-only AUTO stays lazy.
        // Give every descriptor an external address, even the newest rows.
        ledger.resize(population - 1);
        ledger.push_back(row(static_cast<int>(population - 1)));
        Measure cold;
        same_trade(ledger.read(0), row(0));
        const auto first = cold.finish();
        CHECK(first.calls > 1 && first.bytes > TradeLedger::kRecordBytes);
        for (Size i = 1; i < population; ++i)
            same_trade(ledger.read(i), row(static_cast<int>(i)));
        Measure reread;
        same_trade(ledger.read(0), row(0));
        const auto work = reread.finish();
        if (mode == 0) {
            CHECK(ledger.cache_rows() == 2 * TradeLedger::kDefaultCacheRows);
            CHECK(work.calls == 0 && work.bytes == 0);
        } else {
            CHECK(ledger.cache_rows() == (mode == 3 ? 0 : TradeLedger::kDefaultCacheRows));
            CHECK(work.calls > 0 && work.bytes > 0);
        }
    }
}

void growth_failure_is_atomic_and_retryable() {
    // Table, first copied page, later copied page, then the new row's page
    // after a successful reindex publication.
    for (const Size fail_after : {Size{0}, Size{1}, Size{2}, Size{3}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        const auto boundary = TradeLedger::kDefaultCacheRows;
        append_to(ledger, boundary);
        same_trade(ledger.read(0), row(0));
        same_trade(ledger.read(boundary / 2 + 7), row(static_cast<int>(boundary / 2 + 7)));
        const auto resident = ledger.cache_resident_bytes();
        const auto occupied = ledger.cache_size();
        const auto refused = global_allocation::refused;
        const auto next = row(static_cast<int>(boundary));
        bool escaped = false;
        global_allocation::fail_after = fail_after;
        try { ledger.push_back(next); }
        catch (const std::bad_alloc&) { escaped = true; }
        global_allocation::fail_after = std::numeric_limits<Size>::max();
        CHECK(!escaped && "optional reindex failure must keep the backing append");
        REQUIRE(global_allocation::refused == refused + 1);
        REQUIRE(ledger.size() == boundary + 1);
        const bool published = fail_after == 3;
        CHECK(ledger.cache_rows() == (published ? 2 * boundary : boundary));
        CHECK(ledger.cache_size() == occupied);
        const auto extra_directory = published
            ? boundary / TradeLedger::kCachePageSlots * sizeof(void*) : 0;
        CHECK(ledger.cache_resident_bytes() == resident + extra_directory);
        CHECK(ledger.cache_allocation_failed());
        CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
        warm_value(ledger, 0, row(0));
        warm_value(ledger, boundary / 2 + 7, row(static_cast<int>(boundary / 2 + 7)));
        same_trade(ledger.read(boundary), next);
        // A scratch error after optional bypass keeps its typed first fault;
        // a cache failure must not turn it into success or a bad_alloc.
        Measure fault;
        trade_ledger_cache_io::next_error = EIO;
        bool surfaced = false;
        try { (void)ledger.pnl_at(1); }
        catch (const detail::ScratchIoError& error) {
            surfaced = true;
            CHECK(error.code().value() == EIO);
            const auto typed = classify_run_failure(error);
            CHECK(typed.code == RunFailureCode::report_sink_failed);
            REQUIRE(typed.args);
            CHECK(*typed.args == "{\"reason\":\"scratch_io\"}");
        }
        const auto failed = fault.finish();
        REQUIRE(surfaced && failed.calls == 1 && failed.failures == 1);
        // A changed population retries genuine allocation, rather than
        // converting a transient failure into an undocumented finite policy.
        ledger.push_back(row(static_cast<int>(boundary + 1)));
        CHECK(ledger.cache_rows() == 2 * boundary);
        CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
        warm_value(ledger, 0, row(0));
        warm_value(ledger, boundary / 2 + 7, row(static_cast<int>(boundary / 2 + 7)));
        same_trade(ledger.read(1), row(1));
    }
}

void growth_peak_counts_both_geometries() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    const auto boundary = TradeLedger::kDefaultCacheRows;
    append_to(ledger, boundary);
    const auto next = row(static_cast<int>(boundary));
    // One census window owns both old and replacement allocations, so its
    // live/peak counters measure the actual overlap rather than summing
    // peaks from disjoint windows. Scalar warming creates no owning Trade.
    global_allocation::live_bytes = global_allocation::peak_bytes = 0;
    global_allocation::on_recorded = record_allocation;
    global_allocation::recording = true;
    const auto price = ledger.entry_price_at(0);
    const auto before = global_allocation::live_bytes;
    ledger.push_back(next);
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    const auto live = global_allocation::live_bytes;
    const auto peak = global_allocation::peak_bytes;
    const auto new_directory = 2 * boundary / TradeLedger::kCachePageSlots * sizeof(void*);
    const auto old_directory = new_directory / 2;
    const auto page_bytes = TradeLedger::kCachePageSlots * TradeLedger::kLedgerEntryBytes;
    CHECK(same_bits(price, row(0).entry_price));
    REQUIRE(allocation_request_count == allocation_requests.size());
    CHECK(allocation_requests[0] >= old_directory && allocation_requests[1] == page_bytes);
    CHECK(allocation_requests[2] >= new_directory
          && allocation_requests[3] == page_bytes && allocation_requests[4] == page_bytes);
    CHECK(before == allocation_requests[0] + page_bytes);
    CHECK(peak == before + allocation_requests[2] + page_bytes);
    CHECK(peak > live
          && "private warm reindex must account for simultaneous old and new storage");
    const auto array_cookie = allocation_requests[2] - new_directory;
    CHECK(live - array_cookie == ledger.cache_resident_bytes()
          - TradeLedger::resident_cache_bytes(0));
    std::printf("LEDGER_AUTO_MEMORY rows=%zu resident_optional=%zu heap_peak=%zu "
                "heap_live=%zu directory=%zu table_cookie=%zu\n",
        ledger.cache_rows(), ledger.cache_resident_bytes() - TradeLedger::resident_cache_bytes(0),
        peak, live, new_directory, array_cookie);
    warm_value(ledger, 0, row(0));
}

void lifecycle_intent_and_values() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    append_to(ledger, TradeLedger::kDefaultCacheRows + 1);
    same_trade(ledger.read(0), row(0));
    TradeLedger copy(ledger), assigned(0);
    assigned = ledger;
    TradeLedger moved(std::move(copy));
    TradeLedger move_assigned(1);
    move_assigned = std::move(assigned);
    for (auto* owner : {&ledger, &moved, &move_assigned}) {
        const auto replacement = row(700);
        owner->replace(0, replacement);
        same_trade(owner->read(0), replacement);
        owner->resize(1);
        owner->push_back(row(701));
        same_trade(owner->read(1), row(701));
        owner->clear();
        CHECK(owner->cache_size() == 0);
        append_to(*owner, TradeLedger::kDefaultCacheRows + 1);
        same_trade(owner->read(0), row(0));
        CHECK(owner->cache_rows() == 2 * TradeLedger::kDefaultCacheRows);
    }
    for (auto* emptied : {&copy, &assigned}) {
        CHECK(emptied->empty());
        emptied->push_back(row(3));
        same_trade(emptied->read(0), row(3));
    }
}

void copied_failure_provenance() {
    for (const bool finite : {false, true}) {
        TradeLedger original;
        if (finite) original.set_cache_rows(TradeLedger::kDefaultCacheRows);
        original.set_scratch_directory(".");
        append_to(original, 4 * TradeLedger::kResidentRows);
        global_allocation::fail_after = 0;
        const auto value = original.pnl_at(0);
        global_allocation::fail_after = std::numeric_limits<Size>::max();
        CHECK(same_bits(value, row(0).pnl));
        REQUIRE(original.cache_allocation_failed());
        TradeLedger copied(original);
        TradeLedger moved(std::move(copied));
        CHECK(moved.cache_allocation_failed());
        CHECK(moved.read_cache_stats().allocation_bypass_count == 1);
        same_trade(moved.read(0), row(0));
        moved.resize(1);
        moved.push_back(row(9));
        same_trade(moved.read(1), row(9));
        CHECK(moved.read_cache_stats().allocation_bypass_count == 1);
        moved.clear();
        CHECK(!moved.cache_allocation_failed());
        CHECK(moved.read_cache_stats().allocation_bypass_count == 0);
    }
}

void retirement_uses_retained_population() {
    TradeLedger ledger;
    detail::TradeLedgerStorageProbe::retire_history(ledger);
    ledger.set_scratch_directory(".");
    const auto block = TradeLedger::kResidentRows * 2;
    // The global index crosses the initial geometry, but only a small
    // suffix is retained. AUTO must not size from the historical high water.
    for (Size i = 0; i < TradeLedger::kDefaultCacheRows / block + 2; ++i) {
        append_to(ledger, ledger.size() + block);
        same_trade(ledger.back(), row(static_cast<int>(ledger.size() - 1)));
        ledger.seal(ledger.size() - 1);
    }
    CHECK(ledger.size() > TradeLedger::kDefaultCacheRows);
    CHECK(ledger.cache_rows() == TradeLedger::kDefaultCacheRows);
    same_trade(ledger.back(), row(static_cast<int>(ledger.size() - 1)));
    bool retired = false;
    try { (void)ledger.read(0); }
    catch (const std::logic_error&) { retired = true; }
    CHECK(retired);
}
}

int main() {
    test("real descriptor and payload backing counter", backing_counter_control);
    test("AUTO warm values at every doubling through the old ceiling", every_growth_boundary);
    test("cold AUTO versus explicitly finite old default and zero", cold_population_and_explicit_controls);
    test("private reindex allocation failure, first fault and retry", growth_failure_is_atomic_and_retryable);
    test("private reindex resident and peak allocation costs", growth_peak_counts_both_geometries);
    test("copy move clear rewind replacement preserve AUTO intent", lifecycle_intent_and_values);
    test("copy move rewind preserve first bypass provenance", copied_failure_provenance);
    test("retirement sizes from retained population", retirement_uses_retained_population);
    std::printf("test_ledger_auto_cache_growth: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
