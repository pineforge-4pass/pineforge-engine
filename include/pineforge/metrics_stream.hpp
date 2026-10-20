#pragma once

// CR-150 P00 declarations only. DESIGN.md revision 6, section 3.3:513-567,
// with scratch storage in 3.8:902-909 and selected observations in 3.6:745-773.
// Existing array entry points in metrics.hpp remain unchanged.
#include <pineforge/metrics.hpp>

#include <cstdint>
#include <memory>

namespace pineforge {
namespace detail { class ScratchFile; }
namespace metrics {

PF_STATIC_ASSERT(sizeof(pf_equity_point_t) == 24);
PF_STATIC_ASSERT(offsetof(pf_equity_point_t, time_ms) == 0);
PF_STATIC_ASSERT(offsetof(pf_equity_point_t, equity) == 8);
PF_STATIC_ASSERT(offsetof(pf_equity_point_t, open_profit) == 16);

/// Report-only O(1) trade fold (DESIGN 3.3:515-533,547-553).
/// One accumulator per existing TradeFilter ALL/LONG/SHORT. Initialization
/// and filtering match compute_trade_stats; fold is exactly its loop body,
/// finish exactly its tail. Sums are plain left folds from 0.0 in row order,
/// with the existing NaN, positive-loss and even-trade conventions; no sort,
/// compensation or reuse of the engine's differently seeded running counters.
///
/// Fold sealed rows first, then finish a COPY with the unsealed tail and the
/// final, re-marked range-end rows (3.3:547-551). Copies preserve the prefix;
/// finish() never consumes or mutates it, so repeated report reads agree.
/// The implementations replace the old loop only in L7a; none are supplied
/// here. Report state is added to the broker hash waiver list by that lane,
/// not silently treated as new decision state (3.3:552-553).
class TradeStatsAccumulator {
public:
    TradeStatsAccumulator(TradeFilter filter, double initial_capital);
    ~TradeStatsAccumulator();
    TradeStatsAccumulator(const TradeStatsAccumulator&);
    TradeStatsAccumulator& operator=(const TradeStatsAccumulator&);

    void fold(const TradeC& trade);
    pf_trade_stats_t finish() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Array point source, the first source of 3.3:535-543. data stays caller-
/// owned and alive throughout the metric walk. Indexing returns a point
/// value, so callers do not keep references to a source's refillable window.
/// Required structural interface for BOTH sources: size() -> int64_t and
/// source[int64_t] -> pf_equity_point_t, with 0 <= index < size().
class ArrayCurve {
public:
    ArrayCurve(const pf_equity_point_t* data, std::int64_t point_count);
    std::int64_t size() const;
    pf_equity_point_t operator[](std::int64_t index) const;

private:
    const pf_equity_point_t* data_;
    std::int64_t point_count_;
};

/// A region of the engine-private scratch file (3.8:902-909). A point is
/// exactly the 24-byte binary pf_equity_point_t: time_ms, equity, open_profit.
/// This describes either a region or a separate file without choosing the
/// curve/position-tail placement that DESIGN leaves open (3.8:908).
/// Production regions borrow scratch and its lifetime, with descriptor=-1.
/// The descriptor form remains available for existing external file readers;
/// it is used only when scratch is null. Neither form owns its backing.
struct ScratchCurveRegion {
    int descriptor;
    std::int64_t byte_offset;
    std::int64_t point_count;
    const detail::ScratchFile* scratch = nullptr;
};

struct ScratchCurveReader; // positional reader contract: spill_curve_reader.hpp

/// Scratch point source (3.3:535-546; 3.8:904-909). Read exact binary doubles
/// through positional scratch I/O into ONE 64 KiB window; never map a curve
/// in the reader or parse sink text. Index i refers to byte_offset + 24*i.
/// The borrowed backing remains valid during the walk. State/cache is
/// opaque and O(1) in bars.
///
/// Required access pattern is first point, last point, then forward passes;
/// each pass may restart at index 0. This is not a destructive input stream.
/// Return by value keeps a previously read point independent of refills.
/// OPEN M1: DESIGN does not set malformed-region/bounds/short-read error
/// transport or concurrent-reader rules. No recovery policy is implied.
class SpillCurve {
public:
    explicit SpillCurve(const ScratchCurveRegion& region);
    /// Alternate read boundary for scratch adapters and file-like test readers.
    /// Copies the reader descriptor; its context must outlive this source.
    explicit SpillCurve(const ScratchCurveReader& reader);
    ~SpillCurve();
    std::int64_t size() const;
    pf_equity_point_t operator[](std::int64_t index) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Equity computation over the structural point-source interface above
/// (3.3:531-546). Same parameters, guards and formulas as the array overload
/// in metrics.hpp; only curve[i] goes through PointSource. n is the number
/// of points to walk (within source.size()), not an observation denominator.
/// Ordinary calls use observation_count=n; the second overload states it.
///
/// Five sequential walks, no growing vectors: drawdown/runup walk, generated
/// month-end returns twice, generated per-bar returns twice. Pass 1 sums and
/// counts; pass 2 accumulates variance/downside in today's exact order, with
/// today's final-n/span risk-free rate. Do not change grouping/rounding or
/// -ffp-contract=off (3.3:518-543). A non-UTC chart holds ONE ScopedTimezone
/// for the whole fill-time walk, never a nested lock (3.3:545-546).
/// These are template declarations, with no metric bodies/instantiations.
template <class PointSource>
pf_equity_stats_t compute_equity_stats(
    const PointSource& curve, std::int64_t n, double initial_capital,
    const std::string& chart_tz, double first_open, double last_close,
    std::int64_t bars_in_market, double net_profit);

/// Selected source has the final presented anchor and M window points:
/// n=M+1, observation_count=M (including M=0). The anchor is the first
/// return's base, not a bar. Only time_in_market_pct uses the observation
/// denominator, as the existing metrics.hpp overload documents
/// (3.6:745-749,769-773). Metrics read the scratch curve at each report read;
/// they do not consume it or change the engine's held point (3.2:499-509).
template <class PointSource>
pf_equity_stats_t compute_equity_stats(
    const PointSource& curve, std::int64_t n, double initial_capital,
    const std::string& chart_tz, double first_open, double last_close,
    std::int64_t bars_in_market, double net_profit,
    std::int64_t observation_count);

} // namespace metrics
} // namespace pineforge
