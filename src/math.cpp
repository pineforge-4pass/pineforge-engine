#include <pineforge/math.hpp>

namespace pineforge {

namespace math {

Sum::Sum(int length) : window_(length), length_(length) {}

double Sum::compute(double src) {
    if (is_na(src)) {
        // Pine ignores na sources rather than consuming a window slot. Once
        // seeded, the last-N-valid sum is therefore held on na-input bars.
        window_.note_no_push();
        if (length_ > 0 && window_.seeded()) {
            return window_.sum();
        }
        return na<double>();
    }
    // A length of zero or less has no window: no source enters it (the
    // window sum takes a positive length only) and the value is na on every
    // bar, as it already is above for a missing source.
    if (length_ <= 0) {
        return na<double>();
    }
    double sum = window_.push(src);
    if (!window_.seeded()) {
        return na<double>();
    }
    return sum;
}

double Sum::recompute(double src) {
    if (is_na(src)) {
        // An na recompute rewinds the bar: the source it pushed leaves again
        // and the pre-bar seeded window is held (na before seeding).
        window_.unpush();
        window_.note_no_push();
        if (length_ > 0 && window_.seeded()) {
            return window_.sum();
        }
        return na<double>();
    }
    // As in compute(): no replacement source enters a window of no length.
    if (length_ <= 0) {
        return na<double>();
    }
    double sum = window_.repush(src);
    if (!window_.seeded()) {
        return na<double>();
    }
    return sum;
}

} // namespace math

} // namespace pineforge
