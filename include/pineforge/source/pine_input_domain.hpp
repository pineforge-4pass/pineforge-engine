#pragma once

#include <pineforge/market_driver.hpp>

namespace pineforge::source {

inline bool modeled_pine_input(bool is_stream, NativePriceProvenance provenance) noexcept {
    return !is_stream || provenance != NativePriceProvenance::ObservedPrint;
}

}
