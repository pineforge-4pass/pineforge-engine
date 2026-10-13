# order_limit_decimal_finite

Both raw price arguments are present, selecting StopLimit and bypassing the later half-tick threshold helper. This isolates the decimal normalization producer so a removed guard cannot be masked by another S error.

The corresponding decimal quotient stays finite; the large level is a legitimate control.

Pinned before execution: A completed finite run, with no typed failure. Source construction uses the finite literal `1e306`; it does not generate an infinity in Pine. The symbol fixture supplies positive finite mintick `0.01` through the real `--syminfo` setter.

The paired overflow probe is `arith/order_limit_decimal_overflow`. The native boundary file is `tests/test_value_admission_arith.cpp`, row `order_limit_near_boundary_bits`. The C++ near-boundary/bit-control rows are stronger arithmetic identity checks than this completion-only process probe.

Bars follow a hand-written constant-feed rule: 40 minute bars from Unix ms 1700000040000, every OHLC value 100, volume 1. No market data. Margins and slippage are zero. The script cancels outstanding orders and closes any position on bar 3. `min_trades=0` permits a valid remote stop to stay unfilled; status/report completion is mandatory.

Authored only: no transpilation, compile, engine, or probe run in this lane. If the real codegen refuses this finite literal or another construct, retain the pinned runtime expectation and report the first diagnostic; do not count the refusal as S runtime coverage or infer a different expected code from its output.
