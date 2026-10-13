# max_drawdown_earlier_preroll_neg_inf

Pinned before execution from Appendix B's risk setter row and the EN-132
D1/D2 rulings. The sink is `set_pine_risk_max_drawdown`; expected
failure `strategy_value_invalid`, `field=risk.max_drawdown`, `reason=nonfinite`.

The raw value is `-math.exp(close * 10.0)`. Every feed close is 100, so
`close * 10.0` is 1000, above log(DBL_MAX) ~709.783. The documented lowering
`math.exp` -> `std::exp` (docs/pine_v6_coverage_detail.md:612) therefore
produces positive infinity on the supervisor's x86-64 double toolchain;
unary minus produces negative infinity.
The expression uses a runtime series and finite literals. P0's seed
`seed/s_inf_limit/NOTES.md` also records the codegen call-table and direct
argument-forwarding evidence for this construction. No division-by-zero or
oversized-literal behavior is assumed. Toolchain execution is still pending;
a compile refusal is a construction/toolchain issue, never runtime coverage.
The paired native file has the corresponding `MAX_DRAWDOWN-NEG_INF` rows.

The selected window starts at bar 3. The invalid risk statement runs at
bar 0, an EARLIER pre-roll evaluation that would be discarded before the
final retained evaluation at bar 2. It must still fail immediately because
risk acts live (D2), before any order can be submitted. `run.json` pins F/T/E/N
and the selected-window options. P0's current ordinary-path driver rejects
`--report-policy` in `options`; selected-window driver integration is required
before this row can run. That rejection is not the pinned engine outcome and
must not be counted as runtime coverage. The native EARLIER-PREROLL row is
independent of the driver and toolchain.

Feed rule for i=0..39: timestamp=1736121600000+60000*i, O=100, H=101,
L=99, C=100, volume=1000+10*i. All bars are synthetic and finite. No engine,
compiler, transpiler, test or corpus was run while authoring this probe.
