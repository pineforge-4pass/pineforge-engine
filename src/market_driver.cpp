#include <pineforge/market_driver.hpp>

#include <cmath>

namespace pineforge {

bool native_bar_structurally_valid(const Bar& bar) noexcept {
    if (!std::isfinite(bar.open) || !std::isfinite(bar.high)
        || !std::isfinite(bar.low) || !std::isfinite(bar.close)) {
        return false;
    }
    if (bar.low > std::min(bar.open, bar.close)) return false;
    if (bar.high < std::max(bar.open, bar.close)) return false;
    if (!std::isnan(bar.volume) && (!std::isfinite(bar.volume) || bar.volume < 0.0)) {
        return false;
    }
    return true;
}

}  // namespace pineforge
