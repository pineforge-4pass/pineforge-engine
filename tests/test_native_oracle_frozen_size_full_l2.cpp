// Exact native-route twin of the complete L0 frozen-size oracle. The sole
// legacy id-ledger observation is projected from the adapter's live cohort.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>
#include "oracle_fixture_accessors.hpp"

#define PineStrategyHost OraclePineNativeHost
#define signed_position_size oracle_script_position_size
#define id_unclosed_qty_ source_id_ledger_view()
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_frozen_size.cpp"
#undef id_unclosed_qty_
#undef signed_position_size
#undef PineStrategyHost
