#pragma once

// Read-only native-route spelling bridge for restored L4b oracle twins.
// The retired PendingOrder book is never recreated: every access below maps
// to PineStrategyHost's fixture-only projection of live adapter placement
// snapshots.  Keeping the historical spellings lets the CHECK expressions
// remain byte-for-byte identical while their owner-private reads become
// public-source-host projections.

#include <pineforge/source/pine_strategy_host.hpp>

using pineforge::source::FixtureIntentKind;
using pineforge::source::FixtureIntentRow;

#define PendingOrder FixtureIntentRow
#define OrderType FixtureIntentKind
#define pending_orders_ source_pending_view()
