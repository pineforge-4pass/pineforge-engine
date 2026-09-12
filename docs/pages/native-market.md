# Native market host

The native host is a constructor-bound backtest/forward state machine. Pine
source commands, cap/priority adapters and source sizing stay on the
compatibility consumer. Native strategies subclass `NativeStrategyHost`, call
`configure_native`, then `run` / `stream_*`, and submit market requests from
`on_native_bar`.

One `NativeRunSpec` is the setup authority. Empty `chart_timezone` is optional
observation metadata and stays empty. Session `""` and `"24x7"` are distinct
all-day literals. Timeframe arguments on `run`/`stream_begin` must be omitted
or byte-identical to the spec.

Matching uses the existing confirmed-bar and tick drivers. Settlement inspects
FIFO/fees without mutation, admits openings against the frozen spec, then
commits once through `settle_native_execution_at`. Opening denial rejects the
entire crossing before old exposure closes.

C ABI adds `strategy_execution_contract` and `strategy_configure_native_v1`.
The runner accepts `--native-config FILE` with strict keys and refuses source
`--input`/`--override`/`--syminfo` on that path. Legacy identity bytes and the
existing live example are unchanged.

The additional example is `runner/examples/native_market_strategy.cpp`.
