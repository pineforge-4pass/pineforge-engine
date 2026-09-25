# Seeded strategy.risk corpus experiment (lane N12's corpus half, R5 lane H-MEASURE)

Lane N12 ruled every `strategy.risk.*` rule a retention of the Pine adapter
(design §3.6.1): Pine's risk statements run inside the per-bar body, so the
adapter first learns a limit on script bar 0, after `project()` has built the
run spec and `configure_native` has digested it, and `NativeRunSpec::risk` is a
begin-time declaration. The paired scenarios of
`tests/test_adapter_risk_relower.cpp` measure each rule against the kernel's;
the corpus half — the five corpus probes that declare a risk rule, run with the
KERNEL's rule seeded in the adapter's place — needed a source change no test
can make in process, and was an uncommitted experiment until this fixture.

- `n12-experiment.patch` — lane N12's env-gated experiment, recovered from its
  session record and ported to this tree: with `PF_N12_ROUTE` set, `project()`
  declares `spec.risk` from `PF_N12_SEED` (`drawdown:<v>[:pct]`, `cap:<n>`),
  the adapter's own drawdown latch and fill-cap budget step aside, and the
  kernel's first block is logged. With `PF_N12_ROUTE` unset it is inert.
- `expected.tsv` — per probe: the seed, and how many rows of the ten
  TradingView-recorded columns differ from the unseeded run (oldest row first),
  the row counts, and the first differing row. The engine's own trailing
  columns (entry incarnation, range-end) renumber earlier and are not counted.
- `n12_diff.py` — the row comparison `expected.tsv` states.

`scripts/check_seeded_risk_experiment.sh` applies the patch to a throwaway copy
of the tree, builds the runtime and the five probes, checks that the unseeded
run still hashes to `scripts/corpus_parity_baseline.txt`, runs each probe
seeded and compares the counts. Exit 0 reproduced, 1 the measurement moved, 2
the patch no longer applies. On this tree (spark, 29 s): the drawdown probe
and `cap-risk-gates-allow-max-intraday-01` identical (1502 / 1464 rows),
`cap-max-intraday-filled-orders-isolate-01` 3840 of 3916 rows differ from
2025-04-08, `cap-gatekeeper-intraday-risk-01` 312 of 604 from 2025-10-25,
`composite-bracket-cap-range-pending-stop-01` 2370 of 2386 from 2025-04-02
with two extra rows — the counts design §3.6.1 states.
