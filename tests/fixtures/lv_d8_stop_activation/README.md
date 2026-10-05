# LV-D8 stop activation controls

`generated.cpp` comes from `strategy.pine` using the primary codegen checkout at
`c1df8a69346a1fe59e65e357bd9cbd099932fcf9`. The strategy places one stop while
flat, with optional same-ID reissue and sibling-order controls.

`test_lv_d8_tick_stop_activation` checks the native activation, terms and terminal
events. `test_lv_d8_tick_stop_equivalence` drives the actual tick runner and the
canonical ENDPOINTS (distribution 3) batch path. Fixed and funded default-sized
orders fill; an over-budget all-in stop receives terminal HostPrecommit refusal.
Touch and gap-open controls compare execution identity; crossing controls also
pin the intentional difference between an observed print and an OHLC crossing.

For triage, `lv_d8_tick_stop_equivalence.py --assume-fill-equivalence` asserts the
candidate study's stronger premise and fails when observed-price funding rejects
the all-in opening. This diagnostic is deliberately not a CTest row: admitting
unaffordable quantity or inventing an unobserved stop-price execution is not a
correctness repair.

`--require-terminal-receipts` checks that the shared equivalence receipt observer
also exports native match refusals. It fails with the current incomplete observer;
the separate, uncommitted LV-D8 observation patch makes this diagnostic pass.
