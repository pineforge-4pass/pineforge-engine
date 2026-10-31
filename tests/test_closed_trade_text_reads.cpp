// Compile this same workload with PINEFORGE_CLOSED_TEXT_READER=0 for the
// strict owning-reader experiment, or =1 for the normal comparison reader.
// Mutants are separate compilations, never runtime selections.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include "global_allocation_replacement.hpp"
#include "../src/retired_trade_history_read.hpp"

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
constexpr Size kPageSlots = 256;
constexpr Size kPageBytes = kPageSlots * TradeLedger::kLedgerEntryBytes;
constexpr Size kPages = ceil_div(kPopulation, kPageSlots);
constexpr Size kTableBytes = ceil_div(TradeLedger::kDefaultCacheRows, kPageSlots) * sizeof(void*);
static_assert(kPages == 98 && kPopulation <= TradeLedger::kDefaultCacheRows,
              "25000 contiguous distinct slots touch ceil(25000/256)=98 pages");

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
// Warm both the numeric and text surfaces before measurement. The cache must
// already contain the four packed strings before asserting zero payload reads.
void warm(TradeLedger& ledger, Size rows) {
    for (Size i = 0; i < rows; ++i) {
        (void)ledger.pnl_at(i);
        (void)ledger.entry_id_at(i);
    }
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
    // ceil(25000/256)=98 pages, each 256*312=79,872 bytes, plus
    // ceil(65536/256) owning pointers. Full-cap allocation fails this equality.
    const Size fixed = TradeLedger::resident_cache_bytes(1) - TradeLedger::kLedgerEntryBytes - sizeof(void*);
    EXPECT(host.ledger().cache_resident_bytes() == fixed + kTableBytes + kPages * kPageBytes);
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

void page_allocation_failures_and_growth_hits() {
    const Trade first = tiny_trade();
    const auto begin_diagnostics_run = [](Reader& reader, std::uint64_t run_number = 1) {
        NativeRunSpec spec;
        spec.identity = {"ledger-cache-diagnostics", run_number};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:CACHE";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000;
        spec.point_value = spec.account_fx = 1;
        spec.price_tick = .01;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        REQUIRE(reader.configure_native(spec).status == NativeSetupStatus::Applied);
        const Bar bar{100, 100, 100, 100, 1, INT64_C(1704067200000)};
        reader.run(&bar, 1);
        REQUIRE(reader.last_run_status() == 0 && reader.last_error().empty());
        ReportC report{};
        reader.fill_report(&report);
        BacktestEngine::free_report(&report);
    };
    const auto reported = [](Reader& reader, std::uint64_t expected_bypasses) {
        std::uint64_t bytes = 0, bypasses = 0;
        const auto handle = static_cast<pf_strategy_t>(
            static_cast<BacktestEngine*>(&reader));
        REQUIRE(pf_report_cache_stats(handle, &bytes, &bypasses) == 0);
        EXPECT(bytes == reader.ledger().cache_resident_bytes());
        EXPECT(bypasses == expected_bypasses);
    };
    const auto bypasses_allocation = [&](Reader& reader, auto action) {
        auto& target = reader.ledger();
        EXPECT(!target.cache_allocation_failed());
        EXPECT(target.read_cache_stats().allocation_bypass_count == 0);
        reported(reader, 0);
        const auto refused = global_allocation::refused;
        global_allocation::fail_after = 0;
        try { action(); }
        catch (...) {
            global_allocation::fail_after = std::numeric_limits<Size>::max();
            throw;
        }
        global_allocation::fail_after = std::numeric_limits<Size>::max();
        REQUIRE(global_allocation::refused == refused + 1);
        EXPECT(target.cache_allocation_failed());
        EXPECT(target.read_cache_stats().allocation_bypass_count == 1);
        EXPECT(target.read_cache_stats().bypass_reason
               == TradeLedger::CacheBypassReason::allocation_failed);
        reported(reader, 1);
        reported(reader, 1); // Queries cannot add a second charge or bypass.
    };
    Reader cold;
    begin_diagnostics_run(cold);
    auto& bypassed = cold.ledger();
    bypassed.set_scratch_directory(".");
    bypassed.set_cache_rows(2 * kPageSlots + 3);
    for (Size i = 0; i < kPageSlots; ++i) bypassed.push_back(first);
    const auto fixed = bypassed.cache_resident_bytes();
    bypasses_allocation(cold, [&] {
        EXPECT(bypassed.entry_price_at(0) == first.entry_price);
    });
    EXPECT(bypassed.cache_resident_bytes() == fixed);
    EXPECT(bypassed.cache_size() == 0);
    check_row(cold, 0, first);
    const auto allocations = global_allocation::allocations;
    for (Size i = 0; i < bypassed.size(); ++i)
        EXPECT(bypassed.entry_price_at(i) == first.entry_price);
    EXPECT(global_allocation::allocations == allocations);
    EXPECT(bypassed.read_cache_stats().allocation_bypass_count == 1);

    Reader appender;
    begin_diagnostics_run(appender);
    auto& appended = appender.ledger();
    appended.set_scratch_directory(".");
    appended.set_cache_rows(2 * kPageSlots + 3);
    for (Size i = 0; i < kPageSlots; ++i) appended.push_back(first);
    check_row(appender, 0, first);
    const auto append_bytes = appended.cache_resident_bytes();
    bypasses_allocation(appender, [&] { appended.push_back(first); });
    exact_count(appended, kPageSlots + 1);
    EXPECT(appended.cache_size() == 1);
    EXPECT(appended.cache_resident_bytes() == append_bytes);
    check_row(appender, kPageSlots, first);
    check_row(appender, 0, first);
    // Move the published row outside the descriptor window and read backing.
    for (Size i = 0; i < TradeLedger::kResidentRows; ++i) appended.push_back(first);
    check_row(appender, kPageSlots, first);
    EXPECT(appended.read_cache_stats().allocation_bypass_count == 1);

    // Healthy growth retains the original page and its cached hits.
    Reader host;
    begin_diagnostics_run(host);
    auto& ledger = host.ledger();
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(2 * kPageSlots + 3);
    for (Size i = 0; i < kPageSlots; ++i) ledger.push_back(first);
    check_row(host, 0, first);
    const auto one_page = ledger.cache_resident_bytes();
    ledger.push_back(first);
    exact_count(ledger, kPageSlots + 1);
    EXPECT(ledger.cache_size() == 2);
    EXPECT(ledger.cache_resident_bytes() == one_page + kPageBytes);
    // Key 0 is older than the descriptor tail. It must remain a hit after
    // allocating page 1: discard-on-growth causes real descriptor/text I/O.
    const auto reads_before = text_reads::reads;
    text_reads::measuring = true;
    const bool equal = ledger.entry_id_equals(0, first.entry_id);
    text_reads::measuring = false;
    EXPECT(equal);
    EXPECT(text_reads::reads == reads_before);
    reported(host, 0);
    reported(cold, 1); // Another handle's healthy admission cannot reset this run.

    Reader sparse;
    begin_diagnostics_run(sparse);
    auto& other = sparse.ledger();
    other.set_scratch_directory(".");
    other.set_cache_rows(2 * kPageSlots + 3);
    for (Size i = 0; i <= 2 * kPageSlots; ++i) other.push_back(first);
    check_row(sparse, 0, first); // Only page 0 exists.
    const auto before = other.cache_resident_bytes();
    bypasses_allocation(sparse, [&] {
        EXPECT(other.entry_price_at(2 * kPageSlots) == first.entry_price);
    });
    EXPECT(other.cache_size() == 1);
    EXPECT(other.cache_resident_bytes() == before);
    auto replacement = first;
    replacement.entry_id = "replaced";
    other.replace(2 * kPageSlots, replacement);
    exact_count(other, 2 * kPageSlots + 1);
    EXPECT(other.cache_size() == 1);
    EXPECT(other.cache_resident_bytes() == before);
    check_row(sparse, 2 * kPageSlots, replacement);
    check_row(sparse, 0, first);
    EXPECT(other.read_cache_stats().allocation_bypass_count == 1);

    // Independently refuse the page needed by a replacement fill. The new
    // descriptor and strings still publish, without a preceding failed read.
    Reader replacer;
    begin_diagnostics_run(replacer);
    auto& replaced = replacer.ledger();
    replaced.set_scratch_directory(".");
    replaced.set_cache_rows(2 * kPageSlots + 3);
    for (Size i = 0; i <= 2 * kPageSlots; ++i) replaced.push_back(first);
    check_row(replacer, 0, first);
    const auto replace_bytes = replaced.cache_resident_bytes();
    bypasses_allocation(replacer, [&] {
        replaced.replace(2 * kPageSlots, replacement);
    });
    exact_count(replaced, 2 * kPageSlots + 1);
    EXPECT(replaced.cache_size() == 1);
    EXPECT(replaced.cache_resident_bytes() == replace_bytes);
    check_row(replacer, 2 * kPageSlots, replacement);
    for (Size i = 0; i < TradeLedger::kResidentRows; ++i) replaced.push_back(first);
    check_row(replacer, 2 * kPageSlots, replacement);
    check_row(replacer, 0, first);
    EXPECT(replaced.read_cache_stats().allocation_bypass_count == 1);
    begin_diagnostics_run(cold, 2);
    reported(cold, 0);
    EXPECT(cold.ledger().cache_size() == 0 && !cold.ledger().cache_allocation_failed());
    reported(appender, 1);
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
    check_row(host, 0, first); // Allocate the table and first 256-slot page.
    Size end = 2;
    while (end < TradeLedger::kResidentRows + 1) {
        ledger.push_back(first);
        ++end;
        exact_count(ledger, end); // Previous independent end+one append.
    }
    check_row(host, 0, first); // Crossing 64 preserves the page and its rows.
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
    exact_count(assigned, 5); // Copy assignment swaps in the copied State.
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
    // must preserve the old State, including its exact global end.
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
        const Size fixed = TradeLedger::resident_cache_bytes(1) - TradeLedger::kLedgerEntryBytes - sizeof(void*);
        // Caps 1 and 3 each touch one clipped page and one owning pointer.
        EXPECT(ledger.cache_resident_bytes() == fixed + sizeof(void*) + capacity * TradeLedger::kLedgerEntryBytes);
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
    if (retired) {
        invariant([&] { (void)host.equals<F>(0, View{}); }, retired);
        invariant([&] { (void)ledger_equals<F>(host.ledger(), 0, View{}); }, retired);
    } else {
        EXPECT(host.equals<F>(0, View{}));
        EXPECT(ledger_equals<F>(host.ledger(), 0, View{}));
    }
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
    if (retired) {
        invariant([&] { (void)host.equals<F>(2, View{}); }, retired);
        invariant([&] { (void)ledger_equals<F>(host.ledger(), 2, View{}); }, retired);
    } else {
        const auto live = tiny_trade();
        EXPECT(host.equals<F>(2, field<F>(live)));
        EXPECT(ledger_equals<F>(host.ledger(), 2, field<F>(live)));
    }
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

bool double_bytes(double actual, double expected) {
    return std::memcmp(&actual, &expected, sizeof actual) == 0;
}

bool numeric_bytes(const Trade& actual, const Trade& expected) {
    return actual.entry_time == expected.entry_time
        && actual.exit_time == expected.exit_time
        && double_bytes(actual.entry_price, expected.entry_price)
        && double_bytes(actual.exit_price, expected.exit_price)
        && double_bytes(actual.qty, expected.qty)
        && double_bytes(actual.pnl, expected.pnl)
        && double_bytes(actual.pnl_pct, expected.pnl_pct)
        && actual.is_long == expected.is_long
        && actual.entry_bar_index == expected.entry_bar_index
        && actual.exit_bar_index == expected.exit_bar_index
        && actual.exit_from_bracket == expected.exit_from_bracket
        && double_bytes(actual.max_runup, expected.max_runup)
        && double_bytes(actual.max_drawdown, expected.max_drawdown)
        && double_bytes(actual.commission, expected.commission)
        && actual.entry_incarnation == expected.entry_incarnation
        && actual.open_at_end == expected.open_at_end
        && actual.close_cause == expected.close_cause;
}

bool trade_bytes(const Trade& actual, const Trade& expected, bool numeric = false) {
    return numeric_bytes(actual, expected)
        && actual.entry_id == (numeric ? std::string{} : expected.entry_id)
        && actual.exit_id == (numeric ? std::string{} : expected.exit_id)
        && actual.entry_comment == (numeric ? std::string{} : expected.entry_comment)
        && actual.exit_comment == (numeric ? std::string{} : expected.exit_comment);
}

std::array<Trade, 3> sealed_oracle(bool wide) {
    // Input rows, never read back to construct expected values. Distinct
    // numbers include signed zero; binary fractions make bit checks exact.
    std::array<Trade, 3> rows{{
        {101, 203, 32.0, 34.25, 2.0, -0.0, -0.0, true, 7, 13,
         "oak-entry", std::string("oak\0entry", 9), "oak-close", "oak-exit",
         false, 8.0, 4.0, 0.125, 1011, false, execution::CloseCause::Script},
        {307, 409, 64.0, 61.5, 4.0, 17.5, 6.25, false, 19, 23,
         "elm-entry", std::string("elm\0entry", 9), "elm-close", "elm-exit",
         true, 128.0, 64.0, 0.375, 2027, false, execution::CloseCause::Bracket},
        {503, 607, 128.0, 129.75, 8.0, -23.25, -3.125, true, 29, 31,
         "ash-entry", std::string("ash\0entry", 9), "ash-close", "ash-exit",
         false, 64.0, 32.0, 0.625, 3041, false, execution::CloseCause::Liquidation}
    }};
    if (wide) {
        // Each field has L=tail+1=64*168+1 bytes before its original suffix.
        // Thus L>128 and L>tail: owning/equality reads must handle scratch.
        for (Size i = 0; i < rows.size(); ++i) {
            for (const Field f : {Field::EntryId, Field::ExitId,
                                  Field::EntryComment, Field::ExitComment}) {
                std::string* text = nullptr;
                switch (f) {
                case Field::EntryId: text = &rows[i].entry_id; break;
                case Field::ExitId: text = &rows[i].exit_id; break;
                case Field::EntryComment: text = &rows[i].entry_comment; break;
                case Field::ExitComment: text = &rows[i].exit_comment; break;
                }
                text->insert(0, kTail + 1, static_cast<char>('a' + 4 * i + static_cast<Size>(f)));
            }
        }
    }
    return rows;
}

Trade different_text(Trade value) {
    // Same-length mismatch in the final byte, after entry_comment's NUL.
    for (auto* text : {&value.entry_id, &value.exit_id,
                       &value.entry_comment, &value.exit_comment})
        text->back() ^= 1;
    return value;
}

struct SealedRead {
    const TradeLedger& ledger;
    Size index, capacity, prepare_index;
    bool wide;
    const char* phase;
};

// Catch each accessor independently so the base run identifies every missing
// read path, rather than terminating after the first sealed at() exception.
template<class Action>
void sealed_expect(const SealedRead& row, const char* expression, int line, Action action) {
    ++checks;
    try {
        (void)row.ledger.read(row.prepare_index);
        if (action()) return;
        ++failures;
        std::printf("FAIL %s:%d: %s cache=%zu row=%zu wide=%d phase=%s value mismatch\n",
                    __FILE__, line, expression, row.capacity, row.index, row.wide, row.phase);
    } catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL %s:%d: %s cache=%zu row=%zu wide=%d phase=%s exception: %s\n",
                    __FILE__, line, expression, row.capacity, row.index, row.wide,
                    row.phase, error.what());
    }
}
#define SEALED_EXPECT(expression) \
    sealed_expect(row, #expression, __LINE__, [&] { return bool(expression); })

bool c_text_bytes(const char* actual, const std::string& expected) {
    // These three C fields contain no NUL; entry_comment covers embedded NUL
    // through the owning and comparison APIs instead.
    return actual && std::strlen(actual) == expected.size()
        && std::memcmp(actual, expected.data(), expected.size()) == 0;
}

struct SealedReader : Reader {
    SealedReader() { syminfo_.pointvalue = 2.0; }
    void engine_reads(const SealedRead& row, const Trade& expected) const {
        const auto i = static_cast<int>(row.index);
        SEALED_EXPECT(trade_bytes(closed_trade(row.index), expected));
        SEALED_EXPECT(double_bytes(closed_trade_profit(i), expected.pnl));
        SEALED_EXPECT(double_bytes(closed_trade_profit_percent(i), expected.pnl_pct));
        SEALED_EXPECT(double_bytes(closed_trade_commission(i), expected.commission));
        SEALED_EXPECT(closed_trade_entry_bar_index(i) == expected.entry_bar_index);
        SEALED_EXPECT(closed_trade_exit_bar_index(i) == expected.exit_bar_index);
        SEALED_EXPECT(closed_trade_entry_incarnation(i) == expected.entry_incarnation);
        SEALED_EXPECT(double_bytes(closed_trade_entry_price(i), expected.entry_price));
        SEALED_EXPECT(double_bytes(closed_trade_exit_price(i), expected.exit_price));
        SEALED_EXPECT(closed_trade_entry_time(i) == expected.entry_time);
        SEALED_EXPECT(closed_trade_exit_time(i) == expected.exit_time);
        SEALED_EXPECT(double_bytes(closed_trade_size(i), expected.qty));
        SEALED_EXPECT(double_bytes(closed_trade_max_runup(i), expected.max_runup));
        SEALED_EXPECT(double_bytes(closed_trade_max_drawdown(i), expected.max_drawdown));
        // Independent cost = input entry price * input quantity * pointvalue(2).
        const double cost = expected.entry_price * expected.qty * 2.0;
        SEALED_EXPECT(double_bytes(closed_trade_max_runup_percent(i),
                                   expected.max_runup / cost * 100.0));
        SEALED_EXPECT(double_bytes(closed_trade_max_drawdown_percent(i),
                                   expected.max_drawdown / cost * 100.0));
        SEALED_EXPECT(closed_trade_direction(i) == (expected.is_long ? "long" : "short"));
        SEALED_EXPECT(closed_trade_entry_id(i) == expected.entry_id);
        SEALED_EXPECT(closed_trade_exit_id(i) == expected.exit_id);
        SEALED_EXPECT(closed_trade_entry_comment(i) == expected.entry_comment);
        SEALED_EXPECT(closed_trade_exit_comment(i) == expected.exit_comment);
        SEALED_EXPECT(equals<Field::EntryId>(i, expected.entry_id));
        SEALED_EXPECT(equals<Field::ExitId>(i, expected.exit_id));
        SEALED_EXPECT(equals<Field::EntryComment>(i, expected.entry_comment));
        SEALED_EXPECT(equals<Field::ExitComment>(i, expected.exit_comment));
        const auto other = different_text(expected);
        SEALED_EXPECT(!equals<Field::EntryId>(i, other.entry_id));
        SEALED_EXPECT(!equals<Field::ExitId>(i, other.exit_id));
        SEALED_EXPECT(!equals<Field::EntryComment>(i, other.entry_comment));
        SEALED_EXPECT(!equals<Field::ExitComment>(i, other.exit_comment));
        SEALED_EXPECT(closed_trade_close_cause(i) == static_cast<int>(expected.close_cause));
    }
    void negative_reads() const {
        EXPECT(closed_trade_entry_id(-1).empty());
        EXPECT(closed_trade_exit_id(-1).empty());
        EXPECT(closed_trade_entry_comment(-1).empty());
        EXPECT(closed_trade_exit_comment(-1).empty());
        EXPECT(closed_trade_direction(-1).empty());
        EXPECT(equals<Field::EntryId>(-1, View{}));
        EXPECT(equals<Field::ExitId>(-1, View{}));
        EXPECT(equals<Field::EntryComment>(-1, View{}));
        EXPECT(equals<Field::ExitComment>(-1, View{}));
        EXPECT(!equals<Field::EntryId>(-1, "oak-entry"));
        EXPECT(!equals<Field::ExitId>(-1, "oak-exit"));
        EXPECT(!equals<Field::EntryComment>(-1, "oak-entry"));
        EXPECT(!equals<Field::ExitComment>(-1, "oak-close"));
        EXPECT(double_bytes(closed_trade_profit(-1), 0.0));
        EXPECT(closed_trade_entry_incarnation(-1) == 0);
        EXPECT(closed_trade_entry_time(-1) == 0);
        EXPECT(closed_trade_exit_time(-1) == 0);
    }
};

void sealed_ledger_reads(const SealedRead& row, const Trade& expected) {
    const auto& ledger = row.ledger;
    const auto i = row.index;
    SEALED_EXPECT(trade_bytes(ledger.at(i), expected));
    SEALED_EXPECT(trade_bytes(ledger[i], expected));
    SEALED_EXPECT(trade_bytes(ledger.numeric_at(i), expected, true));
    SEALED_EXPECT(double_bytes(ledger.pnl_at(i), expected.pnl));
    SEALED_EXPECT(double_bytes(ledger.pnl_pct_at(i), expected.pnl_pct));
    SEALED_EXPECT(double_bytes(ledger.commission_at(i), expected.commission));
    SEALED_EXPECT(ledger.entry_bar_index_at(i) == expected.entry_bar_index);
    SEALED_EXPECT(ledger.exit_bar_index_at(i) == expected.exit_bar_index);
    SEALED_EXPECT(ledger.entry_incarnation_at(i) == expected.entry_incarnation);
    SEALED_EXPECT(double_bytes(ledger.entry_price_at(i), expected.entry_price));
    SEALED_EXPECT(double_bytes(ledger.exit_price_at(i), expected.exit_price));
    SEALED_EXPECT(ledger.entry_time_at(i) == expected.entry_time);
    SEALED_EXPECT(ledger.exit_time_at(i) == expected.exit_time);
    SEALED_EXPECT(double_bytes(ledger.qty_at(i), expected.qty));
    SEALED_EXPECT(double_bytes(ledger.max_runup_at(i), expected.max_runup));
    SEALED_EXPECT(double_bytes(ledger.max_drawdown_at(i), expected.max_drawdown));
    SEALED_EXPECT(ledger.is_long_at(i) == expected.is_long);
    SEALED_EXPECT(ledger.entry_id_at(i) == expected.entry_id);
    SEALED_EXPECT(ledger.exit_id_at(i) == expected.exit_id);
    SEALED_EXPECT(ledger.entry_comment_at(i) == expected.entry_comment);
    SEALED_EXPECT(ledger.exit_comment_at(i) == expected.exit_comment);
    SEALED_EXPECT(ledger_equals<Field::EntryId>(ledger, i, expected.entry_id));
    SEALED_EXPECT(ledger_equals<Field::ExitId>(ledger, i, expected.exit_id));
    SEALED_EXPECT(ledger_equals<Field::EntryComment>(ledger, i, expected.entry_comment));
    SEALED_EXPECT(ledger_equals<Field::ExitComment>(ledger, i, expected.exit_comment));
    const auto other = different_text(expected);
    SEALED_EXPECT(!ledger_equals<Field::EntryId>(ledger, i, other.entry_id));
    SEALED_EXPECT(!ledger_equals<Field::ExitId>(ledger, i, other.exit_id));
    SEALED_EXPECT(!ledger_equals<Field::EntryComment>(ledger, i, other.entry_comment));
    SEALED_EXPECT(!ledger_equals<Field::ExitComment>(ledger, i, other.exit_comment));
}

void sealed_c_reads(SealedReader& host, const SealedRead& row, const Trade& expected) {
    const auto handle = static_cast<BacktestEngine*>(&host);
    const auto i = static_cast<int>(row.index);
    SEALED_EXPECT(c_text_bytes(strategy_closed_trade_entry_id(handle, i), expected.entry_id));
    SEALED_EXPECT(c_text_bytes(strategy_closed_trade_exit_id(handle, i), expected.exit_id));
    SEALED_EXPECT(c_text_bytes(strategy_closed_trade_exit_comment(handle, i), expected.exit_comment));
    SEALED_EXPECT(strategy_closed_trade_entry_incarnation(handle, i) == expected.entry_incarnation);
    SEALED_EXPECT(strategy_closed_trade_close_cause(handle, i) == static_cast<int>(expected.close_cause));
}
#undef SEALED_EXPECT

template<class Action> void typed_retired(Action action) {
    bool caught = false;
    try { action(); }
    catch (const detail::RetiredTradeHistoryRead& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        EXPECT(failure.code == RunFailureCode::engine_invariant);
        EXPECT(!failure.args || *failure.args == "{}");
    }
    EXPECT(caught);
}

void sealed_controls(SealedReader& host, const std::array<Trade, 3>& oracle) {
    auto& ledger = host.ledger();
    invariant([&] { ledger.replace(0, oracle[2]); });
    bool modified = false;
    invariant([&] { ledger.modify(1, [&](Trade&) { modified = true; }); });
    EXPECT(!modified);
    EXPECT(trade_bytes(ledger.read(0), oracle[0]));
    EXPECT(trade_bytes(ledger.read(1), oracle[1]));
    // The end is the independent initial population, 3 appended rows.
    EXPECT(ledger.size() == oracle.size());
#define END_REFUSES(accessor) invariant([&] { (void)ledger.accessor(oracle.size()); })
    END_REFUSES(at);
    END_REFUSES(numeric_at);
    END_REFUSES(pnl_at);
    END_REFUSES(pnl_pct_at);
    END_REFUSES(commission_at);
    END_REFUSES(entry_bar_index_at);
    END_REFUSES(exit_bar_index_at);
    END_REFUSES(entry_incarnation_at);
    END_REFUSES(entry_price_at);
    END_REFUSES(exit_price_at);
    END_REFUSES(entry_time_at);
    END_REFUSES(exit_time_at);
    END_REFUSES(qty_at);
    END_REFUSES(max_runup_at);
    END_REFUSES(max_drawdown_at);
    END_REFUSES(is_long_at);
    END_REFUSES(entry_id_at);
    END_REFUSES(exit_id_at);
    END_REFUSES(entry_comment_at);
    END_REFUSES(exit_comment_at);
#undef END_REFUSES
    invariant([&] { (void)ledger_equals<Field::EntryId>(ledger, oracle.size(), View{}); });
    invariant([&] { (void)ledger_equals<Field::ExitId>(ledger, oracle.size(), View{}); });
    invariant([&] { (void)ledger_equals<Field::EntryComment>(ledger, oracle.size(), View{}); });
    invariant([&] { (void)ledger_equals<Field::ExitComment>(ledger, oracle.size(), View{}); });
    host.negative_reads();
    const auto handle = static_cast<BacktestEngine*>(&host);
    EXPECT(strategy_closed_trade_entry_id(handle, 3) == nullptr);
    EXPECT(strategy_closed_trade_exit_id(handle, 3) == nullptr);
    EXPECT(strategy_closed_trade_exit_comment(handle, 3) == nullptr);

    SealedReader retiring;
    auto& retired = retiring.ledger();
    retired.set_scratch_directory(".");
    retired.set_cache_rows(ledger.cache_rows());
    detail::TradeLedgerStorageProbe::retire(retired);
    for (const auto& value : oracle) retired.push_back(value);
    (void)retired.read(0); // A cached prefix must still throw its specific type.
    (void)retired.read(1);
    retired.seal(2);
    EXPECT(retired.base() == 2 && retired.size() == oracle.size());
    for (const Size i : {Size{0}, Size{1}}) {
#define RETIRED_REFUSES(accessor) typed_retired([&] { (void)retired.accessor(i); })
        RETIRED_REFUSES(read);
        RETIRED_REFUSES(at);
        RETIRED_REFUSES(numeric_at);
        RETIRED_REFUSES(pnl_at);
        RETIRED_REFUSES(pnl_pct_at);
        RETIRED_REFUSES(commission_at);
        RETIRED_REFUSES(entry_bar_index_at);
        RETIRED_REFUSES(exit_bar_index_at);
        RETIRED_REFUSES(entry_incarnation_at);
        RETIRED_REFUSES(entry_price_at);
        RETIRED_REFUSES(exit_price_at);
        RETIRED_REFUSES(entry_time_at);
        RETIRED_REFUSES(exit_time_at);
        RETIRED_REFUSES(qty_at);
        RETIRED_REFUSES(max_runup_at);
        RETIRED_REFUSES(max_drawdown_at);
        RETIRED_REFUSES(is_long_at);
        RETIRED_REFUSES(entry_id_at);
        RETIRED_REFUSES(exit_id_at);
        RETIRED_REFUSES(entry_comment_at);
        RETIRED_REFUSES(exit_comment_at);
#undef RETIRED_REFUSES
        typed_retired([&] { (void)ledger_equals<Field::EntryId>(retired, i, View{}); });
        typed_retired([&] { (void)ledger_equals<Field::ExitId>(retired, i, View{}); });
        typed_retired([&] { (void)ledger_equals<Field::EntryComment>(retired, i, View{}); });
        typed_retired([&] { (void)ledger_equals<Field::ExitComment>(retired, i, View{}); });
    }
}

void sealed_reads() {
    for (const bool wide : {false, true}) {
        const auto oracle = sealed_oracle(wide);
        for (const Size capacity : {Size{1}, Size{3}}) {
            SealedReader host;
            auto& ledger = host.ledger();
            ledger.set_scratch_directory(".");
            ledger.set_cache_rows(capacity);
            for (const auto& value : oracle) ledger.push_back(value);
            (void)ledger.read(0); // Warm key 0 before sealing can spill a block.
            ledger.seal(2);
            EXPECT(ledger.base() == 2 && ledger.size() == oracle.size());
            for (const Size i : {Size{0}, Size{1}}) {
                const SealedRead row{ledger, i, capacity, i, wide, "warm"};
                sealed_ledger_reads(row, oracle[i]);
                host.engine_reads(row, oracle[i]);
                sealed_c_reads(host, row, oracle[i]);
            }
            sealed_controls(host, oracle);
            // Append C displacers at [3,3+C). With C=1 or 3, key
            // 3+(i%C) has the same cache slot as i. Reading it immediately
            // before EVERY assertion evicts that row, including C ABI reads.
            for (Size j = 0; j < capacity; ++j) {
                auto displacer = oracle[2];
                displacer.entry_id = "displacer-entry-" + std::to_string(j);
                displacer.exit_id = "displacer-exit-" + std::to_string(j);
                displacer.pnl = 1000.0 + static_cast<double>(j);
                ledger.push_back(displacer);
            }
            for (const Size i : {Size{0}, Size{1}}) {
                const SealedRead row{ledger, i, capacity, oracle.size() + i % capacity,
                                     wide, "evicted"};
                sealed_ledger_reads(row, oracle[i]);
                host.engine_reads(row, oracle[i]);
                sealed_c_reads(host, row, oracle[i]);
            }
        }
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
    run("page allocation failures and growth hits", page_allocation_failures_and_growth_hits);
    run("collisions", collisions);
    run("refusals and retiring copies", refusals_and_retiring_copies);
    run("sealed reads", sealed_reads);
    std::printf("test_closed_trade_text_reads: reader=%d mutant=%d "
                "%zu checks, %zu failures\n",
                PINEFORGE_CLOSED_TEXT_READER, PINEFORGE_CLOSED_TEXT_MUTANT,
                checks, failures);
    return failures ? 1 : 0;
}
