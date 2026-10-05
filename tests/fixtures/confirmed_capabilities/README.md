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
physically ordered actions, bitwise for binary64, at warmup boundaries 30, 33,
500 (and 1500 for daily requests) and
after replay. The script-clock controls repeat the proven 5 and 15 minute cases.
The non-POOC `htf15_sma` fixture independently proves the admitted 15-minute
SMA request. Every POOC runner E2E is a refusal test: POOC remains refused on
every script clock, regardless of its order set or request composition.
POOC engine-level equivalence rows carry the `future_admission` CTest label:
evidence for future admission, not admitted shapes. They do not establish
general parity for nullable price legs, same-calculation brackets or global exits.

Refusal snapshots cover unproven clocks, foreign and empty-symbol feeds,
lookahead, gaps, expressions, runtime timeframes, stop-limit/OCA-style orders,
long or mixed-direction brackets, trailing exits, strategy.order, cancellation and immediate
closes. Their runner tests assert refusal by name before ledger, control and
health files exist. Newly admitted shapes are also refused for observed ticks.

The original capability snapshots in `../strategy_capabilities/generated/` deliberately
retain only the old receipt: their unchanged E2E proves that a new runner still
refuses old-library requests, POOC and varip, and still warns/runs a library
without any capability receipt.

The exact five-order-set matcher remains tested without authorizing POOC.
Refusal fixtures also cover all six risk rules, unmodeled call arguments,
pending priced-entry brackets, mixed priced entries, priced closes, unproven
sizing/slippage/account settings and cross-category compositions. Closing a
short has both close-all and entry-bound future-evidence fixtures. Refusal
fixtures also pin literal and series `na` entry prices, nullable bracket legs,
same-calculation market brackets and empty entry identifiers. Calendar refusal
contexts cover source timezone, chart timezone and session; script-five varip
and order-affecting POOC overrides stay refused. Optional receipt-version
fixtures pin ordinary fallback and fail-closed request/POOC/missing-version paths.
