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
#include <vector>

namespace pineforge {

// --- KahanWindowSum (window_sum.hpp): ta.sma / math.sum arithmetic ---
// TradingView's sliding-window sum (ta.sma, math.sum): a Kahan-compensated
// RUNNING sum S with compensation c. On a bar the oldest source leaves and
// then the new source enters, each through one Kahan step; ta.sma divides S
// by `length`.
//
// What leaves the window is the SOURCE (pinned 2026-11-16). The tape is a
// deep backtest of BINANCE:BTCUSDT 60 over 2024-01-01..2024-03-01 whose
// ta.sma(close, 3) starts on the first bar of the range and is exported as
// its exact double for 958 bars (the first of them na). On every bar checked
// -- the 39 averages of bars 2..59, which tests/test_ta_sma_price_tape.cpp
// replays, and eight more around bars 356, 899 and 1436 -- the average is the
// correctly rounded exact sum of the three sources divided by 3, also on the
// bars where that is one unit in the last place away from the correctly
// rounded mean (bar 4 is the first). With every source in one binade each
// compensation is applied exactly, so the Kahan steps below keep S - c equal
// to the exact sum of the window's sources and S is its rounding.
//
// The rule this replaces let the COMPENSATED addend y = x - c leave instead
// (fitted 2026-09-05, round 7). Whenever c had been applied exactly, c then
// stayed behind in S for good: on the tape above the average read one unit in
// the last place above TradingView's on bars 7 and 8 (the first bar is bar 0)
// and again on bar 53, where the order quantity sized from it was cut one lot
// step above TradingView's (45.20609 for 45.20608). The round-7 pulse tapes
// (OANDA:EURUSD 15; tests/test_ta_sma_window_residual.cpp) read the opposite
// on the same step: after a pulse an all-zero window keeps 2^-54 and then
// 2^-55 for 34 bars, and a 0.3 that entered under c = -2^-54 leaves three
// bars later as 0.30000000000000004. One arithmetic cannot produce both
// tapes. This file follows the later one; the rows of the September tapes
// that encode a compensated addend leaving are not reproduced by it.
//
// The other pieces are the fits of 2026-09-05 and 2026-09-06 (round-7
// synthetic pins, scratchpad r7/pins/sma-pulse-*, sma-ident-*: OANDA:EURUSD
// 15, pulse series of known doubles, lengths 3 and 20, three range starts;
// division by `length` confirmed against multiplication by its reciprocal,
// which misses ~3% of those decisions):
//
//   * the sub runs before the add on a bar (an add-then-sub ring misses from
//     bar 5), and a zero source still folds the compensation (y = -c);
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
//     12 events wrong). c is 0 after the re-sum;
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
// The counts in the round-7, 9 and 10 bullets above were measured with the
// compensated addend leaving the window; none of them has been measured again
// with the source leaving.
//
// The stochRSI consequence as measured in September (round-7 family D, probe
// A): k = sma(stoch, 3) and d = sma(k, 3) sit on ALGEBRAIC ties whenever stoch
// plateaus; TradingView resolved each tie by the residue the compensated
// addends had left in S, and ta.crossover fired on its exact sign. The earlier
// exact-order window re-sum (round 6) kept every such tie exact and could
// never fire; a plain running sum fired on the wrong half. With the source
// leaving the window that residue is no longer carried.
//
// Reference (Python, after r7/sma/sma-model.py):
//   c_pre = c
//   if t >= n: y = -x[n] - c; T = S + y; c = (T - S) - y; S = T
//   y = x - c; T = S + y; c = (T - S) - y; S = T
//   if c_pre != 0 and |fl(x + |c_pre|) - x| < |c_pre|:
//       S = (x + x[1]) + ... + x[n-1]; c = 0
//   sma = S / n

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

void KahanWindowSum::kahan_add(double v) {
    double y = v - comp_;
    double t = sum_ + y;
    comp_ = (t - sum_) - y;
    sum_ = t;
}

// The source enters the sum (its evicted predecessor already left), then the
// swallowed-compensation re-sum. `comp_before` is c as the bar found it --
// the rule reads the compensation carried INTO the bar, not the one left by
// the eviction (pinned: the block-63 re-sum happens although the eviction of
// a zero addend had just folded c to 0).
void KahanWindowSum::enter(double src, double comp_before) {
    double y = src - comp_;
    double t = sum_ + y;
    comp_ = (t - sum_) - y;
    sum_ = t;
    head_ = head_ ? head_ - 1 : window_.size() - 1;
    // The compensation corrects S as the source enters and is not remembered
    // with it: what this slot hands back when it leaves the window is the
    // source itself (the 2026-11-16 pin above), so S - c stays the sum of the
    // window's sources whenever the compensation was applied exactly.
    window_[head_] = {src, src};
    ++size_;
    if (comp_before != 0.0) {
        // Round-10 pin (family W, stage 2): the re-sum fires whenever the
        // MAGNITUDE of the carried compensation, ADDED to the incoming source,
        // is under-applied -- fl(src + |c|) landed closer to src than the exact
        // src + |c| would have (fully swallowed, the round-7 case; rounded
        // toward src, the round-9 case; or, on residue-only sources, the
        // rounding of src + |c| that the src - c form does not see). An exact
        // add, or one that rounds away from src, continues. Note the sign:
        // TradingView tests |c| added to src, whatever the sign of c, while the
        // arithmetic itself subtracts c -- the two agree except when src - c
        // and src + |c| round differently (residue-only sources, sources within
        // |c| of a power of two, negative sources).
        const double z = src + std::fabs(comp_before);
        if (std::fabs(z - src) < std::fabs(comp_before)) {
            double total = 0.0;
            for (std::size_t i = 0; i < size_; ++i) {   // newest first
                total += window_[index(i)].value;
            }
            sum_ = total;
            comp_ = 0.0;
        }
    }
}

double KahanWindowSum::push(double src) {
    // Growth is the only throwing step; finish it before changing either
    // the numerical state or the previous bar's rewind record.
    ensure_room();
    saved_sum_ = sum_;
    saved_comp_ = comp_;
    saved_count_ = count_;
    saved_pushed_ = true;
    const double comp_before = comp_;
    if (count_ >= length_) {
        // The window is full: the oldest source leaves before the new source
        // enters (sub-then-add order, pinned).
        const auto& evicted = window_[index(size_ - 1)];
        saved_evicted_value_ = evicted.value;
        saved_evicted_addend_ = evicted.addend;
        --size_;
        kahan_add(-saved_evicted_addend_);
    } else {
        saved_evicted_value_ = na<double>();
        saved_evicted_addend_ = na<double>();
    }
    enter(src, comp_before);
    count_++;
    return sum_;
}

void KahanWindowSum::note_no_push() {
    saved_pushed_ = false;
}

void KahanWindowSum::unpush() {
    if (!saved_pushed_) return;
    pop_front();
    if (!is_na(saved_evicted_addend_)) {
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
    // Rewind to the pre-bar (S, c) and replay the bar with the new source
    // against the same evicted addend; the ring fronts are overwritten in
    // place so the window shape is untouched.
    pop_front();
    sum_ = saved_sum_;
    comp_ = saved_comp_;
    count_ = saved_count_;
    const double comp_before = comp_;
    if (!is_na(saved_evicted_addend_)) {
        kahan_add(-saved_evicted_addend_);
    }
    enter(src, comp_before);
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
    : output_val(na<double>()), sum(0.0), length(length), bar_count(0),
      // Mirror the initial committed state so a recompute() issued before
      // the first compute() (e.g. the first partial sub-bar of a
      // lookahead request.security aggregation) restores a well-defined
      // pristine state instead of reading uninitialized save-state. Without
      // this, restore() reads indeterminate memory and can poison the RMA
      // with NaN non-deterministically.
      saved_output_val_(na<double>()), saved_sum_(0.0), saved_bar_count_(0) {}

double RMA::compute(double src) {
    save();
    if (is_na(src)) {
        return na<double>();
    }

    bar_count++;

    if (bar_count < length) {
        sum += src;
        return na<double>();
    } else if (bar_count == length) {
        sum += src;
        output_val = sum / length;
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
        output_val = alpha * src + (1.0 - alpha) * output_val;
        bar_count++;
        return output_val;
    }

    // EmaSeeding::FirstValue (the default):
    //   ema := na(ema[1]) ? src : alpha * src + (1 - alpha) * ema[1]
    // Seed from the first finite source value, so the output is never na.
    if (is_na(output_val)) {
        output_val = src;
        sum = src;
        bar_count = 1;
        return output_val;
    }

    output_val = alpha * src + (1.0 - alpha) * output_val;
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
    saved_sum_ = sum;
    saved_bar_count_ = bar_count;
}
void RMA::restore() {
    output_val = saved_output_val_;
    sum = saved_sum_;
    bar_count = saved_bar_count_;
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
