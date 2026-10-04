# Generated stream equivalence fixtures

These Pine v6 strategies are compiled through the real PineForge code generator.
The checked-in C++ was generated with codegen commit
`ee04fc66786fda3b97339e1686029e884356432a`, using:

```bash
python3 tests/fixtures/stream_security/generate.py --codegen-dir ../pineforge-codegen-oss
```

Each fixture links into its own CTest executable, like the checked-settings
generated fixture. Tests compare the same 4,000 deterministic confirmed
one-minute bars through the batch ABI and the stream ABI, with 500 warmup bars.
They pin the existing batch trade counts and compare every trade field, net
profit, processed-bar counts and security feed totals. Two controls run with
script timeframes 5 and 15 over input timeframe 1.
Additional rows cover warmup lengths 1, 30, 499, 501 and 3999, so the
warmup boundary need not coincide with a requested-timeframe boundary.
Every equivalence row includes at least one realtime confirmed input. A
warmup-only snapshot retains the warmup mark rather than presenting a completed
batch range, as specified by `tests/test_stream_report_equivalence.cpp`.

## State hash witnesses

The scheduler correction also changes existing equal-clock stream-state hash
witnesses: a completed caller no longer leaves a stale callback wait and
deferred input in the source-state fold. The continuation-view, host-view memo
and quiet-bar pins are refreshed only for those stream-state reads. The base
runtime reproduces the previous pins; every historical pin, trade digest,
trade count and profit stays unchanged. The quiet-bar recorded-state digest
changes with the corrected source state, not with broker trades.
