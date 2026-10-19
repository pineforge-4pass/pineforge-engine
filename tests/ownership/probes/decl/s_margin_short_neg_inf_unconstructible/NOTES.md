# s_margin_short_neg_inf_unconstructible

Runtime declaration case: **UNCONSTRUCTIBLE with the inspected codegen**.
This source requests -1e400 for strategy.margin_short. The parser rejects the
out-of-range numeric operand before emission (codegen parser.py:1633-1649).
The expectation is `compile_refused` with `code` `PF-E0036` (the first error
diagnostic's stable code exposed by codegen, `tests/ownership/README.md:92-93`;
"Numeric literal exceeds the generated C++ range.", args `{}`). Runtime coverage
of the S nonfinite claim rests on the native control rows named in
`native_boundary_control`. This is COMPILE COVERAGE only;
it is not a runtime strategy_value_invalid result or a red runtime producer row.

The paired native row is `decl_margin_short_neg_inf` in
`tests/test_value_admission_decl.cpp`. It supplies -Infinity directly to
`PineStrategyHost::configure_pine_strategy` and pins
`strategy_value_invalid {field: "strategy.margin_short", reason: "nonfinite"}`
from D3. That boundary row is red on main because configure copies unchecked.
`math.pow(10, 400)` is not an alternative infinity construction here: the
strategy-argument literal extractor returns None and emission omits it.
See `.lane-out/MAP.md` section 1 for every numeric declaration member.

Tape: 40 hand-rule synthetic one-minute bars, timestamp 1736121600000 +
i*60000, O=C=100, H=101, L=99, V=1000; no market data. If the declaration
were constructible, the body would enter one contract at bar 0 and request a
close at bar 3, leaving ample bars for both next-open fills. Use input/script
timeframes "1" / "1". Expectations authored before any execution.
