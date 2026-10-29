// Compile this same workload with PINEFORGE_CLOSED_TEXT_READER=0 for the
// strict owning-reader experiment, or =1 for the normal comparison reader.
// Mutants are separate compilations, never runtime selections.
#include <pineforge/native_host.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include "global_allocation_replacement.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifndef PINEFORGE_CLOSED_TEXT_READER
#define PINEFORGE_CLOSED_TEXT_READER 1
#endif
#ifndef PINEFORGE_CLOSED_TEXT_MUTANT
#define PINEFORGE_CLOSED_TEXT_MUTANT 0
#endif
#ifndef PINEFORGE_CLOSED_TEXT_MUTANT_FIELD
#define PINEFORGE_CLOSED_TEXT_MUTANT_FIELD -1
#endif
#if PINEFORGE_CLOSED_TEXT_READER != 0 && PINEFORGE_CLOSED_TEXT_READER != 1
#error "Select reader 0 (owning) or 1 (comparison)"
#endif
#if PINEFORGE_CLOSED_TEXT_MUTANT < 0 || PINEFORGE_CLOSED_TEXT_MUTANT > 3
#error "Select mutant 0 (none), 1 (owning), 2 (constant), or 3 (NUL truncation)"
#endif
#if PINEFORGE_CLOSED_TEXT_READER == 0 && PINEFORGE_CLOSED_TEXT_MUTANT != 0
#error "Mutants require the comparison reader"
#endif
#if PINEFORGE_CLOSED_TEXT_MUTANT_FIELD < -1 || PINEFORGE_CLOSED_TEXT_MUTANT_FIELD > 3
#error "Select all fields (-1), entry id (0), exit id (1), or comments (2,3)"
#endif

namespace text_reads {
inline bool measuring = false;
inline std::uint64_t reads = 0, writes = 0, written_bytes = 0;
}

#if defined(__linux__)
// Forward to the real kernel operation, including during the control.
extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    if (text_reads::measuring) ++text_reads::reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}
extern "C" ssize_t pwrite(int fd, const void* data, std::size_t length, off_t offset) {
    const auto result =
        static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, length, offset));
    if (text_reads::measuring) {
        ++text_reads::writes;
        if (result > 0) text_reads::written_bytes += static_cast<std::uint64_t>(result);
    }
    return result;
}
#endif

namespace {
using namespace pineforge;
using Size = TradeLedger::size_type;
using View = std::string_view;
constexpr Size kIterations = 25000;
constexpr Size kPopulation = 25000;
constexpr Size kChunk = detail::ScratchFile::kCacheBytes;
constexpr Size kTail = TradeLedger::kResidentRows * TradeLedger::kRecordBytes;
constexpr Size ceil_div(Size n, Size d) { return n / d + (n % d != 0); }
constexpr Size cache_capacity(Size population) {
    Size capacity = TradeLedger::kResidentRows;
    while (capacity < population) capacity *= 2;
    return capacity < TradeLedger::kDefaultCacheRows
        ? capacity : TradeLedger::kDefaultCacheRows;
}
constexpr Size kCapacity = cache_capacity(kPopulation);
static_assert(kCapacity == 32768 && kPopulation <= kCapacity,
              "C=min(65536,next_power_of_two(max(64,25000)))=32768");

enum class Field { EntryId, ExitId, EntryComment, ExitComment };
constexpr const char* field_name(Field field) {
    switch (field) {
    case Field::EntryId: return "entry_id";
    case Field::ExitId: return "exit_id";
    case Field::EntryComment: return "entry_comment";
    case Field::ExitComment: return "exit_comment";
    }
    return "invalid";
}

Size checks = 0, failures = 0;
struct Stop {};
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, expression);
    }
}
#define EXPECT(expression) check(bool(expression), #expression, __LINE__)
#define REQUIRE(expression) do { \
    const bool ok = bool(expression); \
    check(ok, #expression, __LINE__); \
    if (!ok) throw Stop{}; \
} while (false)

// Keep checking every iteration, but print only the first failure of each
// invariant per pass. Diagnostics happen after both measurement snapshots.
struct PassCheck {
    bool printed = false;
    void observe(bool ok, const char* expression, int line, const char* row,
                 Field field, Size iteration, std::uint64_t actual,
                 std::uint64_t expected) {
        ++checks;
        if (ok) return;
        ++failures;
        if (printed) return;
        printed = true;
        std::printf("FAIL %s:%d: %s row=%s field=%s iteration=%zu "
                    "observed=%llu required=%llu\n",
                    __FILE__, line, expression, row, field_name(field), iteration,
                    static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(expected));
    }
};
#define OBSERVE(observer, expression, actual, expected) \
    observer.observe(bool(expression), #expression, __LINE__, label, F, i, actual, expected)

template<Field F, class T> auto& field(T& trade) {
    if constexpr (F == Field::EntryId) return trade.entry_id;
    if constexpr (F == Field::ExitId) return trade.exit_id;
    if constexpr (F == Field::EntryComment) return trade.entry_comment;
    if constexpr (F == Field::ExitComment) return trade.exit_comment;
}
Trade tiny_trade() {
    Trade trade{};
    trade.entry_id = "e";
    trade.exit_id = "x";
    trade.entry_comment = "c";
    trade.exit_comment = "d";
    return trade;
}

// Only this adapter block changes between compilations. Neither the
// workloads nor their expected values depend on the selected reader.
struct Reader : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    TradeLedger& ledger() { return trades_; }
    const TradeLedger& ledger() const { return trades_; }

    template<Field F> std::string owning(int index) const {
        if constexpr (F == Field::EntryId) return closed_trade_entry_id(index);
        if constexpr (F == Field::ExitId) return closed_trade_exit_id(index);
        if constexpr (F == Field::EntryComment) return closed_trade_entry_comment(index);
        if constexpr (F == Field::ExitComment) return closed_trade_exit_comment(index);
    }
    template<Field F> bool equals(int index, View rhs) const {
#if PINEFORGE_CLOSED_TEXT_READER == 0
        const auto value = owning<F>(index);
        return View(value.data(), value.size()) == rhs;
#else
#if PINEFORGE_CLOSED_TEXT_MUTANT != 0
        constexpr bool inject = PINEFORGE_CLOSED_TEXT_MUTANT_FIELD == -1
            || PINEFORGE_CLOSED_TEXT_MUTANT_FIELD == static_cast<int>(F);
#endif
#if PINEFORGE_CLOSED_TEXT_MUTANT == 1
        if constexpr (inject) {
            const auto value = owning<F>(index);
            return View(value.data(), value.size()) == rhs;
        }
#elif PINEFORGE_CLOSED_TEXT_MUTANT == 3
        if constexpr (inject) {
            const auto value = owning<F>(index);
            const View bytes(value.data(), value.size());
            return bytes.substr(0, bytes.find('\0')) == rhs.substr(0, rhs.find('\0'));
        }
#endif
        bool result = false;
        if constexpr (F == Field::EntryId) result = closed_trade_entry_id_equals(index, rhs);
        if constexpr (F == Field::ExitId) result = closed_trade_exit_id_equals(index, rhs);
        if constexpr (F == Field::EntryComment)
            result = closed_trade_entry_comment_equals(index, rhs);
        if constexpr (F == Field::ExitComment)
            result = closed_trade_exit_comment_equals(index, rhs);
#if PINEFORGE_CLOSED_TEXT_MUTANT == 2
        // Still exercise the real operation, including its refusal checks.
        if constexpr (inject) return true;
#endif
        return result;
#endif
    }
};

template<Field F> bool ledger_equals(const TradeLedger& ledger, Size index, View rhs) {
#if PINEFORGE_CLOSED_TEXT_READER == 0
    std::string value;
    if constexpr (F == Field::EntryId) value = ledger.entry_id_at(index);
    if constexpr (F == Field::ExitId) value = ledger.exit_id_at(index);
    if constexpr (F == Field::EntryComment) value = ledger.entry_comment_at(index);
    if constexpr (F == Field::ExitComment) value = ledger.exit_comment_at(index);
    return View(value.data(), value.size()) == rhs;
#else
    if constexpr (F == Field::EntryId) return ledger.entry_id_equals(index, rhs);
    if constexpr (F == Field::ExitId) return ledger.exit_id_equals(index, rhs);
    if constexpr (F == Field::EntryComment) return ledger.entry_comment_equals(index, rhs);
    if constexpr (F == Field::ExitComment) return ledger.exit_comment_equals(index, rhs);
#endif
}

struct Measuring {
    Measuring() { REQUIRE(!text_reads::measuring); text_reads::measuring = true; }
    ~Measuring() { text_reads::measuring = false; }
    void finish() { text_reads::measuring = false; }
};

void controls() {
    // One explicit allocation, even when freed before the second snapshot:
    // allocation_count_delta = 1 successful operator-new call, not live bytes.
    const auto before = global_allocation::allocations;
    void* (*volatile allocate)(std::size_t) = static_cast<void* (*)(std::size_t)>(&::operator new);
    void (*volatile release)(void*) = static_cast<void (*)(void*)>(&::operator delete);
    void* temporary = allocate(kChunk + 1);
    release(temporary);
    EXPECT(global_allocation::allocations - before == 1);

    detail::ScratchFile file;
    file.set_directory(".");
    file.open("pineforge-text-read-control");
    constexpr char bytes[] = "control\0payload";
    std::array<char, sizeof(bytes) - 1> actual{};
    const auto reads_before = text_reads::reads, writes_before = text_reads::writes;
    const auto bytes_before = text_reads::written_bytes;
    Measuring measurement;
    file.write(0, bytes, actual.size());
    file.read(0, actual.data(), actual.size());
    measurement.finish();
    EXPECT(std::memcmp(actual.data(), bytes, actual.size()) == 0);
#if defined(__linux__)
    // Unbounded control file, one write and one read of 7+1+7=15 < W=65536 bytes.
    EXPECT(text_reads::reads - reads_before == 1);
    EXPECT(text_reads::writes - writes_before == 1);
    EXPECT(text_reads::written_bytes - bytes_before == actual.size());
#else
    (void)reads_before; (void)writes_before; (void)bytes_before;
#endif
    // Independent control: prove this library's owning 96-byte result needs
    // a counted allocation before interpreting the cache-fit runtime red.
    std::string (*volatile make_owned)(Size) =
        +[](Size size) { return std::string(size, 'q'); };
    const auto strings_before = global_allocation::allocations;
    const std::string owned = make_owned(96);
    EXPECT(owned.size() == 96);
    EXPECT(owned.front() == 'q' && owned.back() == 'q');
    EXPECT(global_allocation::allocations > strings_before);
}

enum class Order { Traverse, RepeatPair };
Size row_index(Order order, Size iteration) {
    if (order == Order::Traverse) return iteration;
    return iteration % 2;
}
struct Sample {
    Size count = 0, allocations_before = 0, allocations_after = 0;
    std::uint64_t reads_before = 0, reads_after = 0;
    bool actual = false;
};
struct IoBounds {
    Size length = 0;
    bool cached = true;
};

template<Field F, bool EngineCount>
void measured_pass(Reader& host, const std::vector<std::string>& oracle,
                   View rhs, const char* label, Size iterations,
                   Size population, Order order, IoBounds io, Size first_index = 0) {
    REQUIRE(!oracle.empty());
    REQUIRE(order != Order::Traverse || iterations == oracle.size());
    REQUIRE(order != Order::RepeatPair || oracle.size() >= 2);
    std::vector<Sample> samples(iterations);
    std::vector<unsigned char> expected(iterations);
    for (Size i = 0; i < iterations; ++i) {
        const auto& bytes = oracle[row_index(order, i)];
        // The input vector is built without reading any engine value.
        expected[i] = View(bytes.data(), bytes.size()) == rhs;
        if (!io.cached) REQUIRE(bytes.size() == io.length);
    }
    const auto allocations_before = global_allocation::allocations;
    const auto reads_before = text_reads::reads, writes_before = text_reads::writes;
    Measuring measurement;
    for (Size i = 0; i < iterations; ++i) {
        auto& sample = samples[i];
        const Size index = first_index + row_index(order, i);
        sample.allocations_before = global_allocation::allocations;
        sample.reads_before = text_reads::reads;
        if constexpr (EngineCount) sample.count = host.closed_trade_count();
        else sample.count = host.ledger().size();
        sample.actual = host.equals<F>(static_cast<int>(index), rhs);
        sample.reads_after = text_reads::reads;
        sample.allocations_after = global_allocation::allocations;
    }
    measurement.finish();
    const auto allocations_after = global_allocation::allocations;
    const auto reads_after = text_reads::reads, writes_after = text_reads::writes;
    std::printf("MEASURE row=%s field=%s count_surface=%s iterations=%zu alloc_delta=%zu",
                label, field_name(F), EngineCount ? "engine" : "ledger", iterations,
                allocations_after - allocations_before);
#if defined(__linux__)
    std::printf(" pread_delta=%llu\n",
                static_cast<unsigned long long>(reads_after - reads_before));
#else
    std::printf(" pread_delta=unobserved\n");
#endif

    PassCheck allocation_check, count_check, oracle_check, read_check, upper_check;
    const Size chunks = ceil_div(io.length, kChunk);
    const Size individual_max = chunks + ceil_div(io.length, kChunk) + 1;
    for (Size i = 0; i < iterations; ++i) {
        const auto& sample = samples[i];
        // Warm cache bytes or fixed stack chunks, a bool and a scalar count
        // require no heap owner: A_iteration=0, including new/delete temporaries.
        OBSERVE(allocation_check, sample.allocations_after == sample.allocations_before,
                sample.allocations_after - sample.allocations_before, 0);
        // Exact global end: P appended rows (25000 in the full population),
        // including any explicitly appended trailing-pressure row in small sets.
        OBSERVE(count_check, sample.count == population, sample.count, population);
        OBSERVE(oracle_check, sample.actual == bool(expected[i]), sample.actual, expected[i]);
#if defined(__linux__)
        const auto reads = sample.reads_after - sample.reads_before;
        if (io.cached || io.length == 0) {
            // Full population: P=25000 <= C=32768; contiguous [0,P) keys
            // have no collisions after warming, so misses=0 => R_iteration=0.
            // Empty uncached extents and extents wholly in the fixed payload
            // tail (including its wrap row) also require zero payload syscalls.
            OBSERVE(read_check, sample.reads_after == sample.reads_before, reads, 0);
        } else {
            // Full extent consumed: K=ceil(L/B), B=W=65536. No injected
            // short reads/EINTR. Running W-byte budget can split each read:
            // K <= R_iteration <= K+ceil(L/W)+1, with L=io.length.
            OBSERVE(read_check, reads >= chunks, reads, chunks);
            OBSERVE(upper_check, reads <= individual_max, reads, individual_max);
        }
#endif
    }
    // A_total=iterations*A_iteration=iterations*0=0; no average or tolerance.
    {
        PassCheck total_allocation;
        const Size i = iterations;
        OBSERVE(total_allocation, allocations_after == allocations_before,
                allocations_after - allocations_before, 0);
    }
#if defined(__linux__)
    if (io.cached || io.length == 0) {
        // R_total=iterations*0=0 for warmed, empty or tail-resident fields.
        PassCheck total_reads;
        const Size i = iterations;
        OBSERVE(total_reads, reads_after == reads_before, reads_after - reads_before, 0);
    } else {
        const Size minimum = iterations * chunks;
        const Size maximum = minimum + ceil_div(iterations * io.length, kChunk) + 1;
        PassCheck lower_total, upper_total;
        const Size i = iterations;
        // T=iterations*L, K=ceil(L/B), W=B=65536, arbitrary initial budget
        // phase: iterations*K <= R_total <= iterations*K+ceil(T/W)+1.
        OBSERVE(lower_total, reads_after - reads_before >= minimum,
                reads_after - reads_before, minimum);
        OBSERVE(upper_total, reads_after - reads_before <= maximum,
                reads_after - reads_before, maximum);
    }
    // Neither count nor equality writes: writes_total=iterations*0=0.
    {
        PassCheck total_writes;
        const Size i = iterations;
        OBSERVE(total_writes, writes_after == writes_before, writes_after - writes_before, 0);
    }
#else
    (void)reads_before; (void)reads_after; (void)writes_before; (void)writes_after;
    (void)chunks; (void)individual_max;
#endif
}

template<Field F> void append_inputs(Reader& host, const std::vector<std::string>& oracle) {
    host.ledger().set_scratch_directory(".");
    for (const auto& bytes : oracle) {
        auto trade = tiny_trade();
        field<F>(trade) = bytes;
        host.ledger().push_back(trade);
    }
}
void warm(TradeLedger& ledger, Size rows) {
    for (Size i = 0; i < rows; ++i) (void)ledger.pnl_at(i);
}
template<Field F> constexpr View short_literal() {
    if constexpr (F == Field::EntryId) return "entry-short";
    if constexpr (F == Field::ExitId) return "exit--short";
    if constexpr (F == Field::EntryComment) return "entry-note";
    if constexpr (F == Field::ExitComment) return "exit--note";
}
constexpr char kLongLiteral[] =
    "01234567" "89abcdef" "01234567" "89abcdef"
    "01234567" "89abcdef" "01234567" "89abcdef"
    "01234567" "89abcdef" "01234567" "89abcdef";
static_assert(sizeof(kLongLiteral) - 1 == 12 * 8, "twelve eight-byte pieces");

template<Field F> void cache_fit_population(View rhs, const char* label) {
    Reader host;
    std::vector<std::string> oracle;
    oracle.reserve(kPopulation);
    for (Size i = 0; i < kPopulation; ++i) {
        oracle.emplace_back(rhs.data(), rhs.size());
        if (i % 2) oracle.back().back() = 'X';
    }
    append_inputs<F>(host, oracle);
    REQUIRE(rhs.size() + 3 <= TradeLedger::kCacheStringBytes); // 96+1+1+1=99<=128.
    warm(host.ledger(), kPopulation);
    // min(65536,next_power_of_two(max(64,25000)))=32768 entries;
    // 32768*(168+2*sizeof(size_type)+128)=10,223,616 bytes on 64-bit.
    const Size fixed = TradeLedger::resident_cache_bytes(1) - TradeLedger::kLedgerEntryBytes;
    EXPECT(host.ledger().cache_resident_bytes() == fixed + kCapacity * TradeLedger::kLedgerEntryBytes);
    // All P contiguous keys occupy distinct slots, so occupied entries=P.
    EXPECT(host.ledger().cache_size() == kPopulation);
    measured_pass<F, false>(host, oracle, rhs, label, kIterations,
                            kPopulation, Order::Traverse, {});
    measured_pass<F, true>(host, oracle, rhs, label, kIterations,
                           kPopulation, Order::RepeatPair, {});
}

template<Field F> void byte_subrows() {
    Reader host;
    const auto literal = short_literal<F>();
    std::vector<std::string> oracle{
        "", std::string(literal), std::string(literal),
        std::string(literal.substr(1)), std::string("ab\0cd", 5),
        std::string("ab\0ce", 5)};
    oracle[2].back() = 'X';
    append_inputs<F>(host, oracle);
    warm(host.ledger(), oracle.size());
    struct Comparison { View rhs; const char* label; };
    const std::array<Comparison, 5> comparisons{{
        {View{}, "empty-rhs"}, {literal, "short-rhs"},
        {literal.substr(1), "unequal-length-rhs"},
        {View("ab\0cd", 5), "embedded-nul"}, {View("ab", 2), "nul-prefix-rhs"}}};
    for (const auto& comparison : comparisons) {
        // Six independent byte rows, each visited once per pass.
        measured_pass<F, false>(host, oracle, comparison.rhs, comparison.label,
                                oracle.size(), oracle.size(), Order::Traverse, {});
    }
}

template<Field F> void uncached_population(Size length, bool other_field_wide,
                                          Size iterations, const char* label,
                                          bool chunk_boundary_nul = false) {
    Reader host;
    std::string rhs(length, 'r');
    if (chunk_boundary_nul) {
        REQUIRE(length > kChunk);
        // A two-byte NUL run straddles the first chunk boundary.
        rhs[kChunk - 1] = '\0';
        rhs[kChunk] = '\0';
    }
    std::vector<std::string> oracle{rhs, rhs};
    if (length != 0) oracle[1].back() = 'X';
    host.ledger().set_scratch_directory(".");
    for (const auto& bytes : oracle) {
        auto trade = tiny_trade();
        field<F>(trade) = bytes;
        if (other_field_wide) {
            // Combined allowance is 128; another field is 128+1=129,
            // so even a short/empty requested field must use the fallback.
            if constexpr (F == Field::ExitComment)
                trade.entry_comment.assign(TradeLedger::kCacheStringBytes + 1, 'w');
            else
                trade.exit_comment.assign(TradeLedger::kCacheStringBytes + 1, 'w');
        }
        host.ledger().push_back(trade);
    }
    // Trailing payload H=64*168+1=10753 > tail=10752 entirely displaces
    // both old target extents. Population is two targets plus this guard.
    auto pressure = tiny_trade();
    pressure.exit_comment.assign(kTail + 1, 'p');
    host.ledger().push_back(pressure);
    const Size population = oracle.size() + 1;
    warm(host.ledger(), population);
    measured_pass<F, true>(host, oracle, rhs, label, iterations,
                           population, Order::RepeatPair, {length, false});
    if (chunk_boundary_nul) {
        // Independent RHS changes on both sides of the boundary, at the last
        // byte, and in length. Every fallback operation must consume all L.
        std::array<std::string, 4> alternatives{rhs, rhs, rhs, rhs};
        alternatives[0][kChunk - 2] = 's';
        alternatives[1][kChunk + 1] = 's';
        alternatives[2].back() = 's';
        alternatives[3].resize(kChunk - 1);
        constexpr std::array<const char*, 4> labels{
            "mismatch-before-chunk", "mismatch-after-chunk",
            "mismatch-last-byte", "unequal-length-chunk"};
        for (Size index = 0; index < alternatives.size(); ++index)
            measured_pass<F, false>(host, oracle, alternatives[index], labels[index],
                                    oracle.size(), population, Order::Traverse, {length, false});
    }
}

template<Field F> void wrapped_tail() {
    Reader host;
    host.ledger().set_scratch_directory(".");
    auto prefix = tiny_trade();
    // Total prefix payload=H-128: three one-byte fields plus this padding.
    // The next 257-byte requested field starts at H-128+[0,3], so it crosses
    // H=64*168=10752 regardless of which of the four fields precede it.
    prefix.exit_comment.assign(kTail - TradeLedger::kCacheStringBytes - 3, 'p');
    host.ledger().push_back(prefix);
    std::string rhs(2 * TradeLedger::kCacheStringBytes + 1, 't');
    // Boundary-relative offset is 128-q for q in [0,3]. NULs at [124,128]
    // cover the byte immediately before and after that edge for every field.
    for (Size offset = TradeLedger::kCacheStringBytes - 4;
         offset <= TradeLedger::kCacheStringBytes; ++offset) rhs[offset] = '\0';
    std::vector<std::string> oracle{rhs, rhs};
    oracle[1].back() = 'X';
    for (const auto& bytes : oracle) {
        auto trade = tiny_trade();
        field<F>(trade) = bytes;
        host.ledger().push_back(trade);
    }
    // The last two rows contain 2*(257+3)=520 < H bytes. Their requested
    // extents are wholly in the tail, one wrapping its physical ring edge.
    // Combined row width=260>128: comparison must use the uncached path.
    const Size population = 1 + oracle.size();
    warm(host.ledger(), population);
    measured_pass<F, false>(host, oracle, rhs, "wrapped-tail-257", 2 * 4,
                            population, Order::RepeatPair, {}, 1);
}

template<Field F> void field_workloads() {
    cache_fit_population<F>(short_literal<F>(), "short-literal");
    cache_fit_population<F>(View(kLongLiteral, sizeof(kLongLiteral) - 1), "cache-fit-96");
    byte_subrows<F>();
    uncached_population<F>(short_literal<F>().size(), true, kIterations, "other-field-129");
    uncached_population<F>(0, true, kIterations, "empty-uncached");
    // L=2*128+1=257, K=ceil(257/65536)=1; two target rows, N=25000.
    uncached_population<F>(2 * TradeLedger::kCacheStringBytes + 1, false,
                          kIterations, "requested-257");
    // Small functional sets only: eight operations=two rows*four repeats.
    uncached_population<F>(kTail + 1, false, 2 * 4, "beyond-tail-10753");
    uncached_population<F>(2 * kChunk + 1, false, 2 * 4, "three-chunk-131073", true);
    wrapped_tail<F>();
}

void exact_count_at(const TradeLedger& ledger, Size expected, int line) {
    // expected is the independently tracked global end, never size-base.
    const Size actual = ledger.size();
    check(actual == expected, "ledger.size() == expected", line);
    if (actual != expected)
        std::printf("count observed=%zu required=%zu\n", actual, expected);
    check(ledger.empty() == (expected == 0), "ledger.empty() == (expected == 0)", line);
}
#define exact_count(ledger, expected) exact_count_at(ledger, expected, __LINE__)
template<Field F> void check_field(const Reader& host, Size index, const Trade& expected) {
    // The expected Trade is fixture input; no read result feeds this oracle.
    const auto& bytes = field<F>(expected);
    const View rhs(bytes.data(), bytes.size());
    EXPECT(host.equals<F>(static_cast<int>(index), rhs));
    EXPECT(ledger_equals<F>(host.ledger(), index, rhs));
}
void check_row(const Reader& host, Size index, const Trade& expected) {
    check_field<Field::EntryId>(host, index, expected);
    check_field<Field::ExitId>(host, index, expected);
    check_field<Field::EntryComment>(host, index, expected);
    check_field<Field::ExitComment>(host, index, expected);
}
void check_ledger_row(const TradeLedger& ledger, Size index, const Trade& expected) {
    EXPECT(ledger_equals<Field::EntryId>(ledger, index, expected.entry_id));
    EXPECT(ledger_equals<Field::ExitId>(ledger, index, expected.exit_id));
    EXPECT(ledger_equals<Field::EntryComment>(ledger, index, expected.entry_comment));
    EXPECT(ledger_equals<Field::ExitComment>(ledger, index, expected.exit_comment));
}

template<class Action> void invariant(Action action, bool retired = false) {
    bool caught = false;
    try { action(); }
    catch (const std::logic_error& error) {
        caught = true;
        EXPECT(dynamic_cast<const RunFailureInfo*>(&error) != nullptr);
        const auto failure = classify_run_failure(error);
        EXPECT(failure.code == RunFailureCode::engine_invariant);
        if (retired) EXPECT(!failure.args || *failure.args == "{}");
    }
    EXPECT(caught);
}
template<class Action> void fails_allocation(Action action) {
    const auto refused_before = global_allocation::refused;
    bool caught = false;
    global_allocation::fail_after = 0;
    try { action(); }
    catch (const std::bad_alloc&) { caught = true; }
    catch (...) {
        global_allocation::fail_after = std::numeric_limits<Size>::max();
        throw;
    }
    global_allocation::fail_after = std::numeric_limits<Size>::max();
    EXPECT(caught);
    // Exactly one injected refusal, fail_after=0 disarms on its first call.
    EXPECT(global_allocation::refused - refused_before == 1);
}

void mutations_and_counts() {
    Reader host;
    auto& ledger = host.ledger();
    exact_count(ledger, 0); // Default construction: zero appends.
    ledger.set_scratch_directory(".");
    exact_count(ledger, 0); // Directory changes preserve the end.
    ledger.set_cache_rows(TradeLedger::kDefaultCacheRows);
    exact_count(ledger, 0);
    ledger.reserve(TradeLedger::kResidentRows + 1);
    exact_count(ledger, 0); // reserve changes no row count.
    const Trade first = tiny_trade();
    ledger.push_back(first); // const-reference overload.
    exact_count(ledger, 1); // 0+1.
    ledger.push_back(tiny_trade()); // rvalue overload.
    exact_count(ledger, 2); // 1+1.
    check_row(host, 0, first); // Allocate the initial 64-entry cache.
    Size end = 2;
    while (end < TradeLedger::kResidentRows + 1) {
        ledger.push_back(first);
        ++end;
        exact_count(ledger, end); // Previous independent end+one append.
    }
    check_row(host, 0, first); // Cache grows across 64 to 128 entries.
    exact_count(ledger, end);
    auto replaced = first;
    replaced.entry_id = "replace-id";
    replaced.exit_id = "replace-exit";
    replaced.entry_comment = std::string("new\0entry", 9);
    replaced.exit_comment = "replace-note";
    ledger.replace(0, replaced);
    exact_count(ledger, end); // replace changes 0 counts.
    check_row(host, 0, replaced);
    EXPECT(!host.equals<Field::EntryId>(0, first.entry_id));
    auto modified = replaced;
    modified.entry_id = "modify-id";
    modified.exit_comment = std::string("new\0exit", 8);
    ledger.modify(1, [&](Trade& trade) { trade = modified; });
    exact_count(ledger, end); // modify changes 0 counts.
    check_row(host, 1, modified);
    ledger.resize(1);
    exact_count(ledger, 1); // Shrink 65 -> 1, crossing the descriptor tail.
    check_row(host, 0, replaced);
    modified.entry_id = "append-id";
    modified.exit_id = "append-exit";
    modified.entry_comment = std::string("append\0entry", 12);
    modified.exit_comment = "append-note";
    ledger.push_back(modified);
    exact_count(ledger, 2); // Reappend at global key 1 must invalidate old data.
    check_row(host, 1, modified);
    ledger.resize(5);
    exact_count(ledger, 5); // Grow 2+3 default rows.
    check_row(host, 2, Trade{});
    check_row(host, 4, Trade{});
    ledger.seal(1);
    exact_count(ledger, 5); // seal preserves global end, even below base.

    TradeLedger copied(ledger);
    exact_count(copied, 5); // Copy constructor appends five retained rows.
    EXPECT(copied.base() == 1);
    check_ledger_row(copied, 1, modified);
    TradeLedger assigned;
    assigned.push_back(first);
    exact_count(assigned, 1);
    assigned = copied;
    exact_count(assigned, 5); // Copy assignment swaps both State and end.
    check_ledger_row(assigned, 1, modified);
    auto* same = &assigned;
    assigned = *same;
    exact_count(assigned, 5); // Self-copy is unchanged.
    check_ledger_row(assigned, 1, modified);
    TradeLedger moved(std::move(copied));
    exact_count(moved, 5);
    exact_count(copied, 0); // Move constructor leaves a valid empty source.
    check_ledger_row(moved, 1, modified);
    copied.push_back(first);
    exact_count(copied, 1);
    copied.clear();
    exact_count(copied, 0);
    assigned.resize(2);
    exact_count(assigned, 2); // Distinct destination count before move assignment.
    assigned = std::move(moved);
    exact_count(assigned, 5);
    exact_count(moved, 0); // Move assignment leaves a valid empty source.
    check_ledger_row(assigned, 1, modified);
    same = &assigned;
    assigned = std::move(*same);
    exact_count(assigned, 5);
    check_ledger_row(assigned, 1, modified);
    ledger.seal(ledger.size());
    exact_count(ledger, 5); // base==end==5 is nonempty, not end-base==0.
    EXPECT(ledger.base() == 5);
    ledger.clear();
    exact_count(ledger, 0);
    EXPECT(ledger.cache_rows() == TradeLedger::kDefaultCacheRows);
    ledger.push_back(first);
    exact_count(ledger, 1);
    check_row(host, 0, first);

    // State construction is the first allocation for copy and clear; failure
    // must preserve both the old State and its exact mirrored global end.
    fails_allocation([&] { TradeLedger unsuccessful(assigned); });
    exact_count(assigned, 5);
    fails_allocation([&] { ledger = assigned; });
    exact_count(ledger, 1);
    check_row(host, 0, first);
    fails_allocation([&] { ledger.clear(); });
    exact_count(ledger, 1);
    check_row(host, 0, first);
    Reader empty;
    empty.ledger().set_scratch_directory(".");
    auto wide = first;
    wide.exit_comment.assign(kChunk + 1, 'w');
    // Fresh file-backed payload setup must allocate before append commits.
    fails_allocation([&] { empty.ledger().push_back(wide); });
    exact_count(empty.ledger(), 0);
    empty.ledger().push_back(wide);
    exact_count(empty.ledger(), 1);
    check_row(empty, 0, wide);
}

void collisions() {
    for (const Size capacity : {Size{1}, Size{3}}) {
        Reader host;
        auto& ledger = host.ledger();
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(capacity);
        // Keys 0,cap,2*cap have the same direct-map slot, including cap=3
        // (modulo rather than a power-of-two mask). Population=2*cap+1.
        std::vector<Trade> oracle(2 * capacity + 1, tiny_trade());
        for (Size i = 0; i < oracle.size(); ++i) {
            oracle[i].entry_id += static_cast<char>('a' + i);
            oracle[i].exit_id += static_cast<char>('a' + i);
            oracle[i].entry_comment += static_cast<char>('a' + i);
            oracle[i].exit_comment += static_cast<char>('a' + i);
            ledger.push_back(oracle[i]);
            exact_count(ledger, i + 1);
        }
        for (Size round = 0; round < 8; ++round)
            for (const Size index : {Size{0}, capacity, 2 * capacity})
                check_row(host, index, oracle[index]);
        const Size fixed = TradeLedger::resident_cache_bytes(1) - TradeLedger::kLedgerEntryBytes;
        // Allocated capacity=min(cap,next_power_of_two(max(64,P)))=cap.
        EXPECT(ledger.cache_resident_bytes() == fixed + capacity * TradeLedger::kLedgerEntryBytes);
        exact_count(ledger, oracle.size());
    }
}
} // namespace

// Existing friend seam: enable the same policy used by no-history sink runs.
namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void retire(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
}

namespace {
template<Field F> void boundaries(Reader& host, bool retired, const Trade& live) {
    const auto& bytes = field<F>(live);
    // Three appended rows, seal(1): base-1=0, base=1, end=3.
    invariant([&] { (void)host.equals<F>(0, View{}); }, retired);
    invariant([&] { (void)ledger_equals<F>(host.ledger(), 0, View{}); }, retired);
    EXPECT(host.equals<F>(1, bytes));
    EXPECT(ledger_equals<F>(host.ledger(), 1, bytes));
    invariant([&] { (void)ledger_equals<F>(host.ledger(), 3, View{}); });
    // The engine's ordinary invalid indices retain empty-string semantics.
    EXPECT(host.equals<F>(-1, View{}));
    EXPECT(!host.equals<F>(-1, "x"));
    EXPECT(host.equals<F>(3, View{}));
    EXPECT(!host.equals<F>(3, "x"));
    if (retired) {
        // Nonempty RHS must refuse too; cached empty bytes cannot bypass check.
        invariant([&] { (void)host.equals<F>(0, "x"); }, true);
    }
}

template<Field F> void fully_published(Reader& host, bool retired) {
    // Three appends followed by seal(3): base=end=3, base-1=2.
    invariant([&] { (void)host.equals<F>(2, View{}); }, retired);
    invariant([&] { (void)ledger_equals<F>(host.ledger(), 2, View{}); }, retired);
    invariant([&] { (void)ledger_equals<F>(host.ledger(), 3, View{}); });
    EXPECT(host.equals<F>(3, View{}));
    EXPECT(!host.equals<F>(3, "x"));
}

void refusals_and_retiring_copies() {
    for (const bool retiring : {false, true}) {
        Reader host;
        auto& ledger = host.ledger();
        ledger.set_scratch_directory(".");
        if (retiring) detail::TradeLedgerStorageProbe::retire(ledger);
        exact_count(ledger, 0);
        ledger.push_back(Trade{}); // Every cached field on the retired row is empty.
        exact_count(ledger, 1);
        check_row(host, 0, Trade{}); // Warm it before retirement/publication.
        const auto live = tiny_trade();
        ledger.push_back(live);
        exact_count(ledger, 2);
        ledger.push_back(live);
        exact_count(ledger, 3);
        ledger.seal(1);
        exact_count(ledger, 3);
        EXPECT(ledger.base() == 1);
        boundaries<Field::EntryId>(host, retiring, live);
        boundaries<Field::ExitId>(host, retiring, live);
        boundaries<Field::EntryComment>(host, retiring, live);
        boundaries<Field::ExitComment>(host, retiring, live);
        TradeLedger copied(ledger);
        exact_count(copied, 3); // Retiring seed base=1, plus two successful appends.
        EXPECT(copied.base() == 1);
        check_ledger_row(copied, 1, live);
        ledger.seal(3);
        exact_count(ledger, 3); // Entire window published; global end remains 3.
        EXPECT(ledger.base() == ledger.size());
        fully_published<Field::EntryId>(host, retiring);
        fully_published<Field::ExitId>(host, retiring);
        fully_published<Field::EntryComment>(host, retiring);
        fully_published<Field::ExitComment>(host, retiring);
        TradeLedger empty_window(ledger);
        exact_count(empty_window, 3); // Retiring copy seed=base=3, zero appends.
        EXPECT(empty_window.base() == 3);
        TradeLedger assigned;
        assigned = empty_window;
        exact_count(assigned, 3);
        TradeLedger moved(std::move(empty_window));
        exact_count(moved, 3);
        exact_count(empty_window, 0);
        assigned = std::move(moved);
        exact_count(assigned, 3);
        exact_count(moved, 0);
        ledger.clear();
        exact_count(ledger, 0);
    }
}

template<class Action> void run(const char* label, Action action) {
    try { action(); }
    catch (const Stop&) {}
    catch (const std::exception& error) {
        text_reads::measuring = false;
        ++failures;
        std::printf("FAIL %s exception: %s\n", label, error.what());
    }
}
} // namespace

int main() {
    run("controls", controls);
    run("entry id", field_workloads<Field::EntryId>);
    run("exit id", field_workloads<Field::ExitId>);
    run("entry comment", field_workloads<Field::EntryComment>);
    run("exit comment", field_workloads<Field::ExitComment>);
    run("mutations and counts", mutations_and_counts);
    run("collisions", collisions);
    run("refusals and retiring copies", refusals_and_retiring_copies);
    std::printf("test_closed_trade_text_reads: reader=%d mutant=%d "
                "%zu checks, %zu failures\n",
                PINEFORGE_CLOSED_TEXT_READER, PINEFORGE_CLOSED_TEXT_MUTANT,
                checks, failures);
    return failures ? 1 : 0;
}
