# Generated stream equivalence fixtures

These Pine v6 strategies are compiled through the real PineForge code generator.
The checked-in C++ was generated with codegen commit
`ee04fc66786fda3b97339e1686029e884356432a`, using:

```bash
python3 tests/fixtures/stream_security/generate.py --codegen-dir ../pineforge-codegen-oss
```

Each fixture links into its own CTest executable, like the checked-settings
generated fixture. Security, dual-stop, priced POOC reversal, slipped-short
affordability and short-exit cases compare the same 4,000 deterministic confirmed
one-minute bars through the batch ABI and the stream ABI, with 30 and 500 warmup
bars. The daily request also has a 2,000-bar split. Two eight-bar all-in reversals
use splits of one and three bars: fractional source-money rounding and whole-lot
ties retain only the closing leg, even when the next open gaps down.

The harness compares every trade field, entry/exit IDs, net profit, processed-bar
counts and security feed totals, and emits a bitwise trade fingerprint. Existing
security batch counts are pinned. Two controls run with script timeframes 5 and
15 over input timeframe 1; the POOC dual-stop case also runs at script timeframe 5.
An eight-bar slipped-short case separately pins next-open affordability without POOC.
