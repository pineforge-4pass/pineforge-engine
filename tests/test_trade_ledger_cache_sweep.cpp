#include "cache_sweep_ledger_adapter.hpp"
#include "../src/run_cache_diagnostics.hpp"

namespace {
cache_sweep::Result private_ledger_rows(cache_sweep::Setting setting) {
    // Reuse the acceptance workload, complete-output replay and inner pread
    // interposer. Only allocated private pages and their directory are charged.
    auto result = cache_sweep::ledger_rows(setting);
    const auto& measured = result.cache;
    using Ledger = pineforge::TradeLedger;
    const auto rows = setting.defaults ? Ledger::kDefaultCacheRows : setting.units;
    const auto directory = (rows / Ledger::kCachePageSlots
        + (rows % Ledger::kCachePageSlots != 0)) * sizeof(void*);
    cache_sweep::require(measured.configured_payload == rows * Ledger::kLedgerEntryBytes
        && measured.configured_metadata == directory,
        "ledger policy allowance omitted entries or directory pointers");
    cache_sweep::require(measured.payload % Ledger::kLedgerEntryBytes == 0
        && measured.payload / Ledger::kLedgerEntryBytes <= rows,
        "ledger allocation must contain complete entries within the row cap");
    cache_sweep::require(measured.metadata == (rows == 0 ? 0 : directory)
        && measured.payload + measured.metadata == measured.peak_resident,
        "ledger reports each allocated page and directory exactly once");
    cache_sweep::require(measured.fixed_control + measured.fixed_workspace
        == Ledger::resident_cache_bytes(0),
        "ledger fixed base omitted control or read workspace");
    // Leave records, cold/reread/fault/physical I/O and hit/eviction witnesses
    // exactly as the fixture measured them at the backing primitive.
    return result;
}

std::uint64_t reported_bytes(const cache_sweep::Telemetry& measured) {
    return measured.fixed_control + measured.fixed_workspace
        + measured.payload + measured.metadata;
}

std::uint64_t production_reported_ledger_bytes(std::size_t rows) {
    using Ledger = pineforge::TradeLedger;
    pineforge::detail::RunCacheDiagnostics sink;
    std::uint64_t reported = 0;
    {
        pineforge::detail::RunCacheDiagnosticsScope scope(sink);
        Ledger ledger(rows);
        ledger.set_scratch_directory(".");
        const auto population = Ledger::kDefaultCacheRows + 1;
        for (std::size_t i = 0; i < population; ++i)
            ledger.push_back(cache_sweep::ledger_row(i));
        cache_sweep::require(sink.resident_bytes() == Ledger::resident_cache_bytes(0),
            "a cold ledger contributes its fixed workspace exactly once to the production sink");
        for (std::size_t i = 0; i < rows; ++i)
            (void)ledger.entry_price_at(i);
        reported = sink.resident_bytes();
        cache_sweep::require(reported == ledger.cache_resident_bytes()
            && sink.resident_bytes() == reported && sink.allocation_bypass_count() == 0,
            "production queries must read the ledger's actual allocation once without mutation");
    }
    cache_sweep::require(sink.resident_bytes() == 0,
        "ledger destruction must detach its production accounting source");
    return reported;
}

void reported_ledger_bytes_are_counted_once() {
    using Ledger = pineforge::TradeLedger;
    const auto zero = private_ledger_rows({0, false, "accounting_zero"});
    cache_sweep::require(zero.cache.payload == 0 && zero.cache.metadata == 0,
        "zero-row ledger reported an optional allocation");
    const auto production_zero = production_reported_ledger_bytes(0);
    for (const auto rows : {std::size_t{515}, Ledger::kDefaultCacheRows}) {
        const auto sized = private_ledger_rows({rows, false, "accounting_sized"});
        const auto entries = rows * Ledger::kLedgerEntryBytes;
        const auto directory = (rows / Ledger::kCachePageSlots
            + (rows % Ledger::kCachePageSlots != 0)) * sizeof(void*);
        cache_sweep::require(sized.cache.fixed_control == zero.cache.fixed_control
            && sized.cache.fixed_workspace == zero.cache.fixed_workspace,
            "ledger cache policy changed the reported fixed base");
        const auto difference = reported_bytes(sized.cache) - reported_bytes(zero.cache);
        cache_sweep::require(difference == entries + directory
            && sized.cache.payload - zero.cache.payload == entries
            && sized.cache.metadata - zero.cache.metadata == directory,
            "reported bytes at N rows minus zero must count private pages exactly once");
        cache_sweep::require(production_reported_ledger_bytes(rows) - production_zero
            == entries + directory,
            "the production sink's N-minus-zero bytes must contain one private allocation charge");
        cache_sweep::compare(zero.output, sized.output);
        std::printf("LEDGER_CACHE_ACCOUNTING rows=%zu reported_difference=%llu "
                    "entry_bytes=%llu directory_bytes=%llu\n", rows,
            static_cast<unsigned long long>(difference),
            static_cast<unsigned long long>(entries),
            static_cast<unsigned long long>(directory));
    }
}
} // namespace

int main() {
    try {
        reported_ledger_bytes_are_counted_once();
        cache_sweep::sweep("ledger_rows_private", 1,
            pineforge::TradeLedger::kDefaultCacheRows,
            2 * pineforge::TradeLedger::kDefaultCacheRows, private_ledger_rows);
        return 0;
    } catch (const std::exception& error) {
        cache_sweep_ledger_io::measuring = cache_sweep_ledger_io::inject_fault = false;
        std::fprintf(stderr, "private ledger row sweep FAIL: %s\n", error.what());
        return 1;
    }
}
