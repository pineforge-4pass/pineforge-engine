#pragma once

#include <pineforge/market_driver.hpp>

namespace pineforge::source {

inline bool modeled_pine_input(NativePriceProvenance provenance) noexcept {
    return provenance != NativePriceProvenance::ObservedPrint;
}

}
