// Exact native-route attempt for the complete direct F7/F8 reversal oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle/test_oracle_reversal.cpp"
#undef signed_position_size
#undef PineStrategyHost
