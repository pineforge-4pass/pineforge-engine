#pragma once

// Compatibility spelling retained for the L2 fixture twins.  L3a has one
// source host: generated strategies and these fixtures both lower through
// PineStrategyHost's native provider and scheduler.
#include <pineforge/source/pine_strategy_host.hpp>

namespace pineforge::source {

using PineNativeHost = PineStrategyHost;
using FixtureIntentRow = PineStrategyHost::FixtureIntentRow;
using FixtureIntentKind = PineStrategyHost::FixtureIntentKind;

} // namespace pineforge::source
