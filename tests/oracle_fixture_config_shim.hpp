#pragma once

// The L0 oracle bodies remain byte-for-byte unchanged.  Their historical
// protected setup names are translated only in native-route fixture wrappers
// to the configuration object consumed by PineStrategyHost::prepare_native_begin.
#define initial_capital_ fixture_configuration().initial_capital
#define default_qty_type_ fixture_default_qty_type_slot()
#define default_qty_value_ fixture_configuration().default_qty_value
#define pyramiding_ fixture_configuration().pyramiding
#define commission_type_ fixture_commission_type_slot()
#define commission_value_ fixture_configuration().commission_value
#define slippage_ fixture_configuration().slippage
#define margin_long_ fixture_configuration().margin_long
#define margin_short_ fixture_configuration().margin_short
#define process_orders_on_close_ fixture_configuration().process_orders_on_close
#define calc_on_order_fills_ fixture_configuration().calc_on_order_fills
#define close_entries_rule_any_ fixture_configuration().close_entries_rule_any

struct FixtureRiskDirection {
    enum Value { BOTH = 0, LONG_ONLY = 1, SHORT_ONLY = -1 };
};
#define RiskDirection FixtureRiskDirection
#define risk_direction_ fixture_risk_direction_slot()
#define id_unclosed_qty_ source_id_ledger_view()

#ifdef PINEFORGE_L4C_NATIVE_ROUTE_TWIN
#undef PineStrategyHost
#define PineStrategyHost L4cFixtureHost
#define PendingOrder L4cPendingOrder
#define OrderType L4cOrderType
#define pending_orders_ l4c_pending_orders()
#define coof_fill_recalc_active_ l4c_coof_recalc_active()
#define coof_cursor_is_bar_close_ l4c_coof_cursor_is_bar_close()
#define callsite_close_callsites_ l4c_callsite_close_callsites()
#define exit_leg_event_seq_ l4c_exit_leg_event_seq()
#define is_first_tick_ is_first_tick()
#endif
