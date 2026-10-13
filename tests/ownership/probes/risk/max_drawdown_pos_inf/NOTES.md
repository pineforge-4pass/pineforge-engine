# max_drawdown_pos_inf

Pinned before execution from Appendix B's risk setter row and the EN-132
D1/D2 rulings. The sink is `set_pine_risk_max_drawdown`; expected
failure `strategy_value_invalid`, `field=risk.max_drawdown`, `reason=nonfinite`.

The raw value is `math.exp(close * 10.0)`. Every feed close is 100, so
`close * 10.0` is 1000, above log(DBL_MAX) ~709.783. The documented lowering
`math.exp` -> `std::exp` (docs/pine_v6_coverage_detail.md:612) therefore
produces positive infinity on the supervisor's x86-64 double toolchain;
the setter receives positive infinity.
The expression uses a runtime series and finite literals. P0's seed
`seed/s_inf_limit/NOTES.md` also records the codegen call-table and direct
argument-forwarding evidence for this construction. No division-by-zero or
oversized-literal behavior is assumed. Toolchain execution is still pending;
a compile refusal is a construction/toolchain issue, never runtime coverage.
The paired native file has the corresponding `MAX_DRAWDOWN-POS_INF` rows.

The setter runs on bar 5 before that bar's entry. Main stores the infinity
without the typed failure, so the assertion is intended to be red for the
missing source admission. No observed output was used to pin it.

Feed rule for i=0..39: timestamp=1736121600000+60000*i, O=100, H=101,
L=99, C=100, volume=1000+10*i. All bars are synthetic and finite. No engine,
compiler, transpiler, test or corpus was run while authoring this probe.
