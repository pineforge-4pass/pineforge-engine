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
market POOC, market close-all and entry-bound market close (explicitly not
immediate). The active five-minute request control crosses the requested close
with its moving average; the frozen close-versus-itself control has no trades
when its script and request clocks coincide. Every admitted source has a C++ batch/stream
equivalence row and a runner tape E2E that compares all ABI report fields and
physically ordered actions, bitwise for binary64, at two warmup boundaries and
after replay. The script-clock controls repeat the proven 5 and 15 minute cases.
POOC on a five-minute script clock stays refused because equal reports do not
establish equal physical action timestamps.

Refusal snapshots cover unproven clocks, foreign and empty-symbol feeds,
lookahead, gaps, expressions, runtime timeframes, stop-limit/OCA-style orders,
long or mixed-direction brackets, trailing exits, strategy.order, cancellation and immediate
closes. Their runner tests assert refusal by name before ledger, control and
health files exist. Newly admitted shapes are also refused for observed ticks.

The original F08 snapshots in `../strategy_capabilities/generated/` deliberately
retain only the old receipt: their unchanged E2E proves that a new runner still
refuses old-library requests, POOC and varip, and still warns/runs a library
without any capability receipt.
