# s1a_ticks_posinf_earlier

Pinned before implementation or execution: completed, with at least the finite witness trade.

Runtime exp(10000) gives positive infinity. visit_call.py:3424-3441 passes loss unchanged as loss_ticks when stop is absent; this source supplies no stop. This is source inspection, not an executed compilation receipt. The language can construct this value; compile_refused is not asserted.

The feed is forty hand-defined one-minute bars from 2024-01-01 00:00 UTC: O=100, H=101, L=99, C=100, V=1000, timestamp=T0+i*60000. No market data or private fixture.

Window: T=T0+2 minutes (1704067320000), E=T0+40 minutes (1704069600000), F=T0; two requested pre-roll bars. The call is at source index 0: an earlier pre-roll evaluation, replaced by index 1 and never replayed. The finite entry at index 3 and close_all at index 5 witness continued execution for completed controls.

Paired host row: exit.loss_ticks/earlier/+inf in tests/test_value_admission_doors.cpp. Its expected code/field/reason comes from the brief, not a sampled output.

Integration requirement: run.json uses the selected-window flags of docker/run_json.py. The inspected P0 ordinary driver currently refuses --report-policy and does not create bound request-feed inventory or phase transport. The supervisor must supply P0's selected route before this probe can be executed as this placement. Never drop these flags and count an ordinary run as retained/discarded coverage.
