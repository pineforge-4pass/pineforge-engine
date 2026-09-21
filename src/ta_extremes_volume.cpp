/*
 * ta_extremes_volume.cpp — range extremes + cumulative + volume: Highest, Lowest, HighestBars, LowestBars, AllTimeMax, AllTimeMin, Median, Range, Mode, Cum, PivotHigh, PivotLow, OBV, AccDist, NVI, PVI, PVT, WAD, WVAD, III, VWAP
 *
 * Carved out of ta.cpp during the v0.1 file-split (phase 6) so the
 * 66-class TA library becomes navigable. Every class declared in
 * <pineforge/ta.hpp> is implemented in exactly one of the ta_*.cpp
 * partitions.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace pineforge {
namespace ta {


// --- Bar-addressed extremum ring (Highest / Lowest / HighestBars / LowestBars) ---
//
// TradingView's rule (see <pineforge/ta.hpp> bar_context()): a window call site
// owns K = length + 1 slots addressed by bar_index % K; a call on bar b writes
// slot[b % K] = src and reads the extremum over the slots (b - k) % K, k in
// [0, length). Every-bar callers get the positional window; an na input is
// written and POISONS the ring: the answer is the extremum over the members
// newer than the newest na slot (na on the na bar itself), and na while the
// window has not formed — pinned 2026-09-04 on NYSE:F 1D (every-bar
// ta.lowest / ta.highest over `bar_index % 4 == 0 ? na : low`, 20/20: equal
// on the bar after each gap, a single live member). Conditional callers
// (inside an ``if`` that does not run every bar) keep stale values in the
// slots they did not rewrite, read never-written slots as 0, and are na iff
// bar_index < length - 1 — pinned 2026-09-03 on NYSE:F 1D (cadence-7 39/39,
// cadence-9 30/30 entry sizes) and 2026-09-04 on BTCUSDT / XAUUSD 1D
// (cadence 13, every source kind 13/13).
// The slots are read through a cached extremum with an implied bar: a src
// that strictly beats the cache takes it without reading the slots (aged or
// not); otherwise an aged-out cache (by bars, not calls) rescans the slots
// oldest first and re-caches (best, bar - k_best); otherwise the cache is
// kept, leaving aliased stale slots unread — pinned 2026-09-04 on NYSE:F 1D
// (8 tapes 696/696: t382 `m = bar_index % 49; m < 4 or 37 <= m < 47 or
// m == 48`, ta.lowest(low, 10) -> bars 48..51 keep 9.88 over bar 3's
// aliased 9.20, bar 52 rescans to 9.20), BINANCE:ETHUSDT.P 15 (82/82) and
// the jayentriken BBWP ETH replay (593/593; the plain ring 587).

BarContext& bar_context() {
    // Thread-local so parallel in-process engines never cross-contaminate;
    // default "not installed" keeps standalone objects on their own cadence.
    static thread_local BarContext ctx;
    return ctx;
}

BarContextScope::BarContextScope(long long bar_index, long long origin)
    : previous_(bar_context()) {
    BarContext& ctx = bar_context();
    ctx.installed = true;
    ctx.bar_index = bar_index;
    ctx.origin = origin;
}

BarContextScope::~BarContextScope() {
    bar_context() = previous_;
}

namespace {

inline std::size_t ring_slot(long long bar, long long K) {
    long long m = bar % K;
    if (m < 0) m += K;
    return static_cast<std::size_t>(m);
}

}  // namespace

ExtremeRing::ExtremeRing(int length)
    : length_(length),
      values_(length > 0 ? static_cast<std::size_t>(length) + 1 : 0, na<double>()),
      written_(length > 0 ? static_cast<std::size_t>(length) + 1 : 0, 0) {}

ExtremeRing::Result ExtremeRing::update(double src, bool advance, bool want_max) {
    Result out{na<double>(), 0};
    if (length_ <= 0) return out;

    long long bar;
    long long origin;
    const BarContext& ctx = bar_context();
    if (ctx.installed) {
        bar = ctx.bar_index;
        origin = ctx.origin;
    } else {
        // No context: every compute() is its own bar and recompute() rewrites
        // the current one (a pristine recompute() counts as the first bar) —
        // the previous positional-deque cadence, byte for byte.
        if (advance || !has_written_) ++own_bar_;
        bar = own_bar_;
        origin = 0;
    }

    const long long K = static_cast<long long>(length_) + 1;
    const std::size_t cur = ring_slot(bar, K);
    values_[cur] = src;
    written_[cur] = 1;
    has_written_ = true;

    // The cached extremum {cval_, cbar_} is maintained on every call, warmup
    // included (r5g8: bar 13 answers bar 4's 9.00 cached while still na).
    // A recompute() on the bar the cache was last touched restores the
    // pre-bar cache first, so every tick of one bar re-applies from the same
    // starting point (magnifier compute -> recompute).
    if (bar != cache_seen_bar_) {
        cache_seen_bar_ = bar;
        saved_cached_ = cached_;
        saved_cval_ = cval_;
        saved_cbar_ = cbar_;
    } else {
        cached_ = saved_cached_;
        cval_ = saved_cval_;
        cbar_ = saved_cbar_;
    }
    const auto rescan = [&]() {
        // Oldest slot first with a strict comparison: ties resolve to the
        // oldest member, exactly as the deque scan did (only the *Bars
        // offsets can tell).
        double best = na<double>();
        int best_k = 0;
        for (int k = length_ - 1; k >= 0; --k) {
            const std::size_t i = ring_slot(bar - k, K);
            // Never written reads as 0 -- pinned 2026-09-04 on BINANCE:BTCUSDT
            // 1D (`if bar_index % 13 == 5: v = ta.lowest(low, 10)`: every call
            // answers 0 while the window holds never-written slots; the same
            // cadence's ta.highest(-close, 10) answers 0 too, ta.highest(high,
            // 10) is unaffected) and re-pinned on OANDA:XAUUSD 1D for every
            // source kind (low, close - 2000, sma(close, 5), low[1], hl2, the
            // sign-mixed sma(close, 5) - close: 13/13 each). Every scraped
            // tape in the population is a deep-backtest export that starts
            // cold at the range start, so this is the rule the campaign
            // grades against (the round-5 "skip" reading was wrong:
            // shiroi-qqe's stops on five 1D lanes are TV's SL = 0 from
            // exactly these reads).
            const double v = written_[i] ? values_[i] : 0.0;
            if (is_na(v)) {
                // A slot WRITTEN with na poisons the running extremum; the
                // next (newer) slot restarts it -- so the answer is the
                // extremum over the slots newer than the newest na slot in
                // the window, never-written 0s among them included, and na
                // when the newest slot is itself na (pinned 2026-09-04 on
                // OANDA:XAUUSD 1D: `ta.lowest(sma(close, 14), 10)` at cadence
                // 13 answers 0 on bars 31..70 -- never-written slots newer
                // than bar 5's na -- 3357.6 = src on bar 83 when the na slot
                // sits at k = 1, and 2998 on bars 109..148, the stale bar-18
                // write once it is newer than the na slot; leading-na sites
                // 7 x 13/13, the market-logic osc replica 32/32).
                best = na<double>();
                best_k = k;
                continue;
            }
            if (is_na(best) || (want_max ? v > best : v < best)) {
                best = v;
                best_k = k;
            }
        }
        cval_ = best;
        cbar_ = bar - best_k;    // the implied bar of an aliased slot
    };
    if (!cached_ || is_na(src)) {
        // The first call caches (src, b). An na src does the same on every
        // call: the na write POISONS the cache -- (na, b) -- so the call
        // answers na and the next valid call restarts from its own src
        // below, whatever the cache held and however young it was (pinned
        // 2026-09-04 on NYSE:F 1D, every-bar `x = bar_index % 4 == 0 ? na :
        // low`: ta.lowest(x, 5) == ta.highest(x, 5) on the bar after each na
        // bar, 20/20 -- bar 9 answers 9.20, its own low, where the untouched
        // 3-bar-old cache would have said 8.44; and on OANDA:XAUUSD 15 the
        // inserted `ta.lowest(na, 10)` calls answer na, 34/34).
        cached_ = true;
        cval_ = src;
        cbar_ = bar;
    } else if (is_na(cval_) || (want_max ? src > cval_ : src < cval_)) {
        // A strictly new extremum takes the cache without reading the slots,
        // aged-out cache or not (ETH bar 4990). Ties never displace it. A
        // poisoned (na) cache is taken by any valid src -- the restart: bar
        // 18 of the sma(close, 14) site answers 2998, its own src, although
        // the never-written slot one behind it would read 0 in a rescan.
        cval_ = src;
        cbar_ = bar;
    } else if (bar - cbar_ >= static_cast<long long>(length_)) {
        // The cached extremum has aged out of the window (by bars, not
        // calls): rescan the slots, aliasing and poison included.
        rescan();
    }
    // else: the cache is younger than `length` bars and src did not beat it.

    // na until the context has seen `length` bars (TV: bar_index < length - 1;
    // `origin` keeps a range-truncated feed warming up over its own bars).
    if (bar - origin < static_cast<long long>(length_) - 1) return out;
    out.value = cval_;
    out.bars_back = static_cast<int>(bar - cbar_);
    return out;
}

// --- Highest ---

Highest::Highest(int length) : ring_(length) {}

double Highest::compute(double src) {
    return ring_.update(src, /*advance=*/true, /*want_max=*/true).value;
}

double Highest::recompute(double src) {
    return ring_.update(src, /*advance=*/false, /*want_max=*/true).value;
}

// --- Lowest ---

Lowest::Lowest(int length) : ring_(length) {}

double Lowest::compute(double src) {
    return ring_.update(src, /*advance=*/true, /*want_max=*/false).value;
}

double Lowest::recompute(double src) {
    return ring_.update(src, /*advance=*/false, /*want_max=*/false).value;
}

// --- PivotHigh ---

PivotHigh::PivotHigh(int left_bars, int right_bars)
    : left_bars_(left_bars), right_bars_(right_bars) {}

double PivotHigh::compute(double src) {
    buffer_.push_back(src);

    int total = left_bars_ + right_bars_ + 1;
    while ((int)buffer_.size() > total) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < total) {
        return na<double>();
    }

    // The pivot candidate is at index left_bars_ (0-indexed)
    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) {
        return na<double>();
    }

    // Pivot candidate rules (validated against TradingView's `ta.pivothigh`
    // semantics by exporting per-bar pivot_high values from a TV indicator
    // and diffing against this engine).
    //
    // LEFT side: equal-high left bars are allowed (non-strict). A run of
    // identical highs to the left of the candidate should still confirm
    // the right-most one as the pivot — TV reports the pivot exactly one
    // bar after each flat-top, never on the first bar of a flat-top run.
    //
    // RIGHT side: equal-high right bars invalidate the candidate (strict).
    // While the flat-top continues to the right, no pivot has yet been
    // confirmed; the pivot is reported only on the bar AFTER the
    // flat-top finishes.
    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] > pivot) {
            return na<double>();
        }
    }

    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] >= pivot) {
            return na<double>();
        }
    }

    return pivot;
}

// --- PivotLow ---

PivotLow::PivotLow(int left_bars, int right_bars)
    : left_bars_(left_bars), right_bars_(right_bars) {}

double PivotLow::compute(double src) {
    buffer_.push_back(src);

    int total = left_bars_ + right_bars_ + 1;
    while ((int)buffer_.size() > total) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < total) {
        return na<double>();
    }

    // The pivot candidate is at index left_bars_ (0-indexed)
    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) {
        return na<double>();
    }

    // Mirror of PivotHigh — see that function's comment for the TV-empirical
    // rule rationale. LEFT non-strict (equal lows on left allowed), RIGHT
    // strict (equal lows on right invalidate). Pivot lows confirm exactly
    // one bar after each flat-bottom run.
    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] < pivot) {
            return na<double>();
        }
    }

    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] <= pivot) {
            return na<double>();
        }
    }

    return pivot;
}

// --- Cum (Cumulative Sum) ---

// saved_sum_ mirrors the initial committed sum_ (see RMA::RMA) so a
// recompute() before the first compute() restores a well-defined pristine
// state instead of reading uninitialized save-state.
Cum::Cum() : sum_(0.0), saved_sum_(0.0) {}

double Cum::compute(double src) {
    saved_sum_ = sum_;
    if (is_na(src)) {
        return sum_;
    }
    sum_ += src;
    return sum_;
}

// --- All-time max/min (chart series) ---

// saved_* mirror the initial committed state (see RMA::RMA) so a recompute()
// before the first compute() restores a well-defined pristine state.
AllTimeMax::AllTimeMax()
    : max_(na<double>()), has_(false),
      saved_max_(na<double>()), saved_has_(false) {}

double AllTimeMax::compute(double src) {
    saved_max_ = max_;
    saved_has_ = has_;
    if (is_na(src)) {
        return has_ ? max_ : na<double>();
    }
    if (!has_) {
        max_ = src;
        has_ = true;
    } else if (src > max_) {
        max_ = src;
    }
    return max_;
}

// saved_* mirror the initial committed state (see RMA::RMA) so a recompute()
// before the first compute() restores a well-defined pristine state.
AllTimeMin::AllTimeMin()
    : min_(na<double>()), has_(false),
      saved_min_(na<double>()), saved_has_(false) {}

double AllTimeMin::compute(double src) {
    saved_min_ = min_;
    saved_has_ = has_;
    if (is_na(src)) {
        return has_ ? min_ : na<double>();
    }
    if (!has_) {
        min_ = src;
        has_ = true;
    } else if (src < min_) {
        min_ = src;
    }
    return min_;
}

// --- Median ---

Median::Median(int length) : length_(length) {}

double Median::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_) {
        return na<double>();
    }

    // Copy and sort
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());

    int n = (int)sorted.size();
    if (n % 2 == 1) {
        return sorted[n / 2];
    } else {
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    }
}

// --- HighestBars ---
// Same ring as Highest; the result is the slot distance k of the extremum
// (0 = the current bar, negative offsets as TV reports them). An na input is
// written (it poisons the ring) and answers na on that bar; afterwards the
// offset is the bars-since of the extremum among the non-poisoned members --
// 0 on the restart bar, the rescan's k (a never-written 0 slot included)
// once the cache has aged.

HighestBars::HighestBars(int length) : ring_(length) {}

double HighestBars::compute(double src) {
    const ExtremeRing::Result r = ring_.update(src, /*advance=*/true, /*want_max=*/true);
    if (is_na(src) || is_na(r.value)) return na<double>();
    return -static_cast<double>(r.bars_back);
}

double HighestBars::recompute(double src) {
    const ExtremeRing::Result r = ring_.update(src, /*advance=*/false, /*want_max=*/true);
    if (is_na(src) || is_na(r.value)) return na<double>();
    return -static_cast<double>(r.bars_back);
}

// --- LowestBars ---

LowestBars::LowestBars(int length) : ring_(length) {}

double LowestBars::compute(double src) {
    const ExtremeRing::Result r = ring_.update(src, /*advance=*/true, /*want_max=*/false);
    if (is_na(src) || is_na(r.value)) return na<double>();
    return -static_cast<double>(r.bars_back);
}

double LowestBars::recompute(double src) {
    const ExtremeRing::Result r = ring_.update(src, /*advance=*/false, /*want_max=*/false);
    if (is_na(src) || is_na(r.value)) return na<double>();
    return -static_cast<double>(r.bars_back);
}

// --- OBV ---
double OBV::compute(double close, double volume) {
    saved_sum_ = sum_;
    saved_prev_close_ = prev_close_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        bar_count_++;
        return 0.0;
    }
    if (close > prev_close_) sum_ += volume;
    else if (close < prev_close_) sum_ -= volume;
    prev_close_ = close;
    bar_count_++;
    return sum_;
}

// --- AccDist ---
double AccDist::compute(double high, double low, double close, double volume) {
    saved_sum_ = sum_;
    if (is_na(high) || is_na(low) || is_na(close) || is_na(volume)) return na<double>();
    double range = high - low;
    if (range == 0.0) return sum_;
    double mfm = ((close - low) - (high - close)) / range;
    sum_ += mfm * volume;
    return sum_;
}

// --- NVI ---
double NVI::compute(double close, double volume) {
    saved_nvi_ = nvi_;
    saved_prev_close_ = prev_close_;
    saved_prev_volume_ = prev_volume_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        prev_volume_ = volume;
        bar_count_++;
        return nvi_;
    }
    if (volume < prev_volume_ && prev_close_ != 0.0) {
        nvi_ += (close - prev_close_) / prev_close_ * nvi_;
    }
    prev_close_ = close;
    prev_volume_ = volume;
    bar_count_++;
    return nvi_;
}

// --- PVI ---
double PVI::compute(double close, double volume) {
    saved_pvi_ = pvi_;
    saved_prev_close_ = prev_close_;
    saved_prev_volume_ = prev_volume_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        prev_volume_ = volume;
        bar_count_++;
        return pvi_;
    }
    if (volume > prev_volume_ && prev_close_ != 0.0) {
        pvi_ += (close - prev_close_) / prev_close_ * pvi_;
    }
    prev_close_ = close;
    prev_volume_ = volume;
    bar_count_++;
    return pvi_;
}

// --- PVT ---
double PVT::compute(double close, double volume) {
    saved_pvt_ = pvt_;
    saved_prev_close_ = prev_close_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (is_na(prev_close_)) { prev_close_ = close; return pvt_; }
    if (prev_close_ != 0.0) {
        pvt_ += ((close - prev_close_) / prev_close_) * volume;
    }
    prev_close_ = close;
    return pvt_;
}

// --- WAD ---
double WAD::compute(double high, double low, double close) {
    saved_wad_ = wad_;
    saved_prev_close_ = prev_close_;
    if (is_na(high) || is_na(low) || is_na(close)) return na<double>();
    if (is_na(prev_close_)) { prev_close_ = close; return 0.0; }
    double true_high = std::max(high, prev_close_);
    double true_low = std::min(low, prev_close_);
    double gain = 0.0;
    if (close > prev_close_) gain = close - true_low;
    else if (close < prev_close_) gain = close - true_high;
    wad_ += gain;
    prev_close_ = close;
    return wad_;
}

// --- WVAD ---
double WVAD::compute(double open, double high, double low, double close, double volume) {
    if (is_na(open) || is_na(high) || is_na(low) || is_na(close) || is_na(volume))
        return na<double>();
    double range = high - low;
    if (range == 0.0) return 0.0;
    return (close - open) / range * volume;
}

// --- III ---
// Intraday Intensity Index. Pine's `ta.iii` per-bar formula
// (per the official reference manual):
//   ((close - low) - (high - close)) / (high - low) * volume
// = (2*close - high - low) / (high - low) * volume.
// Volume MULTIPLIES the close-position ratio; the previous version
// divided, producing values ~1e-6 instead of TV's ~1e+4 magnitudes
// (caught by the TA correctness sweep against TV — every bar diverged).
double III::compute(double high, double low, double close, double volume) {
    if (is_na(high) || is_na(low) || is_na(close) || is_na(volume))
        return na<double>();
    double range = high - low;
    if (range == 0.0) return 0.0;
    return (2.0 * close - high - low) / range * volume;
}

// --- VWAP ---
// Pine v6 `ta.vwap(source)` defaults to a Daily anchor — the cumulator
// resets at the start of every SESSION day of the symbol (the UTC day on
// a 24x7 UTC symbol; 17:00 ET on OANDA forex; 09:30 ET RTH on NASDAQ
// equities — `anchor = timeframe.change("1D")` evaluates on the symbol's
// own daily bar). Earlier the engine treated VWAP as a single chart-wide
// cumulator, which produced values that drifted from TV by ~30% on
// intra-day bars; then it reset on UTC-day boundaries, which is right for
// the crypto corpus only. The tz-less overloads keep that UTC keying; the
// tz/session overloads key on session_day_index, which is the identical
// integer division for tz="UTC" + no session. `anchor_day_` is initialised
// lazily on the first non-NA bar.
void VWAP::roll_anchor(int64_t day) {
    if (anchor_day_ == std::numeric_limits<int64_t>::min()) {
        anchor_day_ = day;
    } else if (day != anchor_day_) {
        cum_pv_ = 0.0;
        cum_vol_ = 0.0;
        cum_pv_sq_ = 0.0;
        anchor_day_ = day;
    }
}

double VWAP::compute(double src, double volume, int64_t timestamp_ms) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return na<double>();
    roll_anchor(timestamp_ms / kMsPerDay);
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return na<double>();
    return cum_pv_ / cum_vol_;
}

double VWAP::compute(double src, double volume, int64_t timestamp_ms,
                     const std::string& tz, const std::string& session) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return na<double>();
    roll_anchor(session_day_index(timestamp_ms, tz, session));
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return na<double>();
    return cum_pv_ / cum_vol_;
}

VWAPBandsResult VWAP::compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult,
                                    const std::string& tz, const std::string& session) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return {na<double>(), na<double>(), na<double>()};
    roll_anchor(session_day_index(timestamp_ms, tz, session));
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return {na<double>(), na<double>(), na<double>()};
    double mean = cum_pv_ / cum_vol_;
    double variance = cum_pv_sq_ / cum_vol_ - mean * mean;
    if (variance < 0.0) variance = 0.0;  // guard against floating-point underflow
    double stdev = std::sqrt(variance);
    double band_offset = stdev_mult * stdev;
    return {mean, mean + band_offset, mean - band_offset};
}

VWAPBandsResult VWAP::compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return {na<double>(), na<double>(), na<double>()};
    roll_anchor(timestamp_ms / kMsPerDay);
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return {na<double>(), na<double>(), na<double>()};
    double mean = cum_pv_ / cum_vol_;
    double variance = cum_pv_sq_ / cum_vol_ - mean * mean;
    if (variance < 0.0) variance = 0.0;  // guard against floating-point underflow
    double stdev = std::sqrt(variance);
    double band_offset = stdev_mult * stdev;
    return {mean, mean + band_offset, mean - band_offset};
}

// --- Mode ---
double Mode::compute(double src) {
    if (is_na(src)) return na<double>();
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    std::unordered_map<double, int> counts;
    for (auto v : buffer_) counts[v]++;
    int max_count = 0;
    double mode_val = na<double>();
    for (auto& [val, cnt] : counts) {
        if (cnt > max_count || (cnt == max_count && (is_na(mode_val) || val < mode_val))) {
            max_count = cnt;
            mode_val = val;
        }
    }
    return mode_val;
}

// --- Range ---
double Range::compute(double src) {
    double h = highest_.compute(src);
    double l = lowest_.compute(src);
    if (is_na(h) || is_na(l)) return na<double>();
    return h - l;
}

// --- PivotHigh ---
// Recompute mirrors compute(): LEFT non-strict (`>` rejects), RIGHT strict
// (`>=` rejects). Both must stay in lockstep — a delta here would surface as
// `recompute != compute` in test_pivothigh_recompute_matches_compute.
double PivotHigh::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int total = left_bars_ + right_bars_ + 1;
    if ((int)buffer_.size() < total) return na<double>();

    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) return na<double>();

    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] > pivot) return na<double>();
    }
    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] >= pivot) return na<double>();
    }
    return pivot;
}

// --- PivotLow ---
// Recompute mirrors compute(): LEFT non-strict (`<` rejects), RIGHT strict
// (`<=` rejects). See PivotHigh::recompute() comment for invariant rationale.
double PivotLow::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int total = left_bars_ + right_bars_ + 1;
    if ((int)buffer_.size() < total) return na<double>();

    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) return na<double>();

    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] < pivot) return na<double>();
    }
    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] <= pivot) return na<double>();
    }
    return pivot;
}

// --- Cum ---
double Cum::recompute(double src) {
    sum_ = saved_sum_;
    return compute(src);
}

// --- AllTimeMax ---
double AllTimeMax::recompute(double src) {
    max_ = saved_max_;
    has_ = saved_has_;
    return compute(src);
}

// --- AllTimeMin ---
double AllTimeMin::recompute(double src) {
    min_ = saved_min_;
    has_ = saved_has_;
    return compute(src);
}

// --- Median ---
double Median::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());

    int n = (int)sorted.size();
    if (n % 2 == 1) return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

// --- OBV ---
double OBV::recompute(double close, double volume) {
    sum_ = saved_sum_;
    prev_close_ = saved_prev_close_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- AccDist ---
double AccDist::recompute(double high, double low, double close, double volume) {
    sum_ = saved_sum_;
    return compute(high, low, close, volume);
}

// --- NVI ---
double NVI::recompute(double close, double volume) {
    nvi_ = saved_nvi_;
    prev_close_ = saved_prev_close_;
    prev_volume_ = saved_prev_volume_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- PVI ---
double PVI::recompute(double close, double volume) {
    pvi_ = saved_pvi_;
    prev_close_ = saved_prev_close_;
    prev_volume_ = saved_prev_volume_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- PVT ---
double PVT::recompute(double close, double volume) {
    pvt_ = saved_pvt_;
    prev_close_ = saved_prev_close_;
    return compute(close, volume);
}

// --- WAD ---
double WAD::recompute(double high, double low, double close) {
    wad_ = saved_wad_;
    prev_close_ = saved_prev_close_;
    return compute(high, low, close);
}

// --- WVAD (stateless) ---
double WVAD::recompute(double open, double high, double low, double close, double volume) {
    return compute(open, high, low, close, volume);
}

// --- III (stateless) ---
double III::recompute(double high, double low, double close, double volume) {
    return compute(high, low, close, volume);
}

// --- VWAP ---
double VWAP::recompute(double src, double volume, int64_t timestamp_ms) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute(src, volume, timestamp_ms);
}

VWAPBandsResult VWAP::recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute_bands(src, volume, timestamp_ms, stdev_mult);
}

double VWAP::recompute(double src, double volume, int64_t timestamp_ms,
                       const std::string& tz, const std::string& session) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute(src, volume, timestamp_ms, tz, session);
}

VWAPBandsResult VWAP::recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult,
                                      const std::string& tz, const std::string& session) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute_bands(src, volume, timestamp_ms, stdev_mult, tz, session);
}

// --- Mode ---
double Mode::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    std::unordered_map<double, int> counts;
    for (auto v : buffer_) counts[v]++;
    int max_count = 0;
    double mode_val = na<double>();
    for (auto& [val, cnt] : counts) {
        if (cnt > max_count || (cnt == max_count && (is_na(mode_val) || val < mode_val))) {
            max_count = cnt;
            mode_val = val;
        }
    }
    return mode_val;
}

// --- Range ---
double Range::recompute(double src) {
    double h = highest_.recompute(src);
    double l = lowest_.recompute(src);
    if (is_na(h) || is_na(l)) return na<double>();
    return h - l;
}

} // namespace ta
} // namespace pineforge
