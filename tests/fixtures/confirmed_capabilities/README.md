# Confirmed-bar capability proofs

These snapshots pass real Pine sources through the paired codegen. Regenerate on
the build host, then compare with the checked-in directory:

```sh
PYTHONPATH=../codegen python3 tests/fixtures/confirmed_capabilities/regenerate.py --output /tmp/confirmed-generated
diff -ru tests/fixtures/confirmed_capabilities/generated /tmp/confirmed-generated
```

The seven security and four POOC regression sources are copied unchanged from
`../stream_security/`, whose batch/stream pins landed in #325. Extra sources pin
constant timeframes, a single-call helper, a Heikin-Ashi alias, close-only varip,
market POOC and market close-all. Every admitted source has a C++ batch/stream
equivalence row and a runner tape E2E that compares all ABI report fields and
physically ordered actions, bitwise for binary64, at two warmup boundaries and
after replay. The script-clock controls repeat the proven 5 and 15 minute cases.

Refusal snapshots cover unproven clocks, foreign and empty-symbol feeds,
lookahead, gaps, expressions, runtime timeframes, stop-limit/OCA-style orders,
long brackets, trailing exits, strategy.order, cancellation and immediate
closes. Their runner tests assert refusal by name before ledger, control and
health files exist. Newly admitted shapes are also refused for observed ticks.

The original F08 snapshots in `../strategy_capabilities/generated/` deliberately
retain only the old receipt: their unchanged E2E proves that a new runner still
refuses old-library requests, POOC and varip, and still warns/runs a library
without any capability receipt.
