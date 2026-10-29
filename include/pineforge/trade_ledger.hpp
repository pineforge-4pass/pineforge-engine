#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

namespace pineforge {

struct Trade;
namespace metrics { class TradeStatsAccumulator; }
namespace detail { struct TradeLedgerStorageProbe; }

// The engine-side additions stay in this existing epoch (DESIGN 6:1344-1347).
inline namespace engine_script_run_v19 {

// Exact global row space. base() is the publication/fold boundary; an
// unsealed exit-time group can have arbitrary length and cannot be retired.
// A kResidentRows descriptor window and an equally sized fixed byte tail of
// the string payload stay in memory. All four string payloads and all older
// mutable/sealed descriptors use the existing run-owned positional scratch
// files. A separate, budgeted read cache retains numeric records and up to
// kCacheStringBytes of decoded string bytes per row. Wider rows retain their
// numeric records in the cache and use scratch only for string reads.
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
    // One knob for this cache kind: ledger_rows. Each entry contains a
    // 168-byte numeric/extent record, a global key and validity generation,
    // and exactly 128 inline string bytes (all four strings combined).
    // The string-validity flag shares the generation word. On 64-bit targets:
    // 168 + 16 + 128 = 312 bytes, with no additional alignment padding.
    // No per-entry heap allocation or growing offset/string directory.
    static constexpr size_type kCacheStringBytes = 128;
    static constexpr size_type kLedgerEntryBytes =
        kRecordBytes + 2 * sizeof(size_type) + kCacheStringBytes;
    static constexpr size_type kDefaultCacheRows = 65536;
    static constexpr size_type kMaxCacheRows = 1048576;
    // A report-sink run has the same declared constant cap as the default:
    // 65,536 x 312 = 20,447,232 entry bytes, plus sizeof(TradeLedger)+sizeof(State).
    // Allocation remains lazy and grows by powers of two up to this cap.
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
    ~TradeLedger();
    TradeLedger(const TradeLedger&);
    TradeLedger& operator=(const TradeLedger&);
    TradeLedger(TradeLedger&&);
    TradeLedger& operator=(TradeLedger&&);

    // Bind before the first spill. No mapping or in-memory offset directory.
    void set_scratch_directory(const std::string&);
    const std::string& scratch_directory() const;
    // Configure before the first append/spill; clear() preserves the budget.
    // Valid rows: [1,1048576]; invalid/late settings throw a coded
    // std::invalid_argument (setting_rejected, reason value_below_minimum,
    // value_above_maximum or frozen). Default: 65,536 rows, 20,447,232 entry
    // bytes on 64-bit targets; maximum: 1,048,576 rows, 327,155,712 entry bytes.
    void set_cache_rows(size_type rows);
    size_type cache_rows() const;
    // Accounted ledger/cache storage: rows*kLedgerEntryBytes plus the fixed
    // ledger, State and accumulator storage. Excludes allocator/scratch/OS pages,
    // configured directory strings, returned owning Trades and the explicit
    // legacy materialize() map. Validates the same row range as the setter.
    static size_type resident_cache_bytes(size_type rows);
    // Actual allocation: fixed base plus allocated capacity*kLedgerEntryBytes.
    // Allocated on the first read, at min(cache_rows(), the next power of two
    // >= max(64,size())). Further growth invalidates entries; capacity stays
    // bounded by cache_rows(), not the bars (a retiring ledger: retained rows).
    size_type cache_resident_bytes() const;
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

    // read() spans the retained source; a read below base() on an explicitly
    // retiring sink run refuses with coded logic_error engine_invariant.
    // at()/[] retain the checked
    // unsealed-window contract for existing settlement callers.
    // Hits allocate only the owning strings already required by read().
    // Direct-mapped eviction above cache_rows(), or oversized strings, retains
    // the existing scratch path; a history loop can still cost O(T).
    Trade read(size_type global) const;
    Trade operator[](size_type global) const;
    Trade at(size_type global) const;
    Trade back() const;
    // Same checked unsealed-row contract as at(). numeric_at() returns every
    // numeric field and flag with empty strings, reading only the descriptor.
    // entry_id_at() returns only that string and shares the field read cache.
    Trade numeric_at(size_type global) const;
    std::string entry_id_at(size_type global) const;
    // Pine field reads use the same check(global,true) as at(). A numeric
    // hit loads just its Record member, without constructing/copying a Trade
    // or any string. A string hit copies only the requested packed bytes.
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

    // Derived ordered percentage fold. No history-sized allocation or hash
    // state belongs to this cache; a changed prefix or ledger restarts it.
    struct PercentFold {
        double all_sum = 0.0;
        double win_sum = 0.0;
        double loss_sum = 0.0;
        size_type all_count = 0;
        size_type win_count = 0;
        size_type loss_count = 0;
    };
    PercentFold percent_fold(const PercentFold& sealed_prefix) const;
    /// Test and diagnostics observer; counts rows consumed by percentage
    /// folds for this State. It is monotone, fixed-size and never hashed or
    /// consulted for cache validity.
    std::uint64_t percent_fold_rows_consumed() const noexcept;
    // Sealed-prefix advancement/reset also changes the fold's starting seed.
    void invalidate_percent_fold() noexcept;

    // Derived report fold. Null metric seeds mean the original empty
    // ALL/LONG/SHORT states at initial_capital; a sealed range supplies all
    // three unfinalized seeds. Output accumulators receive independent copies.
    struct ReportSeed {
        double net = 0.0;
        const metrics::TradeStatsAccumulator* all = nullptr;
        const metrics::TradeStatsAccumulator* longs = nullptr;
        const metrics::TradeStatsAccumulator* shorts = nullptr;
    };
    struct ReportFold {
        double net = 0.0; // Last finite net, starting at positive zero.
        size_type total_trades = 0; // Includes an aggregate-overflow row.
        size_type failure_row = 0;
        const char* nonfinite_field = nullptr;
    };
    ReportFold report_fold(
        double initial_capital, const ReportSeed& sealed_seed,
        metrics::TradeStatsAccumulator* all = nullptr,
        metrics::TradeStatsAccumulator* longs = nullptr,
        metrics::TradeStatsAccumulator* shorts = nullptr) const;
    // Counts every eagerly or lazily consumed row, including invalid rows.
    // Fixed diagnostic state only, never hashed or used as a cache key.
    std::uint64_t report_fold_rows_consumed() const noexcept;

    // Each derived fold retains this many fixed pre-row snapshots. Covered
    // replace/modify edits inside the window restore the exact prefix and
    // replay its suffix in order; older edits restart from the sealed seed.
    // Snapshots are reconstructible, run-owned and never hashed.
    static constexpr size_type kFoldReplayRows = 16;

private:
    friend class BacktestEngine;
    friend struct detail::TradeLedgerStorageProbe;
    // Run-begin policy and non-public storage census; neither changes this
    // installed object's layout or the logical trade/hash representation.
    void set_history_retirement(bool);
    void check_history_read(size_type) const;
    std::array<std::uint64_t, 2> scratch_storage_bytes() const;
    struct State;
    std::unique_ptr<State> state_;
};

} // inline namespace engine_script_run_v19
} // namespace pineforge
