#pragma once

#include <pineforge/source/pine_native_host.hpp>

#include <algorithm>
#include <vector>

namespace pineforge::source {

// The L0 oracle calls the source-language position accessor, whose value can
// intentionally be frozen during POOC. Do not substitute live_position_size:
// that is a physical-position observer and loses the documented script-view
// semantics. This fixture-only adapter exposes the actual protected accessor
// under a distinct name without adding a product surface.
class OraclePineNativeHost : public PineNativeHost {
protected:
    double oracle_script_position_size() const {
        return PineNativeHost::signed_position_size();
    }

    // Keep the frozen oracle's vector view in this fixture. Refresh existing
    // row objects in place so repeated same-size reads preserve references.
    const std::vector<Trade>& oracle_trade_rows() const {
        auto& snapshot = oracle_trade_snapshot_;
        snapshot.resize(trades_.size() - trades_.base());
        if (!snapshot.empty())
            std::copy(trades_.begin(), trades_.end(), snapshot.begin());
        return snapshot;
    }

private:
    mutable std::vector<Trade> oracle_trade_snapshot_;
};

}  // namespace pineforge::source
