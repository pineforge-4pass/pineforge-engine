// Exact native-route twin of the L0 same-bar transaction tape oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#define PendingOrder FixtureIntentRow
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_reversal_same_bar_tx.cpp"
#undef PendingOrder
#undef signed_position_size
#undef PineStrategyHost
