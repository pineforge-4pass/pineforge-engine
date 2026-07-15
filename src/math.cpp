#include <pineforge/math.hpp>

namespace pineforge {

namespace math {

Sum::Sum(int length)
    : length_(length), sum_(0.0), saved_sum_(0.0),
      has_saved_state_(false), current_value_added_(false),
      current_value_evicted_(false), current_evicted_value_(0.0) {}

double Sum::apply(double src) {
    current_value_added_ = false;
    current_value_evicted_ = false;

    if (is_na(src)) {
        // Pine ignores na sources rather than consuming a window slot. Once
        // seeded, the last-N-valid sum is therefore held on na-input bars.
        if (length_ > 0 && static_cast<int>(buffer_.size()) >= length_) {
            return sum_;
        }
        return na<double>();
    }

    buffer_.push_back(src);
    sum_ += src;
    current_value_added_ = true;
    while ((int)buffer_.size() > length_) {
        current_value_evicted_ = true;
        current_evicted_value_ = buffer_.front();
        sum_ -= buffer_.front();
        buffer_.pop_front();
    }

    if (static_cast<int>(buffer_.size()) < length_) {
        return na<double>();
    }
    return sum_;
}

void Sum::restore() {
    if (current_value_added_ && length_ > 0) {
        buffer_.pop_back();
        if (current_value_evicted_) {
            buffer_.push_front(current_evicted_value_);
        }
    }
    sum_ = saved_sum_;
    current_value_added_ = false;
    current_value_evicted_ = false;
}

double Sum::compute(double src) {
    saved_sum_ = sum_;
    has_saved_state_ = true;
    return apply(src);
}

double Sum::recompute(double src) {
    if (!has_saved_state_) {
        return compute(src);
    }
    restore();
    return apply(src);
}

} // namespace math

} // namespace pineforge
