#include <pineforge/math.hpp>

namespace pineforge {

namespace math {

Sum::Sum(int length) : length_(length), sum_(0.0) {}

double Sum::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    sum_ += src;
    while ((int)buffer_.size() > length_) {
        sum_ -= buffer_.front();
        buffer_.pop_front();
    }
    return sum_;
}

double Sum::recompute(double src) {
    if (buffer_.empty()) {
        return compute(src);
    }
    double old_back = buffer_.back();
    buffer_.back() = src;
    sum_ = sum_ - old_back + src;
    return sum_;
}

} // namespace math

} // namespace pineforge
