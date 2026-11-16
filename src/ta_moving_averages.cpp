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
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace pineforge {

// The index of the highest set bit of a non-zero word.
static int highest_bit(std::uint64_t word) noexcept {
    int bit = 0;
    for (int step = 32; step != 0; step /= 2) {
        if ((word >> step) != 0) {
            word >>= step;
            bit += step;
        }
    }
    return bit;
}

KahanWindowSum::KahanWindowSum(int length)
    : length_(length), count_(0), sum_(0.0),
      saved_sum_(0.0), saved_count_(0), saved_evicted_value_(0.0),
      saved_evicted_(false), saved_pushed_(false) {
    static_assert(std::numeric_limits<double>::is_iec559
                      && std::numeric_limits<double>::digits == 53
                      && sizeof(double) == sizeof(std::uint64_t),
                  "the window sum reads the fields of an IEEE binary64");
}

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

// Adds `word` to the exact sum at `limb`, or subtracts it, and lets the carry
// or borrow run into the higher limbs. The integer is two's complement, so
// what leaves the top limb is dropped.
void KahanWindowSum::carry(std::size_t limb, std::uint64_t word, bool subtract) noexcept {
    for (; word != 0 && limb < limbs; ++limb) {
        const std::uint64_t before = exact_[limb];
        if (subtract) {
            exact_[limb] = before - word;
            word = before < word ? 1 : 0;
        } else {
            exact_[limb] = before + word;
            word = exact_[limb] < before ? 1 : 0;
        }
    }
}

// A source enters the exact sum, or leaves it (`leaving`). A finite double is
// its 53-bit significand times 2^(shift - 1074), where shift is its biased
// exponent less one (zero for a subnormal), so it covers at most two limbs
// from bit `shift` up. Leaving is the exact inverse of entering: the sum does
// not depend on the order in which the window was filled or rewound.
void KahanWindowSum::account(double src, bool leaving) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &src, sizeof bits);
    const bool negative = (bits >> 63) != 0;
    const unsigned exponent = static_cast<unsigned>((bits >> 52) & 0x7ffu);
    std::uint64_t significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent == 0x7ffu) {
        int& held = significand != 0 ? nans_
            : negative ? negative_infinities_ : positive_infinities_;
        held += leaving ? -1 : 1;
        return;
    }
    if (exponent != 0) significand |= std::uint64_t{1} << 52;
    const unsigned shift = exponent == 0 ? 0 : exponent - 1;
    const std::size_t limb = shift / 64;
    const unsigned offset = shift % 64;
    // A negative source entering takes its magnitude out of the sum.
    const bool subtract = negative != leaving;
    carry(limb, significand << offset, subtract);
    if (offset != 0) carry(limb + 1, significand >> (64 - offset), subtract);
}

// The window's exact sum as a double: one rounding to nearest, ties to even,
// overflowing to an infinity as an IEEE addition does. A NaN in the window,
// or infinities of both signs, make the sum NaN; infinities of one sign make
// it that infinity. An exact zero is +0.
double KahanWindowSum::rounded() const noexcept {
    if (nans_ != 0 || (positive_infinities_ != 0 && negative_infinities_ != 0)) {
        return na<double>();
    }
    const double infinity = std::numeric_limits<double>::infinity();
    if (positive_infinities_ != 0) return infinity;
    if (negative_infinities_ != 0) return -infinity;

    const bool negative = (exact_[limbs - 1] >> 63) != 0;
    std::array<std::uint64_t, limbs> magnitude = exact_;
    if (negative) {
        // Two's-complement negation: invert every limb and add one.
        std::uint64_t carry_in = 1;
        for (std::uint64_t& limb : magnitude) {
            limb = ~limb + carry_in;
            carry_in = (carry_in != 0 && limb == 0) ? 1 : 0;
        }
    }
    std::size_t top = limbs;
    while (top != 0 && magnitude[top - 1] == 0) --top;
    if (top == 0) return 0.0;

    // Bit k of the magnitude is worth 2^(k - 1074). Below 2^53 units the bit
    // pattern of the double is the integer itself: a subnormal, or the first
    // normal binade, whose exponent field is bit 52.
    int highest = static_cast<int>((top - 1) * 64) + highest_bit(magnitude[top - 1]);
    std::uint64_t bits = magnitude[0];
    if (highest > 52) {
        // The 53 bits from the highest set bit down are the significand; the
        // `dropped` bits below them round it, a half going to the even side.
        const int dropped = highest - 52;
        const std::size_t limb = static_cast<std::size_t>(dropped) / 64;
        const unsigned offset = static_cast<unsigned>(dropped) % 64;
        std::uint64_t significand = magnitude[limb] >> offset;
        if (offset != 0 && limb + 1 < limbs) {
            significand |= magnitude[limb + 1] << (64 - offset);
        }
        const std::size_t half_limb = static_cast<std::size_t>(dropped - 1) / 64;
        const unsigned half_offset = static_cast<unsigned>(dropped - 1) % 64;
        const bool half = ((magnitude[half_limb] >> half_offset) & 1) != 0;
        bool below_half =
            (magnitude[half_limb] & ((std::uint64_t{1} << half_offset) - 1)) != 0;
        for (std::size_t i = 0; i < half_limb && !below_half; ++i) {
            below_half = magnitude[i] != 0;
        }
        if (half && (below_half || (significand & 1) != 0)) ++significand;
        if (significand == (std::uint64_t{1} << 53)) {
            significand >>= 1;
            ++highest;
        }
        // A double whose biased exponent is E has its highest bit at E + 51.
        const int exponent = highest - 51;
        if (exponent >= 0x7ff) return negative ? -infinity : infinity;
        bits = (static_cast<std::uint64_t>(exponent) << 52)
            | (significand & ((std::uint64_t{1} << 52) - 1));
    }
    if (negative) bits |= std::uint64_t{1} << 63;
    double result = 0.0;
    std::memcpy(&result, &bits, sizeof result);
    return result;
}

// One bar of ta.sma / math.sum: once the window is full its oldest source
// leaves, the new source enters, and the exact sum is rounded once.
//
// Why an exact sum and not a compensated running one: a running sum keeps a
// compensation for the next term to absorb, and a term that cannot hold it
// leaves it in every later sum. After 0.5 and 0.3 the compensation is 2^-54,
// which neither the zero that follows nor the departing 0.5 can take, so a
// running sum reads 0.3 + 2^-54 and then 2^-54 where TradingView reads 0.3
// and then exactly 0 (fold capture of 2026-11-16, ta.sma and math.sum alike;
// tests/test_ta_sma_exact_compensation.cpp holds its rows).
double KahanWindowSum::push(double src) {
    // Growth is the only throwing step; finish it before changing either
    // the window or the previous bar's rewind record.
    ensure_room();
    saved_sum_ = sum_;
    saved_count_ = count_;
    saved_pushed_ = true;
    saved_evicted_ = count_ >= length_;
    if (saved_evicted_) {
        saved_evicted_value_ = window_[index(size_ - 1)].value;
        --size_;
        account(saved_evicted_value_, true);
    }
    head_ = head_ ? head_ - 1 : window_.size() - 1;
    window_[head_] = {src};
    ++size_;
    account(src, false);
    count_++;
    sum_ = rounded();
    return sum_;
}

void KahanWindowSum::note_no_push() {
    saved_pushed_ = false;
}

void KahanWindowSum::unpush() {
    if (!saved_pushed_) return;
    account(window_[head_].value, true);
    pop_front();
    if (saved_evicted_) {
        window_[index(size_)] = {saved_evicted_value_};
        ++size_;
        account(saved_evicted_value_, false);
    }
    sum_ = saved_sum_;
    count_ = saved_count_;
    saved_pushed_ = false;
}

double KahanWindowSum::repush(double src) {
    if (!saved_pushed_) {
        return push(src);
    }
    // The bar's source is replaced where it sits: the window keeps its shape
    // and what the bar evicted stays out.
    account(window_[head_].value, true);
    window_[head_] = {src};
    account(src, false);
    sum_ = rounded();
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

    // Replay the bar with the new source in place of the provisional one.
    // The sum is a function of the window, so a recompute()d bar and a
    // freshly computed one with an identical window agree bit-for-bit.
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
