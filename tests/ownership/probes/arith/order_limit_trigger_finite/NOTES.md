# order_limit_trigger_finite

The raw level / tick quotient is finite (about 1e307). The trigger must remain unchanged.

Pinned before execution: A completed finite run, with no typed failure. Source construction uses the finite literal `1e296`; it does not generate an infinity in Pine. The symbol fixture supplies positive finite mintick `1e-11` through the real `--syminfo` setter.

The paired overflow probe is `arith/order_limit_trigger_overflow`. The native boundary file is `tests/test_value_admission_arith.cpp`, row `order_limit_small_tick_bits`. The C++ near-boundary/bit-control rows are stronger arithmetic identity checks than this completion-only process probe.

Bars follow a hand-written constant-feed rule: 40 minute bars from Unix ms 1700000040000, every OHLC value 100, volume 1. No market data. Margins and slippage are zero. The script cancels outstanding orders and closes any position on bar 3. `min_trades=0` permits a valid remote stop to stay unfilled; status/report completion is mandatory.

Authored only: no transpilation, compile, engine, or probe run in this lane. If the real codegen refuses this finite literal or another construct, retain the pinned runtime expectation and report the first diagnostic; do not count the refusal as S runtime coverage or infer a different expected code from its output.
