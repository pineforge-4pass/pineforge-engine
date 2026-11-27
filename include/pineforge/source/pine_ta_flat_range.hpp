#pragma once
// Pine's ta.wvad and ta.iii on a bar whose high equals its low.
//
// Both built-ins divide by the bar's range:
//
//   ta.wvad = (close - open) / (high - low) * volume
//   ta.iii  = (2 * close - high - low) / (high - low) * volume
//
// The kernel classes ta::WVAD and ta::III (<pineforge/ta.hpp>) answer 0 on a
// bar with no range, a native convention their own tests pin. The Pine
// expressions carry no such branch: on a flat bar (open = high = low = close)
// they are 0 / 0, which Pine reads as na, while a bar with a range whose
// numerator is zero (close equal to open, or close at the middle of the
// range) is a genuine 0. These adapters answer na when high equals low and
// delegate every other bar to the kernel class, so the values off a flat bar
// are the kernel's own, bit for bit, and a missing input is na as before.
//
// Not pinned by TradingView values yet: the na on a flat bar is read from the
// expressions above. A bar whose range is zero while its numerator is not
// (a close away from an open that equals high and low: not a valid candle)
// also answers na here; the bare expression would be an infinity there.
//
// Neither class keeps state: recompute() is compute().
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

namespace pineforge {
namespace source {

class PineWvad {
    ta::WVAD impl_;

public:
    double compute(double open, double high, double low, double close, double volume) {
        if (high == low) return na<double>();
        return impl_.compute(open, high, low, close, volume);
    }
    double recompute(double open, double high, double low, double close, double volume) {
        return compute(open, high, low, close, volume);
    }
    // No retained fields.
    void observe_owned_storage(OwnedStorage&) const noexcept {}
};

class PineIii {
    ta::III impl_;

public:
    double compute(double high, double low, double close, double volume) {
        if (high == low) return na<double>();
        return impl_.compute(high, low, close, volume);
    }
    double recompute(double high, double low, double close, double volume) {
        return compute(high, low, close, volume);
    }
    // No retained fields.
    void observe_owned_storage(OwnedStorage&) const noexcept {}
};

}  // namespace source
}  // namespace pineforge

// Feature macro: the classes above exist.
#define PINEFORGE_HAS_PINE_FLAT_RANGE 1
