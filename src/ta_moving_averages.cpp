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
#include "ta_observation_count.hpp"
#include "window_sum_expansion.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pineforge {

// --- KahanWindowSum (window_sum.hpp): ta.sma / math.sum arithmetic ---
// Start with two zero doubles. First subtract the oldest source if the ring is
// full, then add the incoming source. Each step updates S and adds its two-sum
// error to C. The addition into C rounds. Emit S + C without feeding the
// result back into S; ta.sma divides that double by the length.
//
// November 2026, comparing TradingView's 15-minute source and output doubles:
//   * NYSE:F, length 3: all 1,491 stochastic sums matched this rule. Rounding
//     the exact window once matched 1,488, missing rows 211, 855 and 856.
//     Both rules matched all 1,489 sums of K.
//   * NYSE:F, length 7: both rules matched all 114 sums of closes and all
//     114 sums of squared closes.
//   * OANDA:EURUSD: dividing the compensated sum of K by 3 matched D on
//     all 5,641 rows. The exact rule was not compared on that series.
// This also gives zero on all 5,337 and 3,863 simulated all-zero pulse windows
// at lengths 3 and 20, consistent with the November 16–17 captures. Those
// captures alone did not distinguish the two rules.
//
// September 2026 record: TradingView then evicted each *contributed addend*.
// For both removal and insertion it applied y = v - c; t = S + y;
// c = (t - S) - y; S = t. The incoming y was saved for eviction. When the
// pre-bar correction was under-applied (|fl(x + |c_pre|) - x| < |c_pre|),
// it summed the window newest first, cleared c and stored raw x instead.
// It emitted S, dividing by length for SMA. That rule accounted for 14,194
// pulse decisions and 31,140 identity checks. Its residue after a pulse could
// decide a crossover on a plateau; those September results are historical.

KahanWindowSum::KahanWindowSum(int length)
    : length_(length), count_(0), sum_(0.0), comp_(0.0),
      saved_sum_(0.0), saved_comp_(0.0), saved_count_(0),
      saved_evicted_value_(na<double>()), saved_pushed_(false) {}

void KahanWindowSum::ensure_room() {
    const auto limit = static_cast<std::size_t>(length_);
    if (size_ < window_.size() || size_ >= limit) return;
    const auto capacity = window_.size();
    const auto next = capacity == 0 ? std::min(limit, std::size_t{8})
        : capacity + std::min(capacity, limit - capacity);
    std::vector<double> replacement(next);
    for (std::size_t i = 0; i < size_; ++i) {
        replacement[i] = window_[index(i)];
    }
    window_.swap(replacement);
    head_ = 0;
}

bool KahanWindowSum::add(double v) noexcept {
    return detail::add_window_value(sum_, comp_, v);
}

void KahanWindowSum::recover() noexcept {
    detail::recover_window_sum(sum_, comp_, exact_recovery_, size_,
                               [this](std::size_t i) { return window_[index(i)]; });
}

double KahanWindowSum::push(double src) {
    // Growth is the only throwing step; finish it before changing either
    // the numerical state or the previous bar's rewind record.
    ensure_room();
    saved_sum_ = sum_;
    saved_comp_ = comp_;
    saved_count_ = count_;
    saved_pushed_ = true;
    saved_exact_recovery_ = exact_recovery_;
    bool finite = !exact_recovery_;
    if (count_ >= length_) {
        // The window is full: the oldest source leaves, as the double it
        // entered as, before the new source enters.
        saved_evicted_value_ = window_[index(size_ - 1)];
        --size_;
        if (finite) finite = add(-saved_evicted_value_);
    } else {
        saved_evicted_value_ = na<double>();
    }
    head_ = head_ ? head_ - 1 : window_.size() - 1;
    window_[head_] = src;
    ++size_;
    if (!finite || !add(src)) recover();
    detail::advance_observation_count(count_);
    return sum();
}

void KahanWindowSum::note_no_push() {
    saved_pushed_ = false;
}

void KahanWindowSum::unpush() {
    if (!saved_pushed_) return;
    pop_front();
    // A source is never na, so the saved source says whether the bar evicted one.
    if (!is_na(saved_evicted_value_)) {
        window_[index(size_)] = saved_evicted_value_;
        ++size_;
    }
    sum_ = saved_sum_;
    comp_ = saved_comp_;
    count_ = saved_count_;
    exact_recovery_ = saved_exact_recovery_;
    saved_pushed_ = false;
}

double KahanWindowSum::repush(double src) {
    unpush();
    return push(src);
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
    // A length of zero or less has no seed window and no recursion: the
    // value is na on every bar. Answering here keeps `length - 1` below from
    // being evaluated at the smallest int, where the subtraction overflows.
    if (length <= 0) {
        return na<double>();
    }

    detail::advance_observation_count(bar_count);

    if (saved_bar_count_ < length) {
        // The seed is the first fixed SMA: push valid sources in order, then
        // divide the emitted sum by length. The pre-bar count distinguishes
        // the last seed input even when length is INT_MAX.
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

SMA::SMA(int length) : window_(length) {}

double SMA::compute(double src) {
    // A length of zero or less has no window: no source enters it (the
    // window sum takes a positive length only) and the value is na on every
    // bar.
    if (window_.length() <= 0) {
        return na<double>();
    }
    if (is_na(src)) {
        // A missing bar holds the seeded mean and consumes no slot.
        window_.note_no_push();
    } else {
        window_.push(src);
    }
    return window_.seeded() ? window_.sum() / window_.length() : na<double>();
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

double ema_seed_sum(const std::vector<double>& sources) {
    double partials[detail::kWindowSumPartialCapacity];
    std::size_t parts = 0;
    const auto partial = [&partials](std::size_t i) -> double& { return partials[i]; };
    std::size_t i = 0;
    for (; i < sources.size(); ++i) {
        if (!detail::expand_window_sum(sources[i], parts, partial)) break;
    }
    if (i == sources.size()) {
        const double sum = detail::round_window_sum(parts, partial).sum;
        if (std::isfinite(sum)) return sum + 0.0;
    }
    // The expansion could not represent the seed. Retry all retained
    // sources, including any cancellation after an overflowing prefix.
    double sum;
    detail::exact_or_ieee_window_sum(sum, sources.size(),
        [&](std::size_t back) { return sources[sources.size() - 1 - back]; });
    return sum;
}

}  // namespace

EMA::EMA(int length)
    : output_val(na<double>()), alpha(2.0 / (static_cast<double>(length) + 1.0)), sum(0.0),
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

    if (na_warmup_ && is_na(output_val) && bar_count < length_) {
        seed_sources_.push_back(src);
    }
    if (input_ready && std::isfinite(src) && ready_count_ < length_) {
        ++ready_count_;
    }
    if (na_warmup_) {
        // EmaSeeding::SimpleAverage: na until `length` non-na inputs have
        // accumulated since the series start, then seed with their mean, then
        // run the ordinary recursion — the warm-up RMA::compute / SMA already
        // have, so a series truncated at its start reads na for its whole
        // warm-up window. Once output_val is finite the series is seeded.
        if (is_na(output_val)) {
            sum += src;
            detail::advance_observation_count(bar_count);
            if (bar_count < length_) {
                return na<double>();
            }
            // Round the finite seed sum before division. The empty prefix
            // can occur after a nonfinite output, outside the initial seed.
            const double seed_sum = seed_sources_.empty() ? sum : ema_seed_sum(seed_sources_);
            output_val = seed_sum / static_cast<double>(length_);
            return output_val;
        }
        output_val = ema_step(alpha, src, output_val);
        detail::advance_observation_count(bar_count);
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
    detail::advance_observation_count(bar_count);
    return output_val;
}

// --- WMA (Weighted Moving Average) ---

WMA::WMA(int length, WindowCacheCapacity cache)
    : length_(length), buffer_(static_cast<std::size_t>(length), cache) {}

double WMA::compute(double src) {
    if (is_na(src)) {
        pushed_ = false;
        return na<double>();
    }

    const auto capacity = buffer_.capacity();
    const double leaving = capacity && buffer_.size() == capacity
        ? buffer_[capacity - 1] : na<double>();
    buffer_.push_front(src);
    pushed_ = capacity != 0;
    evicted_ = leaving;
    return weighted_mean();
}

double WMA::weighted_mean() const {
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
    double half_val = wma_half_.compute(src);
    double full_val = wma_full_.compute(src);
    // A missing input (including warmup) records a no-push bar in each
    // stage, so a later replacement cannot rewind the preceding bar.
    double diff = 2.0 * half_val - full_val;
    return wma_sqrt_.compute(diff);
}

// --- VWMA (Volume-Weighted Moving Average) ---

VWMA::VWMA(int length) : length_(length), sv_sum_(0.0), v_sum_(0.0) {}

double VWMA::compute(double src, double vol) {
    saved_sv_sum_ = sv_sum_;
    saved_v_sum_ = v_sum_;
    pushed_ = false;
    evicted_ = false;
    saw_na_ = is_na(src) || is_na(vol);
    if (saw_na_) {
        return na<double>();
    }
    // A negative length has no window: no pair enters it, so the eviction
    // loop below has nothing to take out and recompute() nothing to undo.
    // (A length of 0 needs no branch: its one pair leaves as it enters.)
    if (length_ < 0) {
        return na<double>();
    }

    double sv = src * vol;
    sv_buffer_.push_back(sv);
    v_buffer_.push_back(vol);
    sv_sum_ += sv;
    v_sum_ += vol;

    while ((int)sv_buffer_.size() > length_) {
        evicted_ = true;
        evicted_sv_ = sv_buffer_.front();
        evicted_v_ = v_buffer_.front();
        sv_sum_ -= evicted_sv_;
        sv_buffer_.pop_front();
        v_sum_ -= evicted_v_;
        v_buffer_.pop_front();
    }
    pushed_ = !sv_buffer_.empty();

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
    // A post-seed snapshot can no longer rewind into the seed window.
    if (seed_sources_.capacity() != 0 && bar_count >= length_)
        std::vector<double>().swap(seed_sources_);
}
void EMA::restore() {
    output_val = saved_output_val_;
    sum = saved_sum_;
    bar_count = saved_bar_count_;
    ready_count_ = saved_ready_count_;
    // A warm-up observation can append one source after the saved count.
    if (seed_sources_.size() > static_cast<std::size_t>(saved_bar_count_))
        seed_sources_.pop_back();
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
    window_.unpush();
    return compute(src);
}

// --- WMA ---
double WMA::recompute(double src) {
    if (pushed_) {
        if (!is_na(src)) {
            buffer_.update_front(src);
            return weighted_mean();
        }
        buffer_.undo_push_front(is_na(evicted_) ? std::nullopt : std::optional<double>{evicted_});
    }
    return compute(src);
}

// --- HMA ---
double HMA::recompute(double src) {
    double half_val = wma_half_.recompute(src);
    double full_val = wma_full_.recompute(src);
    double diff = 2.0 * half_val - full_val;
    return wma_sqrt_.recompute(diff);
}

// --- VWMA ---
double VWMA::recompute(double src, double vol) {
    const bool replay = saw_na_ || is_na(src) || is_na(vol);
    if (replay || !pushed_) {
        if (pushed_) {
            if (evicted_) {
                sv_buffer_.push_front(evicted_sv_);
                v_buffer_.push_front(evicted_v_);
            }
            sv_buffer_.pop_back();
            v_buffer_.pop_back();
            sv_sum_ = saved_sv_sum_;
            v_sum_ = saved_v_sum_;
        }
        const double result = compute(src, vol);
        saw_na_ = replay;
        return result;
    }

    // A bar that has only had complete pairs keeps the original rounding
    // order. Once it crosses na, every replacement starts at its saved sums.
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
