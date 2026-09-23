// Harvested by test_native_projection_witness with PF_PROJECTION_WITNESS_DUMP=1:
// at lane PERF-P23's base (fc7aad62), then re-harvested once at the v19 value
// epoch (R5 lane V19-C). One row per profile/hook/trigger: the outcome every
// write of that cell produces; since V19-C no write's outcome depends on its
// field. kind 3 = Completed, 4 = Failed; code 10 = ProjectionMismatch;
// op 2 = Begin, 4 = Input, 5 = Callback, 6 = Settlement.
//
// V19-C moved every write a pump catches from the callback or policy-hook
// boundary after it to the pump's own end: each "expectation corrected" row
// below names the boundary it was caught at before. The caught set did not
// move (1520 of 1900 writes at the base and after), and neither did a write
// already caught at a pump or begin boundary (the on_native_run_begin,
// hash_host_extension, margin/resolve_margin_requirement/1 and aggregated
// on_native_input rows) nor one nothing reaches (prepare_native_begin and the
// fired=0 rows). The base's one field exception (margin/margin_check_allowed/3
// under initial_capital, caught at settlement ordinal 13) is its cell's row now.
constexpr Expected kExpected[] = {
    {"batch/prepare_native_begin/1",
     "fired=1 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    {"batch/prepare_native_begin/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    {"batch/on_native_run_begin/1",
     "fired=1 kind=4 code=10 op=5 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/on_native_run_begin/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=5 ordinal=1 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_input/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=13 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_input/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=1 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_bar_open/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=13 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_bar_open/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=5 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_bar/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=12 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_bar/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=5 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_recalculate/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=12 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_recalculate/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=8 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/on_native_applied/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/on_native_applied/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=6 ordinal=7 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/resolve_execution_terms/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/resolve_execution_terms/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=6 ordinal=7 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/validate_execution_precommit/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/validate_execution_precommit/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    {"batch/owns_lot_excursions/1",
     "fired=1 kind=4 code=10 op=2 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/owns_lot_excursions/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=6 ordinal=24 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/closed_lot_excursion/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=26 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"batch/closed_lot_excursion/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/hash_host_extension/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"batch/hash_host_extension/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"margin/resolve_margin_requirement/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=18 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"margin/resolve_margin_requirement/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=5 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"margin/margin_check_allowed/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=6 ordinal=11 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"margin/margin_check_allowed/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=6 ordinal=11 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"margin/resolve_margin_call_units/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"margin/resolve_margin_call_units/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=5 ordinal=14 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"margin/on_native_margin_call/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"margin/on_native_margin_call/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=5 ordinal=4 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"anchored/resolve_anchored_level/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"anchored/resolve_anchored_level/3",
     "fired=0 kind=3 code=0 op=0 ordinal=0 disc=0 ctx=0 error=''"},
    // expectation corrected: op=5 ordinal=4 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"lower/on_native_sub_bar/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=12 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"lower/on_native_sub_bar/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=24 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"subscribed/on_native_timeframe_bar/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=76 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"subscribed/on_native_timeframe_bar/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=13 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"ticks/on_native_tick/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native host already failed'"},
    // expectation corrected: op=5 ordinal=15 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"ticks/on_native_tick/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native host already failed'"},
    {"aggregated/on_native_input/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    {"aggregated/on_native_input/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=1 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"aggregated/on_native_bar_open/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=13 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"aggregated/on_native_bar_open/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=5 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"aggregated/on_native_bar/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=17 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"aggregated/on_native_bar/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=1 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"stream_bars/on_native_input/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=13 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"stream_bars/on_native_input/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=5 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"stream_bars/on_native_bar/1",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
    // expectation corrected: op=5 ordinal=17 -> op=4 ordinal=0, because v19 compares
    // the projection per pump (V19-C): caught at the pump's end.
    {"stream_bars/on_native_bar/3",
     "fired=1 kind=4 code=10 op=4 ordinal=0 disc=0 ctx=0 error='native projection mismatch'"},
};
