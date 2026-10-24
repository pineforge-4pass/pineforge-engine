#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

namespace pineforge {

struct Trade;

// The engine-side additions stay in this existing epoch (DESIGN 6:1344-1347).
inline namespace engine_script_run_v19 {

// Exact global row space. base() is the publication/fold boundary, NOT a
// storage boundary: an unsealed exit-time group can have arbitrary length.
// A kResidentRows descriptor window and an equally sized fixed byte tail of
// the string payload stay in memory. All four string payloads and all older
// mutable/sealed descriptors use the existing run-owned positional scratch
// files. Owning Trade values belong to readers, so no arbitrary-width string
// is hidden in the resident window.
//
// Reads own their values. No reference or contiguous pointer into an
// evictable page escapes. Iteration visits [base(),size()) as before, now
// by value; read()/visit()/previous() also read the sealed history.
class TradeLedger {
public:
    using size_type = std::size_t;
    using value_type = Trade;
    // The fixed descriptor width is part of the ledger I/O contract. The
    // implementation asserts it against State::Record after that private
    // storage type is complete.
    static constexpr size_type kRecordBytes = 168;
    static constexpr size_type kResidentRows = 64;
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
    void clear();
    void reserve(size_type);
    size_type size() const;
    size_type base() const;
    // Cached fixed descriptors plus explicitly materialized legacy values.
    // No retained string payload belongs to the normal descriptor window.
    size_type resident_size() const;
    size_type resident_base() const;
    bool empty() const;

    // read() spans the whole exact source; at()/[] retain the checked
    // unsealed-window contract for existing settlement callers.
    Trade read(size_type global) const;
    Trade operator[](size_type global) const;
    Trade at(size_type global) const;
    Trade back() const;
    // Same checked unsealed-row contract as at(). numeric_at() returns every
    // numeric field and flag with empty strings, reading only the descriptor;
    // entry_id_at() reads just that one string when a predicate needs it.
    Trade numeric_at(size_type global) const;
    std::string entry_id_at(size_type global) const;
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
    // Historical bytes remain readable independently of publication.
    void seal(size_type global_end);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // inline namespace engine_script_run_v19
} // namespace pineforge
