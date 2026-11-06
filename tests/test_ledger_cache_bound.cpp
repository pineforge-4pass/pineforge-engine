// Bounded ledger reads preserve values beyond the decoded cache capacity.
// Linux registration and the native admission dependency are in REGISTRATION.patch.
// Red-first: previous headers lack set_ledger_cache_max_bytes and its diagnostics.
#include "p2cx_tledger_fixture.hpp"
#include <pineforge/cache_policy.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/report_pages.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/source/pine_strategy_host.hpp>
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void bind(TradeLedger& ledger, std::function<int(std::uint64_t)> factory,
                     std::uint64_t bytes) {
        ledger.bind_retained_history(std::move(factory), bytes);
    }
    static auto retained(const TradeLedger& ledger) { return ledger.retained_history_bytes(); }
    static auto source(const TradeLedger& ledger) { return ledger.retained_trade_source(); }
    static void retire(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
}

namespace bound_io {
struct Counts { std::uint64_t reads = 0, bytes = 0, failures = 0; };
inline Counts counts;
inline bool measuring = false;
inline int next_read_error = 0;
inline std::size_t read_error_after = 0;
inline bool descriptor_fault = false;
inline int damaged_fd = -1;
inline unsigned short_writes = 0, write_errors = 0, remaining_errors = 0;
struct Measure {
    Measure() { REQUIRE(!measuring); counts = {}; measuring = true; }
    ~Measure() { measuring = false; next_read_error = 0; read_error_after = 0; }
    Counts finish() { measuring = false; return counts; }
};
struct WriteFault {
    explicit WriteFault(bool poison = false) {
        REQUIRE(!descriptor_fault);
        descriptor_fault = true;
        damaged_fd = -1;
        short_writes = write_errors = 0;
        remaining_errors = poison ? 2 : 1;
    }
    ~WriteFault() { descriptor_fault = false; damaged_fd = -1; }
};
}

// Independent, forwarding kernel observations. No product counter can stand
// in for these bytes, refusals or the short write actually reaching the fd.
extern "C" ssize_t pread(int fd, void* out, std::size_t bytes, off_t offset) {
    using namespace bound_io;
    ssize_t result;
    if (measuring && next_read_error && read_error_after == 0) {
        errno = next_read_error;
        next_read_error = 0;
        result = -1;
    } else result = static_cast<ssize_t>(::syscall(SYS_pread64, fd, out, bytes, offset));
    if (measuring) {
        if (result > 0 && next_read_error && read_error_after) --read_error_after;
        ++counts.reads;
        if (result > 0) counts.bytes += static_cast<std::uint64_t>(result);
        if (result < 0) ++counts.failures;
    }
    return result;
}
extern "C" ssize_t pwrite(int fd, const void* in, std::size_t bytes, off_t offset) {
    using namespace bound_io;
    if (descriptor_fault && damaged_fd == fd) {
        ++write_errors;
        if (--remaining_errors == 0) descriptor_fault = false;
        errno = ENOSPC;
        return -1;
    }
    if (descriptor_fault && bytes == pineforge::TradeLedger::kRecordBytes) {
        const auto result = static_cast<ssize_t>(
            ::syscall(SYS_pwrite64, fd, in, bytes / 2, offset));
        if (result > 0) { damaged_fd = fd; ++short_writes; }
        return result;
    }
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, in, bytes, offset));
}

extern "C" __attribute__((visibility("default")))
int strategy_reads_trade_history() { return 1; }

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
using Probe = pineforge::detail::TradeLedgerStorageProbe;

// An independent geometry oracle, including key/generation/rank and pointers.
// Binary search deliberately differs from the product's quotient conversion.
constexpr Size entry_bytes = 168 + 2 * sizeof(Size) + 128;
Size pages(Size rows) { return rows / 256 + (rows % 256 != 0); }
bool fits(Size rows, Size bytes) {
    const auto directory = pages(rows) * sizeof(void*);
    return directory <= bytes && rows <= (bytes - directory) / entry_bytes;
}
Size rows_for(Size bytes) {
    Size low = 0, high = bytes / entry_bytes;
    while (low < high) {
        const auto middle = low + (high - low) / 2 + 1;
        if (fits(middle, bytes)) low = middle;
        else high = middle - 1;
    }
    return low;
}
Size charge(Size rows) { return rows * entry_bytes + pages(rows) * sizeof(void*); }
std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30; value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27; value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
double word(std::uint64_t bits) {
    double result;
    std::memcpy(&result, &bits, sizeof result);
    return result;
}
Trade value(Size index, std::uint64_t seed, bool wide = false) {
    auto result = row(static_cast<int>(index));
    const auto key = mix(seed + index);
    result.entry_time += static_cast<std::int64_t>(key % 997);
    result.exit_time += static_cast<std::int64_t>(key % 991);
    result.entry_price = 17 + (key % 100003) * 0.125;
    result.exit_price = result.entry_price + (key % 19) * 0.25;
    result.qty = 0.25 * (1 + key % 31);
    result.pnl = index % 7 == 0 ? word(UINT64_C(0x8000000000000000))
        : static_cast<double>(static_cast<std::int64_t>(key % 101) - 50);
    result.pnl_pct = index % 17 == 0
        ? word(UINT64_C(0x7ff8000000000000) | (key & UINT64_C(0xffff)))
        : result.pnl / result.entry_price;
    result.max_runup = (key % 997) * 0.0625;
    result.max_drawdown = (key % 991) * 0.03125;
    result.commission = (key % 127) * 0.015625;
    result.entry_incarnation = key;
    result.is_long = (key & 1) != 0;
    result.exit_from_bracket = (key & 2) != 0;
    result.open_at_end = (key & 4) != 0;
    result.close_cause = result.exit_from_bracket ? ex::CloseCause::Bracket
                                                : ex::CloseCause::Script;
    result.entry_id = std::to_string(key) + std::string("\0id", 3);
    result.exit_id = std::to_string(key ^ seed);
    result.entry_comment.assign(1 + key % 11, static_cast<char>('a' + key % 26));
    result.exit_comment.assign(wide ? 1025 + key % 79 : 1 + key % 13,
                               static_cast<char>('A' + key % 26));
    result.exit_comment[result.exit_comment.size() / 2] = '\0';
    return result;
}
Trade numeric_value(Size index, std::uint64_t seed) {
    auto result = value(index, seed);
    result.entry_id.clear(); result.exit_id.clear();
    result.entry_comment.clear(); result.exit_comment.clear();
    return result;
}
void append(TradeLedger& ledger, Size from, Size end, std::uint64_t seed, bool numeric = false) {
    for (Size i = from; i < end; ++i)
        ledger.push_back(numeric ? numeric_value(i, seed) : value(i, seed));
}
void bound(const TradeLedger& ledger, Size bytes, Size fixed) {
    CHECK(ledger.ledger_cache_max_bytes() == bytes);
    CHECK(ledger.cache_rows() == rows_for(bytes));
    CHECK(ledger.cache_size() <= ledger.cache_rows());
    CHECK(ledger.cache_resident_bytes() >= fixed);
    CHECK(ledger.cache_resident_bytes() - fixed <= bytes);
}
template<class Action>
void typed_io(Action action, int expected) {
    bool caught = false;
    try { action(); }
    catch (const detail::ScratchIoError& error) {
        caught = true;
        CHECK(error.code().value() == expected);
        CHECK(classify_run_failure(error).code == RunFailureCode::report_sink_failed);
    }
    REQUIRE(caught);
}

struct Backing {
    int latest = -1;
    unsigned creations = 0;
    auto factory() {
        return [this](std::uint64_t bytes) {
            const int fd = static_cast<int>(::syscall(SYS_memfd_create,
                "ledger-bound-control", MFD_CLOEXEC));
            if (fd < 0) return -1;
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
                const auto saved = errno; ::close(fd); errno = saved; return -1;
            }
            latest = fd; ++creations;
            return fd; // ownership is transferred; this fixture keeps no dup
        };
    }
};

void forwarding_controls() {
    Backing backing;
    const auto fd = backing.factory()(4096);
    REQUIRE(fd >= 0);
    const std::array<char, 11> expected{{'b','o','u','n','d','\0','r','e','a','l','!'}};
    std::array<char, 11> actual{};
    REQUIRE(::pwrite(fd, expected.data(), expected.size(), 0) == 11);
    bound_io::Measure measure;
    REQUIRE(::pread(fd, actual.data(), actual.size(), 0) == 11);
    const auto io = measure.finish();
    CHECK(actual == expected);
    CHECK(io.reads == 1 && io.bytes == expected.size() && io.failures == 0);
    REQUIRE(::close(fd) == 0);
    const auto refused = global_allocation::refused;
    global_allocation::fail_after = 0;
    void* missing = ::operator new(17, std::nothrow);
    global_allocation::fail_after = std::numeric_limits<Size>::max();
    CHECK(missing == nullptr && global_allocation::refused == refused + 1);
    ::operator delete(missing);
}

void byte_contract(std::uint64_t seed) {
    const auto minimum = entry_bytes + sizeof(void*);
    for (const Size bytes : {Size{0}, Size{1}, minimum - 1, minimum, minimum + 1,
                              charge(3) + 17, charge(257) + 29, charge(1025) + 31}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_ledger_cache_max_bytes(bytes);
        const auto fixed = ledger.cache_resident_bytes();
        append(ledger, 0, 3 * 256 + 19, seed);
        for (Size i = 0; i < ledger.size(); ++i) {
            same_trade(ledger.read(i), value(i, seed));
            same_numeric_fields(ledger, i, value(i, seed));
            same_string_fields(ledger, i, value(i, seed));
            bound(ledger, bytes, fixed);
        }
        ledger.clear();
        bound(ledger, bytes, fixed);
        CHECK(ledger.read_cache_stats().allocation_attempt_count == 0);
    }
    TradeLedger maximum;
    maximum.set_ledger_cache_max_bytes(std::numeric_limits<Size>::max());
    CHECK(maximum.ledger_cache_max_bytes() == std::numeric_limits<Size>::max());
    CHECK(maximum.cache_rows() == rows_for(std::numeric_limits<Size>::max()));
    CHECK(maximum.read_cache_stats().allocation_attempt_count == 0);
    // Never allocate a SIZE_MAX-declared directory merely to test admission.
    TradeLedger defaults;
    CacheDefaults policy;
    CHECK(policy.ledger_cache_max_bytes == TradeLedger::kDefaultLedgerCacheMaxBytes);
    CHECK(defaults.cache_rows() == rows_for(policy.ledger_cache_max_bytes));
    TradeLedger wrapper(policy.ledger_rows);
    CHECK(wrapper.ledger_cache_max_bytes() == policy.ledger_cache_max_bytes);
    policy.ledger_cache_max_bytes = charge(7) + 3;
    policy.apply_ledger_cache(wrapper);
    CHECK(wrapper.cache_rows() == 7);
    policy.ledger_rows = CacheCapacity{charge(3)};
    policy.apply_ledger_cache(wrapper);
    CHECK(wrapper.cache_rows() == 3); // explicit installed policy takes precedence
    TradeLedger finite(TradeLedger::kMaxCacheRows);
    CHECK(finite.cache_rows() == TradeLedger::kMaxCacheRows);
    bool refused = false;
    try { finite.set_cache_rows(TradeLedger::kMaxCacheRows + 1); }
    catch (const coded<std::invalid_argument>& error) {
        refused = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::setting_rejected);
    }
    REQUIRE(refused);
}

void associative_and_cold_cost(std::uint64_t seed, Size population) {
    const Size rows = 4 * 256;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(charge(rows));
    append(ledger, 0, population, seed);
    // Drop the fixed descriptor window without inventing a hidden cache hook.
    ledger.resize(population - 1);
    ledger.push_back(value(population - 1, seed));
    const auto fixed = TradeLedger::resident_cache_bytes(0);
    const auto sets = rows / 4;
    const std::array<Size, 4> adversarial{{0, rows, 2 * rows, 3 * rows}};
    REQUIRE(population > adversarial.back() + TradeLedger::kResidentRows);
    for (const auto index : adversarial) same_trade(ledger.read(index), value(index, seed));
    const auto warm_before = ledger.read_cache_stats();
    bound_io::Measure warm;
    for (unsigned pass = 0; pass < 7; ++pass)
        for (const auto index : adversarial)
            same_trade(ledger.read(index), value(index, seed));
    const auto warm_io = warm.finish();
    CHECK(warm_io.reads == 0 && warm_io.bytes == 0);
    const auto warm_after = ledger.read_cache_stats();
    CHECK(warm_after.hot_hit_count > warm_before.hot_hit_count);
    CHECK(warm_after.lookup_probe_count - warm_before.lookup_probe_count <= 7 * 4 * 4);
    // A fifth old key with the same set genuinely evicts; still one descriptor,
    // never a population-sized scan or reindex. Text prefetch is at most 128 B.
    const Size fifth = sets;
    const auto before = ledger.read_cache_stats();
    bound_io::Measure miss;
    same_trade(ledger.read(fifth), value(fifth, seed));
    const auto cold = miss.finish();
    const auto after = ledger.read_cache_stats();
    CHECK(after.descriptor_read_count == before.descriptor_read_count + 1);
    CHECK(after.descriptor_read_bytes == before.descriptor_read_bytes + 168);
    CHECK(cold.bytes >= 168 && cold.bytes <= 168 + 128);
    CHECK(cold.reads > 0 && cold.failures == 0);
    CHECK(after.allocation_attempt_count == before.allocation_attempt_count);
    // Recent last-eight reads and noncolliding old rows are separate workloads.
    for (Size i = population - 8; i < population; ++i)
        CHECK(ledger.entry_id_at(i) == value(i, seed).entry_id);
    bound_io::Measure recent;
    for (Size i = population - 8; i < population; ++i)
        CHECK(ledger.entry_id_at(i) == value(i, seed).entry_id);
    CHECK(recent.finish().bytes == 0);
    bound(ledger, charge(rows), fixed);
}

void allocation_census(std::uint64_t seed, Size population) {
    REQUIRE(global_allocation::live_bytes == 0);
    for (const Size rows : {Size{0}, Size{1}, Size{3}, Size{257}, Size{1025}}) {
        const auto allowance = rows ? charge(rows) + 7 : 0;
        {
            TradeLedger ledger;
            ledger.set_scratch_directory(".");
            ledger.set_ledger_cache_max_bytes(allowance);
            append(ledger, 0, population, seed, true);
            std::vector<double> expected;
            expected.reserve(population);
            for (Size i = 0; i < population; ++i)
                expected.push_back(numeric_value(i, seed).entry_price);
            const auto fixed = ledger.cache_resident_bytes();
            global_allocation::peak_bytes = 0;
            global_allocation::recording = true;
            for (Size i = 0; i < population; ++i)
                CHECK(same_bits(ledger.entry_price_at(i), expected[i]));
            global_allocation::recording = false;
            // The oracle was allocated before the window. Peak includes the
            // real cold directory and every lazy page allocation/transient.
            REQUIRE(global_allocation::live_bytes == ledger.cache_resident_bytes() - fixed);
            CHECK(global_allocation::peak_bytes <= allowance);
            CHECK(global_allocation::live_bytes <= allowance);
            const auto allocated = ledger.read_cache_stats().allocation_attempt_count;
            const auto resident = ledger.cache_resident_bytes();
            global_allocation::peak_bytes = global_allocation::live_bytes;
            global_allocation::recording = true;
            double consume = 0;
            for (Size pass = 0; pass < 3; ++pass)
                for (Size i = 0; i < population; ++i) consume += ledger.entry_price_at(i);
            global_allocation::recording = false;
            CHECK(consume > 0);
            CHECK(global_allocation::peak_bytes <= allowance);
            CHECK(ledger.cache_resident_bytes() == resident);
            CHECK(ledger.read_cache_stats().allocation_attempt_count == allocated);
        }
        CHECK(global_allocation::live_bytes == 0 && global_allocation::live_blocks == 0);
    }
}

void retirement_rewind_and_materialization(std::uint64_t seed) {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(charge(257) + 13);
    Probe::retire(ledger);
    const Size population = 3 * TradeLedger::kResidentRows + 17;
    append(ledger, 0, population, seed);
    const Trade owning = ledger.read(0);
    same_trade(ledger.materialize(0), owning); // explicit legacy owning boundary
    const auto changed = value(population - 2, seed + 7, true);
    ledger.replace(population - 2, changed);
    const auto published = population - 5;
    ledger.seal(published);
    CHECK(ledger.base() == published);
    CHECK(ledger.cache_size() == 0); // retirement invalidates/releases hot pages
    bool refused = false;
    try { (void)ledger.entry_id_at(0); }
    catch (const std::logic_error& error) {
        refused = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
    REQUIRE(refused);
    same_trade(owning, value(0, seed));
    same_trade(ledger.read(population - 2), changed);
    TradeLedger copy(ledger);
    CHECK(copy.base() == published);
    same_trade(copy.read(population - 2), changed);
    ledger.resize(population - 2);
    const auto recut = value(population - 2, seed + 29);
    ledger.push_back(recut);
    same_trade(ledger.read(population - 2), recut);
    same_trade(copy.read(population - 2), changed);
}

void owning_return_failure(std::uint64_t seed) {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(charge(7));
    auto expected = value(0, seed, true);
    expected.entry_id.assign(1025, 'q');
    ledger.push_back(expected);
    same_trade(ledger.read(0), expected); // numeric entry warm, owning text wide
    const auto failures = ledger.read_cache_stats().allocation_bypass_count;
    const auto refused = global_allocation::refused;
    bool caught = false;
    global_allocation::fail_after = 0;
    try { (void)ledger.read(0); } catch (const std::bad_alloc&) { caught = true; }
    global_allocation::fail_after = std::numeric_limits<Size>::max();
    REQUIRE(caught && global_allocation::refused == refused + 1);
    CHECK(ledger.read_cache_stats().allocation_bypass_count == failures);
    same_trade(ledger.read(0), expected);
}

void allocation_failures(std::uint64_t seed) {
    const auto maximum = std::numeric_limits<Size>::max();
    CHECK(!TradeLedger::cache_retry_population_reached(0, 0));
    CHECK(TradeLedger::cache_retry_population_reached(0, 1));
    CHECK(!TradeLedger::cache_retry_population_reached(1, 1));
    CHECK(TradeLedger::cache_retry_population_reached(1, 2));
    CHECK(TradeLedger::cache_retry_population_reached(maximum / 2, maximum - 1));
    CHECK(!TradeLedger::cache_retry_population_reached(maximum / 2 + 1, maximum));
    CHECK(!TradeLedger::cache_retry_population_reached(maximum, maximum));
    // Directory failure, first-page failure, and later-page failure with an
    // old valid entry. The allocator independently asserts every fault fired.
    for (const unsigned stage : {0u, 1u, 2u}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_ledger_cache_max_bytes(charge(1024));
        const Size failed_population = 1024;
        append(ledger, 0, failed_population, seed, true);
        if (stage == 2) (void)ledger.entry_price_at(0); // old page/hit
        const Size key = stage == 2 ? 64 : 0;
        const auto old_resident = ledger.cache_resident_bytes();
        auto before = ledger.read_cache_stats();
        auto refused = global_allocation::refused;
        global_allocation::fail_after = stage == 1 ? 1 : 0;
        const auto actual = ledger.entry_price_at(key);
        global_allocation::fail_after = maximum;
        REQUIRE(global_allocation::refused == refused + 1);
        CHECK(same_bits(actual, numeric_value(key, seed).entry_price));
        auto observed = ledger.read_cache_stats();
        CHECK(observed.allocation_bypass_count == before.allocation_bypass_count + 1);
        CHECK(observed.allocation_attempt_count == before.allocation_attempt_count
              + (stage == 1 ? 2 : 1));
        CHECK(observed.failed_population == failed_population);
        CHECK(observed.allocation_retry_blocked);
        if (stage == 2) {
            CHECK(ledger.cache_resident_bytes() == old_resident);
            bound_io::Measure preserved;
            CHECK(same_bits(ledger.entry_price_at(0), numeric_value(0, seed).entry_price));
            CHECK(preserved.finish().bytes == 0);
        }
        for (unsigned doubling = 0; doubling < 2; ++doubling) {
            const auto target = observed.failed_population * 2;
            const auto attempts = observed.allocation_attempt_count;
            append(ledger, ledger.size(), target - 1, seed, true);
            for (Size i = 0; i < 11; ++i) (void)ledger.entry_price_at(key);
            CHECK(ledger.read_cache_stats().allocation_attempt_count == attempts);
            const auto next = numeric_value(target - 1, seed);
            refused = global_allocation::refused;
            global_allocation::fail_after = 0;
            ledger.push_back(next); // threshold may retry a missing page here
            (void)ledger.entry_price_at(key); // directory-cold case retries here
            global_allocation::fail_after = maximum;
            REQUIRE(global_allocation::refused == refused + 1);
            observed = ledger.read_cache_stats();
            CHECK(observed.allocation_attempt_count == attempts + 1);
            CHECK(observed.allocation_bypass_count == doubling + 2);
            CHECK(observed.failed_population == target);
            CHECK(ledger.size() == target);
            same_trade(ledger.read(target - 1), next);
        }
        // Reach the next threshold without a fault. Observe genuine allocator
        // calls and successful cache growth, not only the failure counter.
        const auto recovery_population = observed.failed_population * 2;
        append(ledger, ledger.size(), recovery_population - 1, seed, true);
        const auto recovery_row = numeric_value(recovery_population - 1, seed);
        const auto recovery_price = numeric_value(key, seed).entry_price;
        const auto attempts_before_recovery = ledger.read_cache_stats().allocation_attempt_count;
        const auto allocations_before_recovery = global_allocation::allocations;
        ledger.push_back(recovery_row);
        const auto recovered_price = ledger.entry_price_at(key);
        const auto recovery_allocations = global_allocation::allocations - allocations_before_recovery;
        const auto recovered = ledger.read_cache_stats();
        REQUIRE(recovered.allocation_attempt_count > attempts_before_recovery);
        CHECK(recovery_allocations == recovered.allocation_attempt_count - attempts_before_recovery);
        CHECK(recovered.allocation_bypass_count == observed.allocation_bypass_count);
        CHECK(!recovered.allocation_retry_blocked);
        CHECK(same_bits(recovered_price, recovery_price));
        CHECK(ledger.cache_size() != 0);
        if (stage == 2) {
            bound_io::Measure preserved;
            (void)ledger.entry_price_at(0);
            CHECK(preserved.finish().bytes == 0);
        }
        // Establish another real failure before testing a rewind. Geometry
        // has four pages; key 128 belongs to the still-cold third page.
        const auto refused_again = global_allocation::refused;
        global_allocation::fail_after = 0;
        (void)ledger.entry_price_at(128);
        global_allocation::fail_after = maximum;
        REQUIRE(global_allocation::refused == refused_again + 1);
        // A rewind neither frees a stale equality key nor invents an early retry.
        ledger.resize(failed_population / 2);
        const auto attempts = ledger.read_cache_stats().allocation_attempt_count;
        (void)ledger.entry_price_at(0);
        CHECK(ledger.read_cache_stats().allocation_attempt_count == attempts);
        TradeLedger copy(ledger);
        CHECK(!copy.cache_allocation_failed());
        CHECK(copy.read_cache_stats().allocation_bypass_count == 0);
        (void)copy.entry_price_at(0);
        CHECK(copy.cache_size() != 0);
        const auto declared = copy.ledger_cache_max_bytes();
        TradeLedger moved(std::move(copy));
        CHECK(moved.ledger_cache_max_bytes() == declared);
        CHECK(copy.empty() && copy.ledger_cache_max_bytes() == declared);
        ledger.clear();
        CHECK(!ledger.cache_allocation_failed());
        ledger.push_back(numeric_value(0, seed));
        (void)ledger.entry_price_at(0);
        CHECK(ledger.cache_size() == 1);
    }
}

void finite_failure_compatibility(std::uint64_t seed) {
    TradeLedger ledger(Size{1024});
    ledger.set_scratch_directory(".");
    append(ledger, 0, 128, seed, true);
    const auto refused = global_allocation::refused;
    global_allocation::fail_after = 0;
    (void)ledger.entry_price_at(0);
    global_allocation::fail_after = std::numeric_limits<Size>::max();
    REQUIRE(global_allocation::refused == refused + 1);
    const auto attempts = ledger.read_cache_stats().allocation_attempt_count;
    append(ledger, ledger.size(), 8 * 128, seed, true);
    (void)ledger.entry_price_at(0);
    CHECK(ledger.read_cache_stats().allocation_attempt_count == attempts);
    CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
    CHECK(ledger.read_cache_stats().allocation_retry_blocked);
    ledger.clear();
    CHECK(ledger.cache_rows() == 1024 && !ledger.cache_allocation_failed());
}

void string_mismatch_keeps_late_io_failure(std::uint64_t seed) {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(charge(7));
    auto expected = value(0, seed, true);
    expected.exit_comment.assign(2 * detail::ScratchFile::kCacheBytes + 19, 'z');
    expected.exit_comment[31] = '\0';
    ledger.push_back(expected);
    (void)ledger.entry_price_at(0); // warm descriptor without reading wide text
    const auto failures = ledger.read_cache_stats().allocation_bypass_count;
    bound_io::Measure measure;
    bound_io::next_read_error = EIO;
    bound_io::read_error_after = 1;
    typed_io([&] { (void)ledger.exit_comment_equals(0, std::string_view{}); }, EIO);
    const auto io = measure.finish();
    REQUIRE(io.reads >= 2 && io.bytes > 0 && io.failures == 1);
    CHECK(ledger.read_cache_stats().allocation_bypass_count == failures);
    same_trade(ledger.read(0), expected);
}

void supplied_backing_and_transactions(std::uint64_t seed) {
    for (const Size allowance : {Size{0}, charge(7) + 13, charge(1025)}) {
        Backing backing;
        TradeLedger ledger;
        ledger.set_ledger_cache_max_bytes(allowance);
        const Size population = 3 * TradeLedger::kResidentRows + 11;
        Probe::bind(ledger, backing.factory(), population * 8192);
        for (Size i = 0; i < population; ++i) ledger.push_back(value(i, seed, true));
        ledger.seal(population / 2); // retained backing must remain readable
        const Trade held = ledger.read(0);
        for (Size i = 0; i < population; ++i) {
            same_trade(ledger.read(i), value(i, seed, true));
            same_numeric_fields(ledger, i, value(i, seed, true));
            same_string_fields(ledger, i, value(i, seed, true));
        }
        same_trade(held, value(0, seed, true));
        auto source = Probe::source(ledger);
        CHECK(source->size() == population);
        std::array<char, 37> chunk{};
        for (Size i = 0; i < population; ++i) {
            const auto wanted = value(i, seed, true);
            CHECK(same_bits(source->at(i).value.pnl, wanted.pnl));
            const auto length = source->text_size(i, report::TradeTextV1::ExitComment);
            REQUIRE(length == wanted.exit_comment.size());
            std::string text;
            for (std::uint64_t offset = 0; offset < length;) {
                const auto got = source->read_text(i, report::TradeTextV1::ExitComment,
                                                  offset, chunk.data(), chunk.size());
                REQUIRE(got != 0);
                text.append(chunk.data(), got); offset += got;
            }
            CHECK(text == wanted.exit_comment);
        }
        const Size key = population - 2;
        auto changed = value(key, seed + 11, true);
        ledger.replace(key, changed);
        same_trade(ledger.read(key), changed);
        const auto failures = ledger.read_cache_stats().allocation_bypass_count;
        {
            bound_io::WriteFault fault;
            typed_io([&] { ledger.replace(key, value(key, seed + 19, true)); }, ENOSPC);
            REQUIRE(bound_io::short_writes == 1 && bound_io::write_errors == 1);
        }
        same_trade(ledger.read(key), changed);
        CHECK(ledger.read_cache_stats().allocation_bypass_count == failures);
        TradeLedger copy(ledger);
        REQUIRE(backing.creations == 2);
        CHECK(Probe::retained(copy)[0] == Probe::retained(ledger)[0]);
        copy.clear();
        REQUIRE(backing.creations == 3);
        copy.push_back(value(0, seed + 1));
        same_trade(ledger.read(0), held);
        // A true backing failure keeps its typed cause, even if ENOMEM.
        ledger.resize(population - 1); // invalidates hot entries
        for (const int error : {EIO, ENOMEM}) {
            bound_io::Measure measure;
            bound_io::next_read_error = error;
            typed_io([&] { (void)ledger.entry_price_at(0); }, error);
            const auto io = measure.finish();
            REQUIRE(io.failures == 1);
            CHECK(ledger.read_cache_stats().allocation_bypass_count == failures);
        }
        {
            bound_io::WriteFault fault(true);
            typed_io([&] { ledger.replace(key, value(key, seed + 23, true)); }, ENOSPC);
            REQUIRE(bound_io::short_writes == 1 && bound_io::write_errors == 2);
        }
        // Cached/resident values cannot hide an unrepairable descriptor write.
        typed_io([&] { (void)ledger.read(key); }, ENOSPC);
        typed_io([&] { TradeLedger poisoned_copy(ledger); }, ENOSPC);
    }
}

// Real fd sink, captured in a separate process so the allocation replacement
// remains single-threaded. Capture/output memory is test-owned, never cache C.
struct WireCapture {
    int writer = -1, storage = -1;
    pid_t child = -1;
    WireCapture() {
        int pair[2]{};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        storage = static_cast<int>(::syscall(SYS_memfd_create, "ledger-wire", MFD_CLOEXEC));
        REQUIRE(storage >= 0);
        writer = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 32);
        ::close(pair[1]);
        REQUIRE(writer >= 0);
        child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            ::close(writer);
            std::array<char, 4096> buffer{};
            for (;;) {
                const auto count = ::read(pair[0], buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) ::_exit(21);
                if (count == 0) ::_exit(0);
                for (ssize_t done = 0; done < count;) {
                    const auto wrote = ::write(storage, buffer.data() + done,
                                               static_cast<Size>(count - done));
                    if (wrote < 0 && errno == EINTR) continue;
                    if (wrote <= 0) ::_exit(22);
                    done += wrote;
                }
            }
        }
        ::close(pair[0]);
    }
    std::string finish() {
        ::close(writer); writer = -1;
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        child = -1;
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        REQUIRE(::lseek(storage, 0, SEEK_SET) == 0);
        std::string bytes;
        std::array<char, 4096> chunk{};
        for (;;) {
            const auto count = ::read(storage, chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR) continue;
            REQUIRE(count >= 0);
            if (count == 0) break;
            bytes.append(chunk.data(), static_cast<Size>(count));
        }
        return bytes;
    }
    ~WireCapture() {
        if (writer >= 0) ::close(writer);
        if (child > 0) { int status; ::waitpid(child, &status, 0); }
        if (storage >= 0) ::close(storage);
    }
};
struct WireHost final : source::PineStrategyHost {
    Size bar = 0, declared = 0, observed_old = 0;
    bool supplied = false;
    Backing* backing = nullptr;
    std::uint64_t seed = 0;
    std::vector<std::uint64_t> hashes;
    WireHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.default_qty_value = 1;
        config.margin_long = config.margin_short = 0;
        config.commission_value = 0;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_trade_history_streamable(true);
        set_syminfo_session("24x7"); set_syminfo_timezone("UTC");
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (bar == 0) {
            // This is RED if canonical native admission ignores the new field.
            REQUIRE(trades_.ledger_cache_max_bytes() == declared);
            if (supplied) Probe::bind(trades_, backing->factory(), 4 * 1024 * 1024);
        }
        for (int i = std::max(0, static_cast<int>(closed_trade_count()) - 8);
             i < static_cast<int>(closed_trade_count()); ++i)
            CHECK(closed_trade_entry_id(i) == trades_.read(static_cast<Size>(i)).entry_id);
        if (closed_trade_count() > static_cast<int>(TradeLedger::kResidentRows)) {
            const auto old = trades_.read(0);
            CHECK(same_bits(closed_trade_profit(0), old.pnl));
            CHECK(closed_trade_entry_id(0) == old.entry_id);
            ++observed_old;
        }
        hashes.push_back(broker_state_hash());
        const double missing = std::numeric_limits<double>::quiet_NaN();
        const std::string comment = std::to_string(seed + bar) + std::string("\0wire", 5);
        strategy_entry(bar % 2 ? "short" : "long", bar % 2 == 0,
                       missing, missing, 1.0, comment);
        ++bar;
    }
};
struct WireResult { std::string bytes; std::vector<std::uint64_t> hashes; double net = 0; };
WireResult wire_case(Size bytes, std::uint64_t seed, bool supplied) {
    Backing backing;
    WireCapture capture;
    WireHost host;
    host.seed = seed; host.declared = bytes;
    host.supplied = supplied; host.backing = &backing;
    CacheDefaults policy;
    policy.ledger_cache_max_bytes = bytes;
    host.set_cache_policy(policy);
    host.set_broker_state_hash_recording(true);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, capture.writer, "ledger-bound-wire"};
    REQUIRE(host.set_report_sink_fd_v1(&sink) == 0);
    std::vector<Bar> tape(3 * TradeLedger::kResidentRows + 13);
    for (Size i = 0; i < tape.size(); ++i) {
        const double price = 100 + ((seed + i) % 31) * 0.125;
        tape[i] = {price, price, price, price, 1, T + static_cast<std::int64_t>(i) * 60000};
    }
    host.run(tape.data(), static_cast<int>(tape.size()), "1", "1");
    REQUIRE(host.last_run_status() == 0 && host.last_error().empty());
    REQUIRE(host.observed_old != 0);
    ReportC report{};
    host.fill_report(&report);
    WireResult result;
    result.net = report.net_profit;
    BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt; receipt.version = 1;
    REQUIRE(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1);
    result.hashes = std::move(host.hashes);
    result.bytes = capture.finish();
    REQUIRE(!result.bytes.empty() && !result.hashes.empty());
    return result;
}
void wire_and_hash(std::uint64_t seed) {
    const auto reference = wire_case(0, seed, false);
    for (const Size bytes : {Size{1}, charge(7) + 13, TradeLedger::kDefaultLedgerCacheMaxBytes})
        for (const bool supplied : {false, true}) {
            const auto actual = wire_case(bytes, seed, supplied);
            CHECK(actual.bytes == reference.bytes);
            CHECK(actual.hashes == reference.hashes);
            CHECK(same_bits(actual.net, reference.net));
        }
    const auto changed = wire_case(charge(7) + 13, seed + 1, true);
    CHECK(changed.bytes != reference.bytes);
    CHECK(changed.hashes != reference.hashes);
}
}

static void run_suite(std::uint64_t seed, Size population) {
    REQUIRE(population > 3 * 1024 + TradeLedger::kResidentRows);
    REQUIRE(population <= static_cast<Size>(std::numeric_limits<int>::max()));
    test("independent allocation and I/O forwarding", forwarding_controls);
    test("declared byte range and legacy precedence", [&] { byte_contract(seed); });
    test("associative collisions and constant cold work",
         [&] { associative_and_cold_cost(seed, population); });
    test("actual allocation charge and steady bound", [&] { allocation_census(seed, population); });
    test("real failures and doubling retry schedule", [&] { allocation_failures(seed); });
    test("finite sticky allocation compatibility", [&] { finite_failure_compatibility(seed); });
    test("string mismatch still exposes late I/O failure",
         [&] { string_mismatch_keeps_late_io_failure(seed); });
    test("retained backing, strings, rollback and poison",
         [&] { supplied_backing_and_transactions(seed); });
    test("retirement, recut, detached and legacy owning values",
         [&] { retirement_rewind_and_materialization(seed); });
    test("owning output failure is not optional bypass", [&] { owning_return_failure(seed); });
    test("complete fd wire and broker hashes", [&] { wire_and_hash(seed); });
}

int main(int argc, char** argv) {
    // Seed and population are inputs, not baked results. With arguments one seed
    // and one population run alone (larger 1x/4x/16x populations belong on
    // admitted VMs); the registered row has no arguments and runs three
    // independent seeded populations in turn.
    if (argc > 1) {
        run_suite(std::stoull(argv[1]), argc > 2 ? std::stoull(argv[2]) : 16 * 256 + 19);
    } else {
        run_suite(UINT64_C(548), 16 * 256 + 19);
        run_suite(UINT64_C(17), 8263);
        run_suite(UINT64_C(65537), 16411);
    }
    std::printf("test_ledger_cache_bound: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
