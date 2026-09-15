// Exact native-route twin of the complete L0 stop-snapshot oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>
#include "oracle_fixture_accessors.hpp"

#define PineStrategyHost OraclePineNativeHost
#define signed_position_size oracle_script_position_size
#define PendingOrder FixtureIntentRow
#define pending_orders_ source_pending_view()
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_stop_snapshot.cpp"
#undef pending_orders_
#undef PendingOrder
#undef signed_position_size
#undef PineStrategyHost
