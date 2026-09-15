// Exact native-route twin of the complete L0 ShortSeed oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

using pineforge::source::FixtureIntentKind;

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#define PendingOrder FixtureIntentRow
#define OrderType FixtureIntentKind
#define pending_orders_ source_pending_view()
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_short_seed.cpp"
#undef pending_orders_
#undef OrderType
#undef PendingOrder
#undef signed_position_size
#undef PineStrategyHost
