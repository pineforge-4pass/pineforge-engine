#pragma once

#include <pineforge/source/pine_native_host.hpp>

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
};

}  // namespace pineforge::source
