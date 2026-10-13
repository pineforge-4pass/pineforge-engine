# s_inf_limit

Contract: revision 3 section 4 and Appendix B, `entry.limit_price` source
admission I -> S; Appendix D's positive-infinity variant. Label: S.
Expectation pinned before execution: failed, `strategy_value_invalid`,
`field=entry.limit_price`, `reason=nonfinite`, with no success report.

At script bar 5, `math.exp(close * 10.0)` supplies the actionable limit price.
The hand-written feed has close = 101+i for i = 0..39, so the exponent is
1010..1400 (1060 at bar 5). Every exponent exceeds log(DBL_MAX), approximately
709.783, on the x86-64/Linux double toolchain this driver targets. `std::exp`
therefore returns positive infinity. All literals and input bars are finite;
the series `close` makes this a runtime expression, not an oversized literal
that the Pine parser can reject before execution.

Source evidence read during authoring, without executing codegen or the engine:
`pineforge_codegen/codegen/tables.py` maps `math.exp` to `std::exp`;
`codegen/visit_call.py::_visit_math_call` emits that call directly and
`_visit_strategy_call` forwards the limit expression without finite coercion.
`include/pineforge/series.hpp` stores/reads the float result without a finite
filter, and `include/pineforge/na.hpp::is_na(double)` tests `std::isnan`, not
infinity. The optional constructor folder declines nonfinite folds; this
bar-dependent expression does not depend on folding behavior.

Feed rule for i = 0..39: timestamp = 1704067200000 + 60000*i; open = 100+i;
high = open+2; low = open-1; close = open+1; volume = 1000+10*i.
This is synthetic data. No fixture, native injection or market feed is needed.

Main lacks the source-invalid door producer, so this is intended to be red
there for the missing typed code/args, and green only after that producer lands.
No observed result was used to choose the expectation. A transpile/link
refusal is not the intended red: inspect its stage log on the box and resolve
construction/toolchain support. Do not substitute a finite price or relabel
that refusal as runtime coverage. No native boundary control is claimed.
