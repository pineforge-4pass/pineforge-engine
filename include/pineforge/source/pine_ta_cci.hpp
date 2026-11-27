#pragma once
// TradingView's ta.cci on a window whose mean deviation is zero up to
// rounding.
//
// ta::CCI (<pineforge/ta.hpp>) divides (src - mean) by 0.015 times the mean
// deviation unless that deviation is exactly 0. Identical values whose
// decimal spelling is not a binary fraction do not reach an exact 0: three
// values of 0.1 sum to 0.30000000000000004, the mean is one step above 0.1,
// every absolute deviation is that one step (2^-56, about 1.4e-17), and the
// quotient is -1 / 0.015, about -66.67.
//
// TradingView's ta.cci answers 0 there. On a synthetic script whose source
// is 0.1 on every bar its call site sees, TradingView's own ta.dev(x, 3) and
// ta.sma(x, 3) - x are both about 1.4e-17, not zero, and ta.cci(x, 3) is 0;
// ta.cci(1.0, 3) is 0 as well. PineCci therefore treats a mean deviation
// inside the comparison band of <pineforge/ta_compare_band.hpp> (at most
// 1e-10 from zero, the band TradingView's relational operators follow) as
// zero and answers 0. Outside the band every value is ta::CCI's own, bit for
// bit, and na stays na.
//
// The deviation is read from a ta::Dev that receives the same calls: ta::Dev
// and ta::CCI keep the same window (the last `length` values that are not na)
// and use the same arithmetic in the same order, so ta::Dev's value is the
// deviation ta::CCI divides by.
//
// Not reproduced: TradingView's ta.cci carries state from earlier bars. After
// a history of much larger values the same three values of 0.1 read -50
// there. That is the value an ordinary window gives when one of its three
// members is higher than the other two, and no mean shared by three equal
// members can give it. PineCci, like ta::CCI, computes each window from its
// members alone and answers 0 in that case too.
//
// The members forwarded are those of ta::CCI: the length constructor,
// compute, recompute and observe_owned_storage.
#include <pineforge/ta.hpp>
#include <pineforge/ta_compare_band.hpp>

namespace pineforge {
namespace source {

class PineCci {
    ta::CCI cci_;
    ta::Dev dev_;

    static double flat_as_zero(double value, double deviation) {
        return float_band_eq(deviation, 0.0) ? 0.0 : value;
    }

public:
    explicit PineCci(int length) : cci_(length), dev_(length) {}
    double compute(double src) {
        const double deviation = dev_.compute(src);
        return flat_as_zero(cci_.compute(src), deviation);
    }
    double recompute(double src) {
        const double deviation = dev_.recompute(src);
        return flat_as_zero(cci_.recompute(src), deviation);
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        ::pineforge::observe_owned_members(out, cci_, dev_);
    }
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the class above exists.
#define PINEFORGE_HAS_PINE_CCI 1
