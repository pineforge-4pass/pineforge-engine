# entry_stop_decimal_overflow

Both raw price arguments are present, selecting StopLimit and bypassing the later half-tick threshold helper. This isolates the decimal normalization producer so a removed guard cannot be masked by another S error.

The raw level / decimal point-size quotient overflows before tick-trigger construction.

Pinned before execution: `strategy_value_invalid`, exact field `entry.stop_price`, reason `numeric_overflow`. Source construction uses the finite literal `1e308`; it does not generate an infinity in Pine. The symbol fixture supplies positive finite mintick `0.01` through the real `--syminfo` setter.

The paired completion control is `arith/entry_stop_decimal_finite`. The native boundary file is `tests/test_value_admission_arith.cpp`, row `entry_stop_decimal_overflow`. The C++ near-boundary/bit-control rows are stronger arithmetic identity checks than this completion-only process probe.

Bars follow a hand-written constant-feed rule: 40 minute bars from Unix ms 1700000040000, every OHLC value 100, volume 1. No market data. Margins and slippage are zero. The script cancels outstanding orders and closes any position on bar 3. `min_trades=0` permits a valid remote stop to stay unfilled; status/report completion is mandatory.

Authored only: no transpilation, compile, engine, or probe run in this lane. If the real codegen refuses this finite literal or another construct, retain the pinned runtime expectation and report the first diagnostic; do not count the refusal as S runtime coverage or infer a different expected code from its output.
