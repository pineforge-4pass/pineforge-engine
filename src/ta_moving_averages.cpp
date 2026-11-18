/*
 * ta_moving_averages.cpp — moving averages: RMA, SMA, EMA, WMA, HMA, VWMA, ALMA, SWMA
 *
 * Carved out of ta.cpp during the v0.1 file-split (phase 6) so the
 * 66-class TA library becomes navigable. Every class declared in
 * <pineforge/ta.hpp> is implemented in exactly one of the ta_*.cpp
 * partitions.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include "runtime_ambient.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace pineforge {

// --- KahanWindowSum (window_sum.hpp): ta.sma / math.sum arithmetic ---
// TradingView's sliding-window sum (ta.sma, math.sum) is the exact sum of the
// sources in the window, rounded once. Nothing of a source that has left the
// window stays in it: an all-zero window reads exactly 0 and a window of one
// repeated value reads its exact multiple, whatever passed through before.
// ta.sma divides that sum by `length`.
//
// Pinned 2026-11-17 on TradingView's own exact doubles (a capture
// of a stochRSI strategy on OANDA:EURUSD 15 and OANDA:XAUUSD 15, k =
// ta.sma(stoch, 3) and d = ta.sma(k, 3) on 23 bars): every k and every d is
// the exact sum of its three sources, rounded once, divided by 3. Among them
// d = 0 over k = 0, 0, 0 on the bar where the last non-zero k left the window,
// and d = 100 over k = 100, 100, 100. A window re-added from zero, newest first
// or oldest first, misses some of them by one unit in the last place, and a
// running sum that keeps a rounding of what left the window misses most of
// them. The pulse capture of
// tests/test_ta_sma_window_residual.cpp says the same on synthetic series: its
// all-zero windows read 0.
//
// How it is carried: sum_ is the exact sum rounded to nearest and comp_ is what
// that rounding left out, so sum_ + comp_ is the exact sum (add_exact below
// keeps it so with error-free additions). When the exact sum needs more than
// two doubles the pair can not hold it; the window is then summed exactly from
// its own sources (resum below) on every bar until two doubles suffice again.
// Either way the emitted sum is a function of the window alone.
//
// The rule this replaces is kept below as the record of what TradingView read
// in September 2026. TradingView's own values changed between the two
// captures: its earlier trade lists of the two charts above follow the
// September rule row for row, its lists of 2026-11-17 do not.
//
// The September rule (ta.sma, math.sum), fitted 2026-09-05 on
// the round-7 synthetic pins (scratchpad r7/pins/sma-pulse-*, sma-ident-*:
// OANDA:EURUSD 15, pulse series of known doubles, lengths 3 and 20, three
// range starts; 14,194 pulse decisions and 31,140 identity checks reproduced
// bit-for-bit, division by `length` confirmed against multiplication by its
// reciprocal). The tape facts that pin each piece:
//
//   * an all-zero window does NOT read 0: the residue of the values that left
//     the window persists (1.9e-17, 9e-18, ... over 34 zero bars), so the sum
//     is RUNNING, not a re-sum of the window;
//   * the residue changes again exactly n and 2n bars after a pulse, i.e. the
//     ring subtracts n bars later what Kahan's compensation made the value
//     ACTUALLY contribute (y = x - c), not the raw value;
//   * the sub runs before the add on a bar (an add-then-sub ring misses from
//     bar 5), and a zero addend still folds the compensation (y = -c);
//   * on the bar whose incoming value swallows the carried compensation
//     (c != 0 and fl(x - c) == x -- the compensation is below half an ulp of
//     x), the emitted sum is the newest-first direct re-sum of the window
//     with c reset (block 63 of both tapes drops an 18-unit / 182-unit
//     residue to exactly 1.1 there; block 69 reads (0.7 + 0.3) + 0.7000000000000001
//     = 1.7000000000000002, the oldest-first order gives 1.7);
//   * round 9 (family W, stochRSI probe A on six charts) generalised that
//     trigger on a full-coverage NYSE:F@15 oracle (pineforge-workflow
//     famw-f15-kdhi-*: 7,035 bars of math.sum(stoch, 3), k, math.sum(k, 3),
//     d, every value exported as its exact double): the re-sum fires whenever
//     the compensation is UNDER-applied to the incoming source, i.e. with
//     y0 = fl(x - c), |y0 - x| < |c| -- fully swallowed (round 7) or rounded
//     toward x (c = 1.25 ulp(x) applied as 1 ulp; a 1.5-ulp tie resolved
//     toward x). An exact subtraction or one rounded away from x continues.
//     Bit-exact 7,018/7,018 on math.sum(stoch, 3) (round-7 trigger 6,849) and
//     every k vs d sign / ta.crossover event on the lane (round 7: 24 signs,
//     12 events wrong). The re-summed bar stores its raw source as its ring
//     addend (c is 0 after the re-sum);
//   * round 10 (family W, stage 2: stochRSI probe B on NYSE:F@15, whose
//     d = sma(k, 3) sits on residue-only plateaus |k| ~ 1e-15 ~ |c|) closed
//     that pin: the quantity TradingView tests is src + |c|, the compensation's
//     MAGNITUDE added to the source -- fire iff |fl(x + |c|) - x| < |c|. On
//     ordinary sources (|c| << |x|) x - c and x + |c| round alike, so the
//     round-9 form was exact there; on residue-only sources, on sources within
//     |c| of a power of two, and on negative sources the two forms differ and
//     TradingView follows x + |c| (its decision flips under negation of the
//     series because x + |c| is not odd in x). Pinned on 21,509 labelled
//     decisions: the two NYSE:F@15 stochRSI oracles (probe A's ta.stoch and
//     probe B's literal stoch: math.sum(stoch, 3) 7,018/7,018 and math.sum(k, 3)
//     7,016/7,016 each, bit-exact), the stdev sums 7,029/7,029 x2, and 1,003
//     literal-replay decisions (pineforge-workflow r9-famW-5, lab tv w5-scan*:
//     dense +-40 ulp neighbourhoods, sign mirrors, binade straddles, window
//     lengths 3 and 5).
//
// The stochRSI consequence (round-7 family D, probe A): k = sma(stoch, 3)
// and d = sma(k, 3) sit on ALGEBRAIC ties whenever stoch plateaus; TradingView
// resolves each tie by this arithmetic's residue and ta.crossover fires on its
// exact sign. The previous exact-order window re-sum (round 6) kept every such
// tie exact and could never fire; a plain running sum fires on the wrong
// half. In the captures of 2026-11-17 those ties are exact and neither cross
// fires on them.
//
// Reference of the September rule (Python):
//   c_pre = c
//   if t >= n: y = -y[t-n] - c; T = S + y; c = (T - S) - y; S = T
//   y = x - c; T = S + y; c = (T - S) - y; S = T; store y
//   if c_pre != 0 and |fl(x + |c_pre|) - x| < |c_pre|:
//       S = (x + x[1]) + ... + x[n-1]; c = 0; store x instead of y
//   sma = S / n
//
// Reference of the rule in force (Python):
//   sma = math.fsum(x[t-n+1 .. t]) / n

namespace {

// a + b == s + e exactly, for finite a and b whose sum does not overflow
// (Knuth's two-sum; it needs no ordering of the magnitudes).
inline void two_sum(double a, double b, double& s, double& e) {
    s = a + b;
    const double b_seen = s - a;
    const double a_seen = s - b_seen;
    e = (a - a_seen) + (b - b_seen);
}

}  // namespace

KahanWindowSum::KahanWindowSum(int length)
    : length_(length), count_(0), sum_(0.0), comp_(0.0),
      saved_sum_(0.0), saved_comp_(0.0), saved_count_(0),
      saved_evicted_value_(na<double>()), saved_evicted_addend_(na<double>()),
      saved_pushed_(false) {}

void KahanWindowSum::ensure_room() {
    const auto limit = static_cast<std::size_t>(length_);
    if (size_ < window_.size() || size_ >= limit) return;
    const auto capacity = window_.size();
    const auto next = capacity == 0 ? std::min(limit, std::size_t{8})
        : capacity + std::min(capacity, limit - capacity);
    std::vector<Entry> replacement(next);
    for (std::size_t i = 0; i < size_; ++i) {
        replacement[i] = window_[index(i)];
    }
    window_.swap(replacement);
    head_ = 0;
}

// Adds v (a source entering, or minus a source leaving) to the carried pair,
// keeping sum_ + comp_ the exact sum and sum_ its rounding. Returns false, and
// leaves the pair as it was, when the new sum does not fit two doubles or is
// not finite.
bool KahanWindowSum::add_exact(double v) {
    double t;
    double e;
    two_sum(sum_, v, t, e);             // sum_ + v == t + e
    if (!std::isfinite(t)) return false;
    double low;
    double lost;
    two_sum(comp_, e, low, lost);       // comp_ + e == low + lost
    if (lost != 0.0) return false;
    two_sum(t, low, sum_, comp_);       // t + low == sum_ + comp_
    return true;
}

// The exact sum of the window's sources, rounded once, from the sources
// themselves (Shewchuk's expansion sum, the algorithm of Python's math.fsum).
// The partial sums are non-overlapping doubles, smallest first; there is at
// most one per source, so they live in the ring's spare slots. Returns true
// when the sum fits sum_ + comp_ exactly.
bool KahanWindowSum::resum() {
    std::size_t parts = 0;
    bool finite = true;
    for (std::size_t i = 0; i < size_ && finite; ++i) {
        double x = window_[index(i)].value;
        std::size_t kept = 0;
        for (std::size_t j = 0; j < parts; ++j) {
            double y = window_[index(j)].addend;
            if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
            const double high = x + y;
            const double low = y - (high - x);  // exact: |x| >= |y|
            if (low != 0.0) window_[index(kept++)].addend = low;
            x = high;
        }
        window_[index(kept++)].addend = x;
        parts = kept;
        finite = std::isfinite(x);
    }
    if (!finite) {
        // An infinite source, or a sum beyond the largest double: there is
        // no rounding to carry, and the sum is what IEEE addition gives.
        double total = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {   // newest first
            total += window_[index(i)].value;
        }
        sum_ = total;
        comp_ = 0.0;
        return false;
    }
    double high = 0.0;
    double low = 0.0;
    std::size_t rest = parts;
    if (rest > 0) {
        high = window_[index(--rest)].addend;
        while (rest > 0) {
            const double x = high;
            const double y = window_[index(--rest)].addend;
            high = x + y;
            low = y - (high - x);
            if (low != 0.0) break;
        }
        // Exactly halfway between two doubles, the partial sums still below
        // decide the direction (round half to even otherwise).
        if (rest > 0 && ((low < 0.0 && window_[index(rest - 1)].addend < 0.0)
                         || (low > 0.0 && window_[index(rest - 1)].addend > 0.0))) {
            const double twice = low * 2.0;
            const double up = high + twice;
            if (twice == up - high) high = up;
        }
    }
    sum_ = high;
    comp_ = low;
    return rest == 0;
}

// The source takes the front slot of the ring (its evicted predecessor has
// already left) and joins the sum. `carried` says the pair still holds the
// exact sum of the other sources; if it does not, or the source does not fit,
// the window is summed from its sources, and comp_ is na while two doubles do
// not hold that sum.
void KahanWindowSum::enter(double src, bool carried) {
    head_ = head_ ? head_ - 1 : window_.size() - 1;
    window_[head_] = {src, src};
    ++size_;
    if (carried && add_exact(src)) return;
    if (!resum()) comp_ = na<double>();
}

double KahanWindowSum::push(double src) {
    // Growth is the only throwing step; finish it before changing either
    // the numerical state or the previous bar's rewind record.
    ensure_room();
    saved_sum_ = sum_;
    saved_comp_ = comp_;
    saved_count_ = count_;
    saved_pushed_ = true;
    bool carried = !is_na(comp_);
    if (count_ >= length_) {
        // The window is full: the oldest source leaves, as the double it
        // entered as, before the new source enters.
        const auto& evicted = window_[index(size_ - 1)];
        saved_evicted_value_ = evicted.value;
        saved_evicted_addend_ = evicted.addend;
        --size_;
        carried = carried && add_exact(-saved_evicted_value_);
    } else {
        saved_evicted_value_ = na<double>();
        saved_evicted_addend_ = na<double>();
    }
    enter(src, carried);
    count_++;
    return sum_;
}

void KahanWindowSum::note_no_push() {
    saved_pushed_ = false;
}

void KahanWindowSum::unpush() {
    if (!saved_pushed_) return;
    pop_front();
    // A source is never na, so the saved source says whether the bar evicted
    // one (the spare slot beside it may hold anything).
    if (!is_na(saved_evicted_value_)) {
        window_[index(size_)] = {saved_evicted_value_, saved_evicted_addend_};
        ++size_;
    }
    sum_ = saved_sum_;
    comp_ = saved_comp_;
    count_ = saved_count_;
    saved_pushed_ = false;
}

double KahanWindowSum::repush(double src) {
    if (!saved_pushed_) {
        return push(src);
    }
    // Rewind to the pre-bar pair and replay the bar with the new source
    // against the same evicted source; the ring fronts are overwritten in
    // place so the window shape is untouched.
    pop_front();
    sum_ = saved_sum_;
    comp_ = saved_comp_;
    count_ = saved_count_;
    bool carried = !is_na(comp_);
    if (!is_na(saved_evicted_value_)) {
        carried = carried && add_exact(-saved_evicted_value_);
    }
    enter(src, carried);
    count_++;
    return sum_;
}

} // namespace pineforge

namespace pineforge {
namespace ta {

// Ambient default seeding for EMA instances that name none (see
// <pineforge/ta.hpp>). Per thread (internal::runtime_ambient: the thread's
// own block, or the one a running pump installed) so parallel in-process
// engines never cross-contaminate. Default false → EmaSeeding::FirstValue,
// byte-identical to the recursion before the option existed; a host raises it
// around the evaluation context whose instances should latch
// EmaSeeding::SimpleAverage.
bool& ema_na_warmup_flag() {
    return internal::runtime_ambient().ema_na_warmup;
}

RMA::RMA(int length)
    : output_val(na<double>()), length(length), bar_count(0), seed_window_(length),
      // Mirror the initial committed state so a recompute() issued before
      // the first compute() (e.g. the first partial sub-bar of a
      // lookahead request.security aggregation) restores a well-defined
      // pristine state instead of reading uninitialized save-state. Without
      // this, restore() reads indeterminate memory and can poison the RMA
      // with NaN non-deterministically.
      saved_output_val_(na<double>()), saved_bar_count_(0) {}

double RMA::compute(double src) {
    save();
    seed_pushed_ = false;
    if (is_na(src)) {
        return na<double>();
    }

    bar_count++;

    if (bar_count <= length) {
        // The seed is ta.sma of the first `length` valid sources: the exact sum
        // of the window, rounded once, over length. TradingView's own doubles
        // (tests/test_ta_rma_exact_seed.cpp: ta.rma(close, 3) on
        // BINANCE:ETHUSDT.P 15m) read that value, where a running sum of the
        // same sources is one unit in the last place away on some windows. The
        // window is the KahanWindowSum ta.sma uses, so the two agree bit for bit.
        const double window_sum = seed_window_.push(src);
        seed_pushed_ = true;
        if (bar_count < length) {
            return na<double>();
        }
        output_val = window_sum / length;
        return output_val;
    } else {
        // Pine reference formula (TradingView ta.rma):
        //   rma := (src + (length - 1) * rma[1]) / length
        // Use the same expression order as Pine to match per-bar values to
        // the last ULP. The mathematically equivalent form
        //   alpha*src + (1-alpha)*rma[1]   with alpha = 1/length
        // produces 1-3 ULP drift per bar that compounds across the series
        // and can flip RSI/ATR threshold-crossings on close calls. See
        // tests/test_ta_rma_warmup.cpp for fixed Pine-formula reference
        // values.
        output_val = (src + static_cast<double>(length - 1) * output_val)
                     / static_cast<double>(length);
        return output_val;
    }
}

// --- SMA ---

SMA::SMA(int length)
    : window_(length), length(length), bar_count(0) {}

double SMA::compute(double src) {
    if (is_na(src)) {
        // Pine ta.sma: an na input never enters the compact last-N-
        // valid window. Once `length` valid values have been seen the buffer
        // mean is HELD and re-emitted on every bar including na-input bars;
        // before seeding an na input is still na. State is left untouched.
        window_.note_no_push();
        if (bar_count >= length) {
            return window_.sum() / length;
        }
        return na<double>();
    }

    double sum = window_.push(src);
    bar_count++;

    if (bar_count < length) {
        return na<double>();
    }
    return sum / length;
}

// --- EMA ---

namespace {

// One step of ta.ema's recursion, shared by both seedings:
//     ema := ema[1] + alpha * (src - ema[1])
// in plain double arithmetic. The algebraically equal
//     alpha * src + (1 - alpha) * ema[1]
// is a different double on many bars, one unit in the last place away, and
// the difference stays in the series.
//
// Pinned 2026-11-17 on TradingView's own exact doubles (a capture
// on OANDA:EURUSD 15 and OANDA:XAUUSD 15 of macd = ta.ema(close, 12) -
// ta.ema(close, 26) and signal = ta.ema(macd, 9)): this form gives the logged
// signal from the logged previous signal and macd on all 20 single steps, and
// macd and signal on every logged bar from the start of the series. The other
// form misses 8 of the 20 steps; the same step with the product fused into the
// addition misses one.
inline double ema_step(double alpha, double src, double previous) {
    return previous + alpha * (src - previous);
}

}  // namespace

EMA::EMA(int length)
    : output_val(na<double>()), alpha(2.0 / (length + 1)), sum(0.0),
      bar_count(0), length_(length),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // issued before the first compute() restores a well-defined pristine
      // state instead of reading uninitialized save-state members.
      saved_output_val_(na<double>()), saved_sum_(0.0), saved_bar_count_(0) {}

EMA::EMA(int length, EmaSeeding seeding)
    : EMA(length) {
    // The instance names its seeding: the ambient default is never consulted.
    na_warmup_ = seeding == EmaSeeding::SimpleAverage;
    warmup_latched_ = true;
}

double EMA::compute(double src) {
    return compute(src, true);
}

double EMA::compute(double src, bool input_ready) {
    save();
    if (input_ready && std::isfinite(src) && ready_count_ < length_) {
        ++ready_count_;
    }
    // An instance that named no seeding latches the ambient default once, on
    // its first compute(), so a host that raises the default around one
    // evaluation context gets that context's instances SMA-seeded for life
    // without naming the seeding at each construction.
    if (!warmup_latched_) {
        na_warmup_ = ema_na_warmup_flag();
        warmup_latched_ = true;
    }
    if (is_na(src)) {
        // An na input neither updates nor resets the recursion: the output is
        // na on this bar and the recursion resumes over the finite inputs on
        // the next finite bar (the same rule as RMA::compute). State is
        // left untouched, so a SimpleAverage warm-up count is unaffected:
        // output_val is still na during warm-up, so the return is na either way.
        return na<double>();
    }

    if (na_warmup_) {
        // EmaSeeding::SimpleAverage: na until `length` finite inputs have
        // accumulated since the series start, then seed with their mean, then
        // run the ordinary recursion — the warm-up RMA::compute / SMA already
        // have, so a series truncated at its start reads na for its whole
        // warm-up window. Once output_val is finite the series is seeded.
        if (is_na(output_val)) {
            sum += src;
            bar_count++;
            if (bar_count < length_) {
                return na<double>();
            }
            // bar_count == length_: seed = SMA of the first `length_` values.
            output_val = sum / static_cast<double>(length_);
            return output_val;
        }
        output_val = ema_step(alpha, src, output_val);
        bar_count++;
        return output_val;
    }

    // EmaSeeding::FirstValue (the default):
    //   ema := na(ema[1]) ? src : ema[1] + alpha * (src - ema[1])
    // Seed from the first finite source value, so the output is never na.
    if (is_na(output_val)) {
        output_val = src;
        sum = src;
        bar_count = 1;
        return output_val;
    }

    output_val = ema_step(alpha, src, output_val);
    bar_count++;
    return output_val;
}

// --- WMA (Weighted Moving Average) ---

WMA::WMA(int length, WindowCacheCapacity cache)
    : length_(length), buffer_(static_cast<std::size_t>(length), cache) {}

double WMA::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    // Ring buffer is capacity-bounded at length_: push_front drops the
    // oldest sample automatically once full (the deque pop_front evict).
    buffer_.push_front(src);

    if ((int)buffer_.size() < length_) {
        return na<double>();
    }

    // Linear weights: oldest has weight 1, newest has weight length_.
    // buffer_ holds newest at offset 0, so the oldest sample (weight 1)
    // is at offset length_-1. Walk oldest→newest to keep the exact
    // accumulation order — parity is ULP-sensitive (WMA feeds HMA).
    double weighted_sum = 0.0;
    double weight_total = 0.0;
    for (int i = 0; i < length_; i++) {
        double weight = i + 1;
        weighted_sum += buffer_[static_cast<std::size_t>(length_ - 1 - i)] * weight;
        weight_total += weight;
    }
    return weighted_sum / weight_total;
}

// --- HMA (Hull Moving Average) ---
// Pine: ta.hma(src, length) = ta.wma(2 * ta.wma(src, length/2) - ta.wma(src, length), math.round(math.sqrt(length)))
// Empirically matches TV: length/2 is truncation, sqrt uses truncation too (not round),
// verified by `validation/05-hma-cross` matching 100% of TV trades with this behavior.

HMA::HMA(int length)
    : wma_half_(std::max(length / 2, 1)),
      wma_full_(length),
      wma_sqrt_(std::max((int)std::sqrt((double)length), 1)) {}

double HMA::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    double half_val = wma_half_.compute(src);
    double full_val = wma_full_.compute(src);

    if (is_na(half_val) || is_na(full_val)) {
        return na<double>();
    }

    double diff = 2.0 * half_val - full_val;
    return wma_sqrt_.compute(diff);
}

// --- VWMA (Volume-Weighted Moving Average) ---

VWMA::VWMA(int length) : length_(length), sv_sum_(0.0), v_sum_(0.0) {}

double VWMA::compute(double src, double vol) {
    if (is_na(src) || is_na(vol)) {
        return na<double>();
    }

    double sv = src * vol;
    sv_buffer_.push_back(sv);
    v_buffer_.push_back(vol);
    sv_sum_ += sv;
    v_sum_ += vol;

    while ((int)sv_buffer_.size() > length_) {
        sv_sum_ -= sv_buffer_.front();
        sv_buffer_.pop_front();
        v_sum_ -= v_buffer_.front();
        v_buffer_.pop_front();
    }

    if ((int)sv_buffer_.size() < length_) {
        return na<double>();
    }

    if (v_sum_ == 0.0) {
        return na<double>();
    }

    return sv_sum_ / v_sum_;
}

// ============================================================================
// ALMA (Arnaud Legoux Moving Average)
// ============================================================================

ALMA::ALMA(int length, double offset, double sigma, bool floor)
    : length_(length), offset_(offset), sigma_(sigma), floor_(floor) {
    if (length_ <= 0) return;

    weights_.resize(static_cast<std::size_t>(length_));
    double m = offset_ * (length_ - 1);
    if (floor_) m = std::floor(m);
    double s = length_ / sigma_;
    for (int i = 0; i < length_; i++) {
        // Keep this expression in the same order as the former per-call
        // calculation; only its lifetime changes.
        weights_[i] = std::exp(-((i - m) * (i - m)) / (2.0 * s * s));
    }
}

double ALMA::compute(double src) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    int sz = (int)buffer_.size();
    if (sz < length_) return na<double>();
    double norm = 0, sum = 0;
    for (int i = 0; i < length_; i++) {
        double w = weights_[i];
        norm += w;
        sum += buffer_[i] * w;
    }
    return sum / norm;
}

// ============================================================================
// SWMA (Symmetrically Weighted Moving Average, fixed period 4)
// ============================================================================

SWMA::SWMA() {}

double SWMA::compute(double src) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > 4) buffer_.pop_front();
    if ((int)buffer_.size() < 4) return na<double>();
    // Weights: 1/6, 2/6, 2/6, 1/6
    return (buffer_[0] + 2.0 * buffer_[1] + 2.0 * buffer_[2] + buffer_[3]) / 6.0;
}

// ============================================================================
// recompute() implementations
// ============================================================================
// --- RMA ---
void RMA::save() {
    saved_output_val_ = output_val;
    saved_bar_count_ = bar_count;
    // A bar starts here and nothing of it has entered the seed window. RSI, ATR
    // and DMI save their RMAs at the start of every bar and return before
    // computing them when the input is na; restore() on that bar must not take
    // the previous bar's source out of the window.
    seed_pushed_ = false;
}
void RMA::restore() {
    output_val = saved_output_val_;
    bar_count = saved_bar_count_;
    // A bar that entered the seed window leaves it again, so the window reads
    // as it did before the bar; a bar that did not (na, or after the seed)
    // leaves it alone.
    if (seed_pushed_) {
        seed_window_.unpush();
        seed_pushed_ = false;
    }
}
double RMA::recompute(double src) {
    restore();
    return compute(src);
}

// --- EMA ---
void EMA::save() {
    saved_output_val_ = output_val;
    saved_sum_ = sum;
    saved_bar_count_ = bar_count;
    saved_ready_count_ = ready_count_;
}
void EMA::restore() {
    output_val = saved_output_val_;
    sum = saved_sum_;
    bar_count = saved_bar_count_;
    ready_count_ = saved_ready_count_;
}
double EMA::recompute(double src) {
    return recompute(src, true);
}
double EMA::recompute(double src, bool input_ready) {
    restore();
    return compute(src, input_ready);
}

// --- SMA ---
double SMA::recompute(double src) {
    if (is_na(src)) {
        return na<double>();
    }
    if (window_.count() == 0) {
        return compute(src);
    }

    // Replay the bar with the new source: the same evicted addend, the same
    // pre-bar (sum, compensation), so a recompute()d bar and a freshly
    // computed one with an identical window agree bit-for-bit.
    double sum = window_.repush(src);

    if (bar_count < length) {
        return na<double>();
    }
    return sum / length;
}

// --- WMA ---
double WMA::recompute(double src) {
    if (buffer_.size() == 0) return compute(src);
    buffer_.update_front(src);  // overwrite newest in place (deque back())

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double weighted_sum = 0.0;
    double weight_total = 0.0;
    for (int i = 0; i < length_; i++) {
        double weight = i + 1;
        weighted_sum += buffer_[static_cast<std::size_t>(length_ - 1 - i)] * weight;
        weight_total += weight;
    }
    return weighted_sum / weight_total;
}

// --- HMA ---
double HMA::recompute(double src) {
    if (is_na(src)) return na<double>();

    double half_val = wma_half_.recompute(src);
    double full_val = wma_full_.recompute(src);

    if (is_na(half_val) || is_na(full_val)) {
        return na<double>();
    }

    double diff = 2.0 * half_val - full_val;
    return wma_sqrt_.recompute(diff);
}

// --- VWMA ---
double VWMA::recompute(double src, double vol) {
    if (sv_buffer_.empty()) return compute(src, vol);
    if (is_na(src) || is_na(vol)) return na<double>();

    double old_sv = sv_buffer_.back();
    double old_v = v_buffer_.back();
    double new_sv = src * vol;

    sv_buffer_.back() = new_sv;
    v_buffer_.back() = vol;
    sv_sum_ = sv_sum_ - old_sv + new_sv;
    v_sum_ = v_sum_ - old_v + vol;

    if ((int)sv_buffer_.size() < length_) return na<double>();
    if (v_sum_ == 0.0) return na<double>();
    return sv_sum_ / v_sum_;
}

// --- ALMA ---
double ALMA::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int sz = (int)buffer_.size();
    if (sz < length_) return na<double>();

    double norm = 0, sum = 0;
    for (int i = 0; i < length_; i++) {
        double w = weights_[i];
        norm += w;
        sum += buffer_[i] * w;
    }
    return sum / norm;
}

// --- SWMA ---
double SWMA::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;
    if ((int)buffer_.size() < 4) return na<double>();
    return (buffer_[0] + 2.0 * buffer_[1] + 2.0 * buffer_[2] + buffer_[3]) / 6.0;
}

} // namespace ta
} // namespace pineforge
