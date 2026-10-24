// Original I/O workloads use frozen2's public API; Round 4 adds field APIs.
// Linux-only registration:
// executable-local interposers forward every request to its real syscall.
// There is no production counter, test-only branch, or linker-option fence.
#include "p2cx_tledger_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <sys/syscall.h>
#include <unistd.h>

namespace ledger_io {
struct Counts {
    std::uint64_t reads = 0, writes = 0, written_bytes = 0, allocations = 0;
};
inline bool measuring = false;
inline Counts counts;

struct Measure {
    Measure() {
        REQUIRE(!measuring);
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counts finish() {
        measuring = false;
        return counts;
    }
};
} // namespace ledger_io

// Executable-local allocation counter: real storage still comes from malloc.
// Numeric-only measurement contains no expected-row copies or string reads.
// Production has no counter, branch or hook.
void* operator new(std::size_t bytes) {
    void* result = std::malloc(bytes ? bytes : 1);
    if (!result) throw std::bad_alloc();
    if (ledger_io::measuring) ++ledger_io::counts.allocations;
    return result;
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
// The nothrow forms must be replaced too: CommandScratchStore allocates with
// `new (std::nothrow)`, and a block from the sanitizer's own nothrow new would
// otherwise be released by the free() above (alloc-dealloc-mismatch).
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    void* result = std::malloc(bytes ? bytes : 1);
    if (result && ledger_io::measuring) ++ledger_io::counts.allocations;
    return result;
}
void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept {
    return ::operator new(bytes, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    if (ledger_io::measuring) ++ledger_io::counts.reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}

extern "C" ssize_t pwrite(int fd, const void* data, std::size_t length, off_t offset) {
    const auto written = static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, length, offset));
    if (ledger_io::measuring) {
        ++ledger_io::counts.writes;
        if (written > 0) ledger_io::counts.written_bytes += static_cast<std::uint64_t>(written);
    }
    return written;
}

namespace {
using namespace p2cx_tledger;
using ledger_io::Counts;
using ledger_io::Measure;

void forwarding_control() {
    detail::ScratchFile file;
    file.set_directory(".");
    file.open("p2cx-tledger-counter");
    const std::string expected("counter\0bytes", 13);
    std::string actual(expected.size(), '\0');
    Measure measure;
    file.write(0, expected.data(), expected.size());
    file.read(0, actual.data(), actual.size());
    const auto io = measure.finish();
    REQUIRE(actual == expected);
    REQUIRE(io.reads == 1);
    REQUIRE(io.writes == 1);
    REQUIRE(io.written_bytes == expected.size());
    Measure allocation_measure;
    void* block = ::operator new(257);
    ::operator delete(block);
    const auto allocation = allocation_measure.finish();
    REQUIRE(allocation.allocations == 1);
}

// Prepare genuine misses without inventing a per-bar OPEN -> replace path.
// A real string change displaces old extents from the 10,752-byte payload
// tail; a public rewind/reappend evicts the old descriptor window as well.
void pressure(TradeLedger& ledger, const std::vector<Trade>& expected) {
    Trade changed = expected.back();
    changed.exit_comment.assign(TradeLedger::kResidentRows * TradeLedger::kRecordBytes + 257, 'P');
    ledger.replace(expected.size() - 1, changed);
    ledger.replace(expected.size() - 1, expected.back());
    ledger.resize(expected.size() - 1);
    ledger.push_back(expected.back());
    REQUIRE(ledger.resident_base() == expected.size() - 1);
}

Counts closed_loop(Reader& host, const std::vector<Trade>& expected) {
    Measure measure;
    for (std::size_t i = 0; i < expected.size(); ++i)
        host.compare_fields(static_cast<int>(i), expected[i]);
    return measure.finish();
}

Counts bars_with_open_lot(int population, int bars_count, bool spilled) {
    Reader host;
    std::vector<Trade> expected;
    for (int i = 0; i < population; ++i) expected.push_back(row(i));
    Counts total;
    int measured_bars = 0;
    double first_runup = 0, first_drawdown = 0;
    host.calculation = [&](Host& base) {
        auto& reader = static_cast<Reader&>(base);
        if (base.calculations == 1) {
            // A real native OPEN lot; subsequent bars sample this lot, not
            // the closed ledger. Settle BEFORE inserting the oracle rows:
            // settlement can hash/read the ledger and warm its read cache.
            apply(base, put(base, tx(1.0, "still-open")));
            REQUIRE(base.lots().size() == 1);
            first_runup = base.lots()[0].max_runup;
            first_drawdown = base.lots()[0].max_drawdown;
            reader.ledger().set_scratch_directory(".");
            // Run begin sets the ledger budget from the run mode: no report
            // sink is installed here, so it is the default, not the sink cap.
            CHECK(reader.ledger().cache_rows() == TradeLedger::kDefaultCacheRows);
            for (const auto& value : expected) reader.ledger().push_back(value);
            if (spilled) pressure(reader.ledger(), expected);
            const auto cold = closed_loop(reader, expected);
            CHECK(cold.writes == 0);
            if (spilled) REQUIRE(cold.reads > 0); // Proves the miss path is reached.
            else CHECK(cold.reads == 0); // Honest four-tiny-row control.
        }
        const auto io = closed_loop(reader, expected);
        // Derived zero: every immutable row/string fits the configured cache.
        // OPEN-lot sampling does not write the closed ledger.
        CHECK(io.reads == 0);
        CHECK(io.writes == 0);
        total.reads += io.reads;
        total.writes += io.writes;
        total.written_bytes += io.written_bytes;
        ++measured_bars;
        REQUIRE(base.lots().size() == 1);
    };
    auto config = spec("p2cx-tledger-read-loop");
    config.report_open_position_at_end = false;
    REQUIRE(host.configure_native(config).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    for (int n = 0; n < bars_count; ++n) {
        const double close = 100.0 + (n % 3 - 1) * 0.25;
        bars.push_back({100.0, 101.0 + (n % 13) * 0.125,
                        99.0 - (n % 17) * 0.125, close, 1.0, T + n * 60000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    completed(host);
    REQUIRE(measured_bars == bars_count);
    REQUIRE(host.closed_trade_count() == expected.size());
    REQUIRE(host.lots().size() == 1);
    CHECK(host.lots()[0].max_runup > first_runup);
    CHECK(host.lots()[0].max_drawdown > first_drawdown);
    return total;
}

void per_bar_reads() {
    // The four-tiny-row case is already green on frozen2 and is labelled so.
    const auto control = bars_with_open_lot(4, 1000, false);
    CHECK(control.reads == 0 && control.writes == 0);
    // 300 > kSinkCacheRows (128): a sink-off run must keep the DEFAULT budget
    // set at run begin, so 300 immutable rows still fit and the loop does no I/O.
    static_assert(300 > TradeLedger::kSinkCacheRows && 300 < TradeLedger::kDefaultCacheRows,
                  "population must separate the sink budget from the default budget");
    for (const int population : {4, 32, 300}) {
        const auto small = bars_with_open_lot(population, 1000, true);
        const auto large = bars_with_open_lot(population, 10000, true);
        CHECK(small.reads == 0 && small.writes == 0);
        CHECK(large.reads == small.reads);
        CHECK(large.writes == small.writes);
    }
}

void unchanged_strings_only_write_descriptor() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    Trade expected = row(0);
    ledger.push_back(expected);
    // All original strings are short and resident; only numeric bytes change.
    Measure measure;
    constexpr int changes = 1000;
    for (int n = 0; n < changes; ++n) {
        expected.max_runup = n + 0.5;
        ledger.replace(0, expected);
        same_trade(ledger.read(0), expected);
    }
    const auto io = measure.finish();
    CHECK(io.reads == 0);
    // Every replacement writes one descriptor; a 64 KiB scratch write
    // boundary may split that descriptor into two actual pwrite calls.
    const auto window_cuts = (changes + 1) * TradeLedger::kRecordBytes
        / detail::ScratchFile::kCacheBytes;
    CHECK(io.writes >= changes);
    CHECK(io.writes <= changes + window_cuts);
    CHECK(io.written_bytes == changes * TradeLedger::kRecordBytes);
    // A genuinely changed, embedded-NUL string must still write its bytes.
    expected.exit_comment.assign("changed\0comment", 15);
    Measure changed_measure;
    ledger.replace(0, expected);
    const auto changed_io = changed_measure.finish();
    CHECK(changed_io.writes == 2);
    CHECK(changed_io.written_bytes == TradeLedger::kRecordBytes + expected.exit_comment.size());
    same_trade(ledger.read(0), expected);
}

void specialized_reads_share_warmed_rows() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    for (int n = 0; n < 32; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    pressure(ledger, expected);
    for (std::size_t i = 0; i < expected.size(); ++i)
        same_trade(ledger.read(i), expected[i]);
    Measure measure;
    for (int pass = 0; pass < 20; ++pass) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            same_numeric(ledger.numeric_at(i), expected[i]);
            CHECK(ledger.entry_id_at(i) == expected[i].entry_id);
        }
    }
    const auto io = measure.finish();
    CHECK(io.reads == 0 && io.writes == 0);
}

void hungpixi_population_field_reads() {
    constexpr std::size_t population = 24238;
    Reader host;
    auto& ledger = host.ledger();
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    expected.reserve(population);
    for (std::size_t i = 0; i < population; ++i) {
        expected.push_back(row(static_cast<int>(i)));
        const auto& value = expected.back();
        // Some comments exceed both common std::string SSO capacities.
        // Building a Trade for a numeric field therefore allocates, while
        // all four strings together still fit the 128-byte inline allowance.
        REQUIRE(value.entry_comment.size() > 23);
        REQUIRE(value.entry_id.size() + value.entry_comment.size()
                + value.exit_comment.size() + value.exit_id.size()
                <= TradeLedger::kCacheStringBytes);
        ledger.push_back(value);
    }
    REQUIRE(ledger.cache_rows() >= population);
    Measure warmup;
    for (std::size_t i = 0; i < population; ++i) {
        same_numeric_fields(ledger, i, expected[i]);
        same_string_fields(ledger, i, expected[i]);
        host.compare_fields(static_cast<int>(i), expected[i]);
    }
    const auto cold = warmup.finish();
    REQUIRE(cold.reads > 0); // Old descriptors/strings really leave the tails.
    CHECK(cold.writes == 0);
    CHECK(ledger.cache_size() == population);
    const auto fixed_bytes = TradeLedger::resident_cache_bytes(1)
        - TradeLedger::kLedgerEntryBytes;
    // next_power_of_two(24,238) = 32,768, below the 65,536-row default cap.
    CHECK(ledger.cache_resident_bytes()
          == fixed_bytes + 32768 * TradeLedger::kLedgerEntryBytes);
    for (int bar = 0; bar < 8; ++bar) {
        Measure numeric;
        for (std::size_t i = 0; i < population; ++i) {
            same_numeric_fields(ledger, i, expected[i]);
            host.compare_numeric_fields(static_cast<int>(i), expected[i]);
        }
        const auto numbers = numeric.finish();
        CHECK(numbers.reads == 0 && numbers.writes == 0);
        CHECK(numbers.allocations == 0);
        Measure strings;
        for (std::size_t i = 0; i < population; ++i) {
            same_string_fields(ledger, i, expected[i]);
            host.compare_string_fields(static_cast<int>(i), expected[i]);
        }
        const auto texts = strings.finish();
        CHECK(texts.reads == 0 && texts.writes == 0);
        CHECK(ledger.cache_size() == population);
    }
}

void wide_strings_keep_numeric_rows_cached() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected{row(0), row(1)};
    expected[0].exit_comment.assign(TradeLedger::kCacheStringBytes + 1, 'W');
    for (const auto& value : expected) ledger.push_back(value);
    pressure(ledger, expected);
    same_numeric_fields(ledger, 0, expected[0]); // Warm just the numeric record.
    CHECK(ledger.cache_size() == 1);
    Measure numeric;
    for (int pass = 0; pass < 8; ++pass)
        same_numeric_fields(ledger, 0, expected[0]);
    const auto numbers = numeric.finish();
    CHECK(numbers.reads == 0 && numbers.writes == 0);
    CHECK(numbers.allocations == 0);
    Measure strings;
    same_string_fields(ledger, 0, expected[0]);
    const auto texts = strings.finish();
    CHECK(texts.reads > 0); // Wide strings deliberately retain scratch fallback.
    CHECK(texts.writes == 0);
    same_trade(ledger.read(0), expected[0]);
}
} // namespace

int main() {
    test("real forwarded positional I/O counters", forwarding_control);
    test("closedtrades loops with a live OPEN lot", per_bar_reads);
    test("equal strings retain extents, changed strings append", unchanged_strings_only_write_descriptor);
    test("numeric and id accessors share decoded cache", specialized_reads_share_warmed_rows);
    test("24,238 closed rows: field reads have no warm I/O or numeric allocation",
         hungpixi_population_field_reads);
    test("wide strings retain cached numeric fields", wide_strings_keep_numeric_rows_cached);
    std::printf("test_p2cx_tledger_io: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
