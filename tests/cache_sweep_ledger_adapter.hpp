#pragma once

#include "cache_sweep_fixture.hpp"
#include <pineforge/trade_ledger.hpp>

#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>

// Same Linux syscall boundary as the existing ledger I/O rows. Only the
// selected read operation is measured: broker/report/oracle I/O is outside
// this window. This header belongs in one TU of an executable.
namespace cache_sweep_ledger_io {
inline bool measuring = false, inject_fault = false;
inline std::uint64_t reads = 0, bytes = 0;
}
extern "C" ssize_t pread(int fd, void* destination, std::size_t count, off_t offset) {
    using namespace cache_sweep_ledger_io;
    if (measuring) {
        ++reads;
        if (inject_fault) { errno = EIO; return -1; }
    }
    const auto received = static_cast<ssize_t>(
        ::syscall(SYS_pread64, fd, destination, count, offset));
    if (measuring && received > 0) bytes += static_cast<std::uint64_t>(received);
    return received;
}

namespace cache_sweep {
inline pf::Trade ledger_row(std::size_t index) {
    pf::Trade t{};
    t.entry_time = INT64_C(1736121600000) + static_cast<std::int64_t>(index) * 60000;
    t.exit_time = t.entry_time + 60000;
    t.entry_price = 100 + (index % 31) * .25;
    t.exit_price = 100 + (index % 17) * .5;
    t.qty = .25 * (1 + index % 7);
    t.pnl = index % 3 == 0 ? -0.0 : (index % 2 ? -1.5 : 2.5);
    t.pnl_pct = t.pnl / t.entry_price;
    t.is_long = index % 2 == 0;
    t.entry_bar_index = static_cast<int>(index);
    t.exit_bar_index = static_cast<int>(index + 1);
    t.entry_id = "e" + std::to_string(index);
    t.entry_comment = std::string("entry\0comment", 13);
    t.exit_comment = "exit-" + std::to_string(index % 13);
    t.exit_id = "x" + std::to_string(index);
    t.exit_from_bracket = index % 2 != 0;
    t.max_runup = index % 11 + .5;
    t.max_drawdown = index % 19 + .25;
    t.commission = (index % 23) * .125;
    t.entry_incarnation = 1000 + index;
    t.open_at_end = index % 5 == 0;
    t.close_cause = pf::execution::CloseCause::Bracket;
    return t;
}

inline void ledger_cache_telemetry(Telemetry& out, const pf::TradeLedger& ledger) {
    using Ledger = pf::TradeLedger;
    const auto rows = ledger.cache_rows();
    const auto pages = rows / Ledger::kCachePageSlots
        + (rows % Ledger::kCachePageSlots != 0);
    const auto directory = pages * sizeof(void*);
    const auto fixed = Ledger::resident_cache_bytes(0);
    const auto resident = ledger.cache_resident_bytes();
    require(resident >= fixed, "ledger accounting omitted fixed workspace");
    const auto optional = resident - fixed;
    // The directory is allocated in full on the first read. Entry pages
    // are private, lazy and clipped; there is no per-entry shared tag charge.
    const auto metadata = optional == 0 ? 0 : directory;
    require(optional >= metadata
        && (optional - metadata) % Ledger::kLedgerEntryBytes == 0,
        "ledger allocation must contain its directory and complete entries");
    require((optional - metadata) / Ledger::kLedgerEntryBytes <= rows,
        "ledger allocated more entries than its configured row budget");
    out.configured_payload = rows * Ledger::kLedgerEntryBytes;
    out.configured_metadata = directory;
    out.payload = optional - metadata;
    out.metadata = metadata;
    out.peak_resident = std::max<std::uint64_t>(out.peak_resident, optional);
    out.fixed_control = sizeof(Ledger);
    out.fixed_workspace = fixed - sizeof(Ledger);
}

inline Result ledger_rows(Setting setting) {
    using namespace cache_sweep_ledger_io;
    constexpr std::size_t population = 100003;
    pf::TradeLedger ledger;
    if (!setting.defaults) ledger.set_cache_rows(setting.units);
    ledger.set_scratch_directory(".");
    for (std::size_t i = 0; i < population; ++i) ledger.push_back(ledger_row(i));
    // Real rewind/reappend invalidates the read cache and leaves only the
    // last descriptor resident. Every measured older row must read its
    // descriptor on a miss, so a no-pread read is an actual decoded hit.
    ledger.resize(population - 1);
    ledger.push_back(ledger_row(population - 1));
    require(ledger.resident_base() == population - 1,
            "ledger census requires older descriptors to be physically cold");
    Result result;
    auto& s = result.cache;
    ledger_cache_telemetry(s, ledger);
    reads = bytes = 0;
    const auto read = [&](std::size_t index, const pf::Trade* oracle = nullptr) {
        const auto before_reads = reads;
        const auto before_occupied = ledger.cache_size();
        measuring = true;
        pf::Trade row{};
        try { row = ledger.read(index); }
        catch (...) { measuring = false; throw; }
        measuring = false;
        // Complete bytes, including the embedded NUL and signed zero.
        std::string actual, expected;
        trade(actual, row);
        trade(expected, oracle ? *oracle : ledger_row(index));
        require(actual == expected, "ledger cache changed a complete trade field");
        result.output.records.append(actual);
        if (index < ledger.resident_base()) {
            if (reads == before_reads) ++s.hits;
            else {
                ++s.misses;
                const auto occupied = ledger.cache_size();
                if (occupied > 0 && occupied == before_occupied) ++s.evictions;
            }
        }
        ledger_cache_telemetry(s, ledger);
    };
    read(0);
    s.cold_reads = reads;
    read(0);
    for (std::size_t index = 1; index < population; ++index) read(index);
    const auto before_reread = reads;
    read(0);
    s.evicted_reread_reads = reads - before_reread;
    // Change an OLD row's text and every numeric value through replace,
    // then restore its complete value; stale decoded hits must not survive.
    const auto changed = ledger_row(population + 17);
    ledger.replace(0, changed);
    read(0, &changed);
    ledger.replace(0, ledger_row(0));
    read(0);
    ledger_cache_telemetry(s, ledger);
    s.workspace = sizeof(pf::Trade); // Owning row/string output is a separate caller value.
    // Rewind/reappend invalidates all tags even for the large policy.
    ledger.resize(population - 1);
    ledger.push_back(ledger_row(population - 1));
    inject_fault = measuring = true;
    const auto attempts = reads;
    bool surfaced = false;
    try { static_cast<void>(ledger.read(0)); }
    catch (const std::system_error& error) { surfaced = error.code().value() == EIO; }
    catch (...) { inject_fault = measuring = false; throw; }
    inject_fault = measuring = false;
    require(surfaced && reads == attempts + 1, "ledger cold backing I/O fault did not surface");
    s.fault_reads = reads - attempts;
    s.inner_reads = reads;
    s.inner_bytes = bytes;
    return result;
}
} // namespace cache_sweep
