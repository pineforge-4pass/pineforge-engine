#pragma once

// CR-150 P00 declarations only. DESIGN.md revision 6, section 3.4:568-716.
// No container swap, streaming, codegen flag or execution change is installed.
#include <cstddef>
#include <memory>

namespace pineforge {

struct Trade;

// The engine-side additions stay in this existing epoch (DESIGN 6:1344-1347).
inline namespace engine_script_run_v19 {

/// Closed rows with stable global indices (DESIGN 3.4:612-620). size() is
/// the logical global end, including the sealed prefix; base() is the first
/// resident global index. The resident window is [base(),size()). Iteration
/// visits only that window, in row order; event/adapter indices never rebase.
/// Tail resize/replacement still uses global indices, as today's vector does.
///
/// Retention policy (3.4:619-620,690-711): the engine defaults to retaining
/// every row. Seal/drop is permitted only with BOTH an installed sink and
/// codegen's streamable policy. A closedtrades.* accessor, or missing policy
/// in an old module, retains all rows, while still writing the same sink.
/// opentrades.* reads open lots, so it does not require closed rows. Counts,
/// first_index, capital_held and the three avg_*_percent built-ins do not
/// disable streaming; their counts/prefix folds remain available.
///
/// Below-base internal reads, edits and rewinds throw engine_invariant
/// (3.4:664-665). C accessors must check the window before indexing: a
/// streamed row answers 0 for entry incarnation, NULL for ids/comment, -1
/// for close cause; no exception crosses C (3.4:666-670).
///
/// RESOLVED T1: CORE-TOP's rev 6 amendment (10-18 16:04) supplies reset,
/// capacity hints, checked access and the empty query below. References and
/// iterators invalidate as a std::vector's. Representation is opaque.
class TradeLedger {
public:
    using size_type = std::size_t;
    using value_type = Trade;
    using iterator = Trade*;
    using const_iterator = const Trade*;

    TradeLedger();
    ~TradeLedger();
    TradeLedger(const TradeLedger&);
    TradeLedger& operator=(const TradeLedger&);
    TradeLedger(TradeLedger&&);
    TradeLedger& operator=(TradeLedger&&);

    /// Reset size() to 0 and base() to 0 (RESOLVED T1 amendment).
    void clear();

    /// Capacity hint (RESOLVED T1 amendment).
    void reserve(size_type);

    /// Total logical row count, NOT resident count (3.4:613-618).
    /// Resident count is size()-base(); sealed rows remain in total counts.
    size_type size() const;
    size_type base() const;

    /// True exactly when size() == 0 (RESOLVED T1 amendment).
    bool empty() const;

    /// Global-index access to the resident window (3.4:613-616,664-665).
    Trade& operator[](size_type global);
    const Trade& operator[](size_type global) const;

    /// Checked global-index access (RESOLVED T1 amendment). Both forms
    /// throw engine_invariant below base() or at/after size().
    Trade& at(size_type global);
    const Trade& at(size_type global) const;

    Trade& back();
    const Trade& back() const;

    /// Append at the logical global end (3.4:613-616).
    void push_back(const Trade& trade);
    void push_back(Trade&& trade);

    /// Resize the unsealed tail using a global end >= base() (3.4:614).
    /// Rewinding below base() is an engine_invariant (3.4:664-665).
    void resize(size_type global_end);

    /// Iterate only resident rows, in global-index order (3.4:615).
    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

    /// Retire [base(),global_end) after publication (3.4:622-665).
    /// This is the storage operation; its caller owns the following proof.
    /// Once per script bar, AFTER same-bar sorting and BEFORE the script
    /// body, choose the longest prefix whose every row i satisfies:
    /// 1. i < closed_rows_final_ (applied notification returned).
    /// 2. i < S-1, S being the first row of the last exit-time block; keep
    ///    that whole block AND its predecessor. No predecessor => no prefix.
    /// 3. exit_time < the current script bar's open.
    /// 4. It is not the materialize_long incarnation of a short-seed plan
    ///    that can still arm: final_short.incarnation != 0 and qty type is
    ///    not FIXED. Checking .active instead is insufficient.
    /// 5. i < source_money_folded_.
    ///
    /// Before dropping storage: fold the closed-row digest through the end;
    /// fold metrics, streamed PnL and all/winning/losing percent sums/counts
    /// in the same left-fold order from 0.0; write each T record; and spill
    /// any row the current position's backwards qty_percent scan can reach.
    /// Preserve each digest/money/exit-phase fold's state at the new base;
    /// seal no farther than exit_phase_final_. Prefix/phase storage below
    /// base is retired too, and restart/Debug re-folds start at base with
    /// those seeds (3.4:639-663). The sink remains the all-row report.
    ///
    /// Position-tail spill is 30 B + entry_id per row, walked backwards
    /// after the resident tail in the same addition order. Truncate when
    /// the position-open mark changes or a sealed row predates that mark
    /// (3.4:672-688). No unbounded in-memory history is substituted for it.
    ///
    /// base() <= global_end <= size(); size()/global row numbering survive
    /// retirement. No predicate evaluation, folding or writing is defined
    /// in P00. The retaining exception path never calls this operation.
    void seal(size_type global_end);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // inline namespace engine_script_run_v19
} // namespace pineforge
