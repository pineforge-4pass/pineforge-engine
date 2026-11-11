#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace pineforge {
struct CacheCapacity;
class LedgerCachePolicy;

struct Trade;
namespace detail { struct TradeLedgerStorageProbe; }
namespace report { class TradeSourceV1; }

// The 1.11 public layout change updates the shared engine epoch once.
// This SIDE source deliberately does not perform a separate local bump.
inline namespace engine_script_run_v20 {

// Exact global row space. base() is the publication/fold boundary; an
// unsealed exit-time group can have arbitrary length and cannot be retired.
// A kResidentRows descriptor window and an equally sized fixed byte tail of
// the string payload stay in memory. All four string payloads and all older
// mutable/sealed descriptors use the existing run-owned positional scratch
// files. A separate, budgeted read cache retains numeric records and up to
// kCacheStringBytes of decoded string bytes per row. The default uses a fixed
// byte allowance and associative eviction, independent of retained history.
// Wider rows retain their numeric records in the cache and use scratch only
// for string reads.
// Owning Trade values still belong to readers.
//
// Reads own their values. No reference or contiguous pointer into an
// evictable page escapes. Iteration visits [base(),size()) as before, now
// by value; read()/visit()/previous() also read the sealed history, except
// when a sink run explicitly admits physical history retirement.
class TradeLedger {
public:
    using size_type = std::size_t;
    using value_type = Trade;
    // The fixed descriptor width is part of the ledger I/O contract. The
    // implementation asserts it against State::Record after that private
    // storage type is complete.
    static constexpr size_type kRecordBytes = 168;
    static constexpr size_type kResidentRows = 64;
    // ledger_cache_max_bytes bounds optional hot storage. Each entry contains a
    // 168-byte numeric/extent record, a global key and generation/flags,
    // and exactly 128 inline string bytes (all four strings combined).
    // String validity, short-ID length and replacement rank share the generation
    // word; no separate replacement directory exists. On 64-bit targets:
    // 168 + 16 + 128 = 312 bytes, with no additional alignment padding.
    // Byte allowances include both entries and the private page directory.
    // No per-entry heap allocation or growing offset/string directory.
    static constexpr size_type kCacheStringBytes = 128;
    static constexpr size_type kLedgerEntryBytes =
        kRecordBytes + 2 * sizeof(size_type) + kCacheStringBytes;
    static constexpr size_type kCachePageSlots = 256;
    static constexpr size_type kCacheWays = 4;
    // Provisional source candidate; measurement/disposition must freeze it.
    static constexpr size_type kDefaultLedgerCacheMaxBytes = 67108864;
    // Existing explicit row-setting geometry/range, not the default byte cap.
    static constexpr size_type kDefaultCacheRows = 65536;
    static constexpr size_type kMaxCacheRows = 1048576;
    static constexpr size_type cache_capacity_bytes(size_type rows) noexcept {
        return rows * kLedgerEntryBytes
            + (rows / kCachePageSlots + (rows % kCachePageSlots != 0)) * sizeof(void*);
    }
    // Retry arithmetic is shared by directory/page failures. A failure at zero
    // first becomes eligible at one; an unrepresentable doubling never retries.
    static constexpr bool cache_retry_population_reached(
            size_type failed, size_type population) noexcept {
        return failed == 0 ? population != 0
            : failed <= std::numeric_limits<size_type>::max() / 2
                && population >= failed * 2;
    }
    // Sole byte-to-row conversion, shared by the host and standalone setters.
    static size_type cache_rows_for_capacity(CacheCapacity capacity) noexcept;
    // Compatibility constant for an explicitly selected legacy sink row cap.
    // Omitted sink/array policies both use kDefaultLedgerCacheMaxBytes instead.
    static constexpr size_type kSinkCacheRows = kDefaultCacheRows;
    using Visitor = std::function<bool(size_type, const Trade&)>;

    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Trade;
        using difference_type = std::ptrdiff_t;
        using reference = Trade;
        using pointer = void;
        const_iterator() = default;
        Trade operator*() const;
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const const_iterator& other) const {
            return ledger_ == other.ledger_ && index_ == other.index_;
        }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }
    private:
        friend class TradeLedger;
        const_iterator(const TradeLedger* ledger, size_type index)
            : ledger_(ledger), index_(index) {}
        const TradeLedger* ledger_ = nullptr;
        size_type index_ = 0;
    };
    using iterator = const_iterator;

    TradeLedger();
    explicit TradeLedger(size_type cache_rows);
    explicit TradeLedger(CacheCapacity capacity);
    // Preserve omission intent instead of implicitly converting AUTO to bytes.
    explicit TradeLedger(const LedgerCachePolicy& policy);
    ~TradeLedger();
    TradeLedger(const TradeLedger&);
    TradeLedger& operator=(const TradeLedger&);
    TradeLedger(TradeLedger&&);
    TradeLedger& operator=(TradeLedger&&);

    // Bind before the first spill. No mapping or in-memory offset directory.
    void set_scratch_directory(const std::string&);
    const std::string& scratch_directory() const;
    // Configure before the first append/spill; clear() preserves policy intent.
    // Compatibility spelling: select the provisional fixed default byte cap.
    // No history-sized AUTO allocation or reindex remains.
    void set_cache_auto();
    // Inclusive bytes for entry payload, tags, packed replacement ranks and
    // the directory. All size_type values [0,SIZE_MAX] are valid. Zero or less
    // than one complete entry+pointer bypasses; otherwise round down to the
    // largest fitting row geometry. There is no legacy row ceiling here.
    // Geometry is fixed until clear/reconfiguration; pages allocate lazily.
    // Failed optional allocations retain old hits and retry only after the
    // retained population doubles since the failed attempt. No value/history
    // limit is introduced. Configuration is frozen after append/spill.
    void set_ledger_cache_max_bytes(size_type bytes);
    size_type ledger_cache_max_bytes() const;
    // The row constructor/setter always select a finite budget. Zero bypasses
    // the decoded cache. Optional table/page allocation failure selects
    // backing reads for this ledger without retrying the failed allocation.
    // Valid rows: [0,1048576]; invalid/late settings throw a coded
    // std::invalid_argument (setting_rejected, reason value_above_maximum
    // or frozen). Legacy initial geometry: 65,536; maximum: 1,048,576.
    // On 64-bit targets, entries plus the page directory cost 20,449,280 B
    // at 65,536 rows and 327,188,480 B at maximum, plus the fixed base.
    void set_cache_rows(size_type rows);
    // Standalone inclusive byte allowance for entries plus owning pointers.
    // Selects the largest row cap that fits, without the legacy row ceiling;
    // allowances below one entry plus one pointer bypass. The host policy
    // uses this same conversion, with the existing row-setting ceiling.
    // Frozen after the first append/spill.
    void set_cache_capacity(CacheCapacity capacity);
    size_type cache_rows() const;
    // Accounted storage: sizeof(TradeLedger)+sizeof(State)+rows*kLedgerEntryBytes
    // + ceil(rows/256)*sizeof(void*). Excludes allocator overhead, scratch/OS pages,
    // configured directory strings, returned owning Trades and the explicit
    // legacy materialize() map. Validates the same row range as the setter.
    static size_type resident_cache_bytes(size_type rows);
    // Actual allocation: fixed base + ceil(cache_rows()/256) owning pointers
    // after first read + min(256,cache_rows()-256*p)*kLedgerEntryBytes per page p.
    // Placement follows the row count: up to cache_rows() rows, row n owns slot n (one way);
    // with more rows, four-way sets (a final partial set is permitted) use bounded rank
    // replacement. A crossing drops the cached rows, keeps the pages. Writes only fill an
    // existing cache; no rebuild or coexisting allocation. Retirement may release every hot page.
    // The fixed base (with the zero-row read workspace), directory and entry pages count once.
    size_type cache_resident_bytes() const;
    enum class CacheBypassReason : std::uint32_t { none, allocation_failed };
    struct CacheStats {
        std::uint64_t allocation_bypass_count = 0;
        CacheBypassReason bypass_reason = CacheBypassReason::none;
        std::uint64_t allocation_attempt_count = 0;
        std::uint64_t hot_hit_count = 0, cold_miss_count = 0;
        std::uint64_t lookup_probe_count = 0;
        std::uint64_t descriptor_read_count = 0, descriptor_read_bytes = 0;
        std::uint64_t string_read_count = 0, string_read_bytes = 0;
        size_type failed_population = 0;
        bool allocation_retry_blocked = false;
    };
    // Every real failed optional allocation increments the failure count and
    // diagnostics sink; every attempt is separately observable. Counters
    // saturate at UINT64_MAX. Representability does not fabricate a failure.
    // Explicit finite legacy policies retain sticky allocation bypass.
    // The byte-bound/default path retries on population doubling. Rewind does
    // not reset that threshold. Copy starts fresh; clear starts fresh.
    bool cache_allocation_failed() const noexcept;
    CacheStats read_cache_stats() const noexcept;
    // Occupied decoded entries only; separate from resident_size()'s legacy
    // descriptor/materialization accounting.
    size_type cache_size() const;
    void clear();
    void reserve(size_type);
    size_type size() const;
    size_type base() const;
    // Cached fixed descriptors plus explicitly materialized legacy values.
    // No retained string payload belongs to the normal descriptor window.
    size_type resident_size() const;
    size_type resident_base() const;
    bool empty() const;

    // read()/at()/[] span the retained source, including sealed rows on a
    // non-retiring ledger. An explicitly retiring ledger refuses reads below
    // base() with a coded std::logic_error (engine_invariant).
    // Writers still reject sealed rows below the publication boundary.
    // Hits allocate only the owning strings already required by read().
    // A miss reads at most one direct-addressed ordinal descriptor and the
    // requested text bytes, with at most 128 B of packed small-text prefetch.
    // Associative eviction never scans/rebuilds history.
    // A caller's history loop still costs O(T), plus its requested text bytes.
    Trade read(size_type global) const;
    Trade operator[](size_type global) const;
    Trade at(size_type global) const;
    Trade back() const;
    // Same retained-row read contract as at(). numeric_at() returns every
    // numeric field and flag with empty strings, reading only the descriptor.
    // entry_id_at() returns only that string and shares the field read cache.
    Trade numeric_at(size_type global) const;
    std::string entry_id_at(size_type global) const;
    // Pine field reads use the same check(global) as at(). A numeric
    // hit reads the Record from its private cache entry without
    // constructing a Trade or owning string. A string hit constructs only
    // the requested string from its packed bytes.
    // Misses retain the numeric record and all strings fitting the allowance.
    double pnl_at(size_type global) const;
    double pnl_pct_at(size_type global) const;
    double commission_at(size_type global) const;
    std::int32_t entry_bar_index_at(size_type global) const;
    std::int32_t exit_bar_index_at(size_type global) const;
    std::uint64_t entry_incarnation_at(size_type global) const;
    double entry_price_at(size_type global) const;
    double exit_price_at(size_type global) const;
    std::int64_t entry_time_at(size_type global) const;
    std::int64_t exit_time_at(size_type global) const;
    double qty_at(size_type global) const;
    double max_runup_at(size_type global) const;
    double max_drawdown_at(size_type global) const;
    bool is_long_at(size_type global) const;
    std::string entry_comment_at(size_type global) const;
    std::string exit_comment_at(size_type global) const;
    std::string exit_id_at(size_type global) const;
    // Same checked retained-row contract as the owning text getters. Compare
    // exact bytes (including NUL); empty fields equal an empty rhs. Return a
    // plain bool: no pointer escapes. Warm reads allocate no owning strings;
    // oversized text uses fixed stack chunks and consumes the whole extent,
    // even on mismatch, so later I/O errors propagate. As with existing const
    // reads, all access to the same ledger requires external synchronization.
    bool entry_id_equals(size_type global, std::string_view rhs) const;
    bool exit_id_equals(size_type global, std::string_view rhs) const;
    bool entry_comment_equals(size_type global, std::string_view rhs) const;
    bool exit_comment_equals(size_type global, std::string_view rhs) const;
    void replace(size_type global, const Trade&);
    void modify(size_type global, const std::function<void(Trade&)>&);

    /// Append at the logical global end (3.4:613-616).
    void push_back(const Trade& trade);
    void push_back(Trade&& trade);

    // A rewind can cross the resident window, but not the published base.
    void resize(size_type global_end);

    const_iterator begin() const;
    const_iterator end() const;
    void visit(size_type first, size_type end, const Visitor&) const;
    // Initialize cursor=size(); each success decrements it and returns that
    // global index's exact row, newest resident first and then scratch.
    // Independent cursors may interleave and retain the returned values.
    bool previous(size_type& cursor, Trade& value) const;

    // Explicit legacy borrowed-pointer materialization. Only the C string
    // ABI uses this non-flat compatibility boundary; execution, folds and
    // report streaming must use read()/visit() instead.
    const Trade& materialize(size_type global) const;

    // Publication still requires a sink and streamable policy, returned
    // applied notifications, finalized phase/money folds and old exit times.
    // Keep the last exit-time block, its predecessor and a short-seed opening
    // that can still be relabelled. Write T and advance the exact row, money,
    // metric and phase seeds before advancing this publication boundary.
    // Those predicates may pin every row; physical eviction is independent.
    // Historical bytes remain readable unless the engine admitted retirement
    // from an explicit no-history generated-module capability.
    void seal(size_type global_end);

    // Signed-index comparisons: an index outside [0,size()) compares as empty.
    // Valid indices keep the checked retained-row contract of *_equals.
    bool entry_id_matches(int index, std::string_view rhs) const;
    bool exit_id_matches(int index, std::string_view rhs) const;
    bool entry_comment_matches(int index, std::string_view rhs) const;
    bool exit_comment_matches(int index, std::string_view rhs) const;

private:
    friend class BacktestEngine;
    friend struct detail::TradeLedgerStorageProbe;
    // Run-begin policy and non-public storage census; neither changes this
    // installed object's layout or the logical trade/hash representation.
    void set_history_retirement(bool);
    void check_history_read(size_type) const;
    // Legacy file/arena extents; a retained capability's fstat extent enters
    // the first slot once, with zero in the second. Never RSS/charge/blocks.
    std::array<std::uint64_t, 2> scratch_storage_bytes() const;
    // Private composition proposal, not a shipping resource/config default.
    // The supplier transfers one exclusively owned O_RDWR/CLOEXEC capability
    // of exactly extent bytes; copies/clear require a fresh distinct resource.
    // Extent and decoded-cache capacity are independent. No fallback or resize.
    using HistoryFactory = std::function<int(std::uint64_t)>;
    void bind_retained_history(HistoryFactory, std::uint64_t extent);
    // {declared extent, index bytes, payload bytes, pending/committed high water}.
    // Logical/reservation facts only; none is a resident-memory observation.
    std::array<std::uint64_t, 4> retained_history_bytes() const;
    std::unique_ptr<report::TradeSourceV1> retained_trade_source() const;
    class Source;
    struct State;
    std::unique_ptr<State> state_;
};

} // inline namespace engine_script_run_v20
} // namespace pineforge
