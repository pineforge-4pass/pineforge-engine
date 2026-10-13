# s_margin_long_pos_inf_unconstructible

Runtime declaration case: **UNCONSTRUCTIBLE with the inspected codegen**.
This source requests 1e400 for strategy.margin_long. The parser rejects the
out-of-range numeric operand before emission (codegen parser.py:1633-1649).
The schema therefore records compile_refused, an empty runtime code/args,
and the existing native boundary control file. This is COMPILE COVERAGE only;
it is not a runtime strategy_value_invalid result or a red runtime producer row.

The paired native row is `decl_margin_long_pos_inf` in
`tests/test_value_admission_decl.cpp`. It supplies +Infinity directly to
`PineStrategyHost::configure_pine_strategy` and pins
`strategy_value_invalid {field: "strategy.margin_long", reason: "nonfinite"}`
from D3. That boundary row is red on main because configure copies unchecked.
`math.pow(10, 400)` is not an alternative infinity construction here: the
strategy-argument literal extractor returns None and emission omits it.
See `.lane-out/MAP.md` section 1 for every numeric declaration member.

Tape: 40 hand-rule synthetic one-minute bars, timestamp 1736121600000 +
i*60000, O=C=100, H=101, L=99, V=1000; no market data. If the declaration
were constructible, the body would enter one contract at bar 0 and request a
close at bar 3, leaving ample bars for both next-open fills. Use input/script
timeframes "1" / "1". Expectations authored before any execution.
