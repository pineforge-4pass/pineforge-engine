# Strategy lifecycle {#lifecycle}

@tableofcontents

A PineForge backtest is a four-step pipeline. Each step maps to one
function in `<pineforge/pineforge.h>`.

```
strategy_create()  →  configure()  →  run_backtest_full()  →  read pf_report_t
       │                  │                    │                       │
       ▼                  ▼                    ▼                       ▼
  pf_strategy_t      set_input /          fills pf_report_t       report_free()
                     set_override                                  strategy_free()
```

## 1. Allocate a strategy handle

```c
pf_strategy_t s = strategy_create(NULL);
if (!s) {
    /* allocation failure — out of memory */
    return -1;
}
```

The argument is reserved for future use; pass `NULL` today. Each handle
owns its own state machine and is **not thread-safe** — one handle per
worker thread.

## 2. Configure (optional)

Override Pine `input.*()` values, `strategy(...)` declaration params, or
runtime knobs **before** calling `run_backtest`. Calls made after a run
are accepted but only take effect on subsequent runs.

```c
strategy_set_input    (s, "Length",          "21");
strategy_set_input    (s, "Use Trend Filter","true");
strategy_set_override (s, "initial_capital", "100000");
strategy_set_override (s, "commission_value","0.04");
strategy_set_trace_enabled(s, 1);   /* enable @pf-trace capture */
```

Full list of recognised override keys is on the
[Configuration](@ref configuration) page.

## 3. Push bars and run

```c
pf_bar_t *bars = load_my_ohlcv(&n);
pf_report_t r  = {0};

run_backtest_full(s, bars, n,
                  /* input_tf  */ "15",
                  /* script_tf */ "15",
                  /* magnifier */ 1, /* samples */ 4,
                  PF_MAGNIFIER_ENDPOINTS,
                  &r);
```

Or for the common case of "auto-detect timeframe, no magnifier":

```c
run_backtest(s, bars, n, &r);
```

### Continue from historical bars into realtime trades

Use the stream lifecycle when the data source changes but the strategy
instance must not. `strategy_stream_begin()` executes the confirmed OHLCV
warmup once. Every later operation advances the same broker, equity, pending
orders, Pine variables, TA objects, `request.security()` evaluators, and any
partially formed higher-timeframe candle.

```c
pf_bar_t *history = load_confirmed_ohlcv(&history_n);

if (strategy_stream_begin(s, history, history_n, "1", "1") != 0)
    fail(strategy_get_last_error(s));

for (;;) {
    pf_trade_tick_t tick = next_exchange_trade();
    if (strategy_stream_push_tick(s, &tick) != 0)
        fail(strategy_get_last_error(s));

    /* Call on wall-clock boundaries too, including quiet markets. */
    strategy_stream_advance_time(s, current_time_ms());
    if (should_stop()) break;
}

strategy_stream_end(s, 0);  /* partial input bar remains unconfirmed */

pf_report_t r = {0};
strategy_stream_fill_report(s, &r);
```

The default strategy cadence remains close-only. Resting broker orders are
checked on every raw trade, so stop/limit fills use the observed exchange path
and a market order from the preceding close fills on the first subsequent
trade. `strategy_stream_push_ticks()` is the batch equivalent for replay and
reduces FFI overhead without changing tick semantics.

The warmup's last bar must be confirmed. Begin raw trades at or after the next
input-bar open. Call `strategy_stream_advance_time()` at confirmed boundaries;
it closes elapsed bars and creates zero-volume carry-forward bars for quiet
intervals. Normally end with `finalize_partial_input_bar = 0` to avoid treating
an open bar as confirmed.

See [Historical to realtime streaming](@ref streaming) for the complete tick
validation rules, a contiguous-replay example, and the runnable Python
tutorial.

The runtime fills `r` in place — the `pf_report_t` struct itself is
caller-owned (typically stack-allocated), but the arrays it points to
(`trades`, `security_diag`, `trace`, `trace_names`) are heap-allocated
inside the runtime.

## 4. Consume the report

See [Report schema](@ref report_schema) for every field. Quick summary:

```c
printf("Trades: %d  Net PnL: %.2f\n", r.trades_len, r.net_profit);

for (int i = 0; i < r.trades_len; ++i) {
    pf_trade_t t = r.trades[i];
    printf("  [%c] entry %.4f -> exit %.4f  pnl %.2f\n",
           t.is_long ? 'L' : 'S',
           t.entry_price, t.exit_price, t.pnl);
}
```

## 5. Free everything

Both calls are mandatory and idempotent. Order matters: free the
**report first**, then the **handle** — the report's `trace_names`
strings are owned by the live handle.

```c
report_free(&r);   /* releases trades / security_diag / trace arrays */
strategy_free(s);  /* releases the handle */
```

@warning Calling `strategy_free()` while `r.trace_names` is still in use
leaves dangling pointers — the trace name string table lives on the
strategy, not the report.

## Handle reuse and continuous streams

Calling #run_backtest or #run_backtest_full starts a new backtest and resets
per-run broker/report state. The `strategy_stream_*` lifecycle is the explicit
way to preserve and continue state across historical and realtime sources.

For parameter sweeps, walk-forward windows, or any A/B comparison,
**create a fresh handle per run**:

```c
for (int len = 10; len <= 30; len += 2) {
    pf_strategy_t s = strategy_create(NULL);

    char buf[16]; snprintf(buf, sizeof buf, "%d", len);
    strategy_set_input(s, "Length", buf);

    pf_report_t r = {0};
    run_backtest(s, bars, n, &r);
    printf("len=%d  pnl=%.2f\n", len, r.net_profit);

    report_free(&r);
    strategy_free(s);
}
```

This is the canonical sweep loop. See `tutorial/run_advanced.py` for the
Python equivalent and [Parameter sweep](@ref examples_python_sweep) for
the annotated walkthrough.

## Errors and partial state

The one-shot `run_backtest*` calls return through
#strategy_get_last_error. Stream lifecycle calls additionally return `0` on
success and `-1` on failure. In both cases, read
#strategy_get_last_error for the detailed message.

- **Allocation failure** in `strategy_create` returns `NULL`. Always check.
- **Empty bar feed** (`n == 0`) is valid — the report is filled with zero
  counts and an empty trade list.
- **Invalid timeframes, reordered ticks, and replayed trade ids** are rejected
  without unwinding a C++ exception across the C boundary.
