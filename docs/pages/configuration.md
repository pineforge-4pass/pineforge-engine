# Configuration {#configuration}

@tableofcontents

PineForge exposes three configuration surfaces:

1. **Pine inputs** — anything declared with `input.*()` in the source script.
2. **Strategy declaration overrides** — fields of the script's `strategy(...)` call.
3. **Runtime knobs** — bar magnifier, trade-start gating, trace recording.

All three are set on the **strategy handle**, before calling
#run_backtest. Calls made after a run are queued for the next run.

## Pine inputs

```c
strategy_set_input(s, "Length",          "21");
strategy_set_input(s, "Use Trend Filter","true");
strategy_set_input(s, "Source",          "close");
```

| Pine type | Serialized form |
| --- | --- |
| `int`     | decimal string, e.g. `"21"` |
| `float`   | decimal string, e.g. `"0.04"` (use `.` always — no locale) |
| `bool`    | `"true"` / `"false"` |
| `string`  | the string itself (no quoting) |
| `source`  | one of `"open"`, `"high"`, `"low"`, `"close"`, `"hl2"`, `"hlc3"`, `"ohlc4"`, `"hlcc4"` |
| `color`   | hex `"#RRGGBB"` or `"#AARRGGBB"` |
| `timeframe` | TV-style — `"1"`, `"5"`, `"60"`, `"1D"`, `"1W"` |

The `key` is the input's title, exactly as it appears in the Pine
`title=` argument. If no title was given, the runtime falls back to
the variable identifier.

@note Unknown keys are silently accepted and ignored. This lets
harnesses set a superset of inputs across multiple strategies without
per-strategy gating.

## Strategy declaration overrides

```c
strategy_set_override(s, "initial_capital",        "100000");
strategy_set_override(s, "commission_value",       "0.04");
strategy_set_override(s, "commission_type",        "percent");  /* or "cash_per_contract", "cash_per_order" */
strategy_set_override(s, "slippage",               "1");
strategy_set_override(s, "default_qty_value",      "100");
strategy_set_override(s, "default_qty_type",       "percent_of_equity"); /* or "fixed", "cash" */
strategy_set_override(s, "pyramiding",             "0");
strategy_set_override(s, "process_orders_on_close","true");
strategy_set_override(s, "close_entries_rule",     "FIFO"); /* or "ANY" */
```

These mirror the parameters of Pine's `strategy(...)` declaration.
Setting them via the C ABI **overrides** the script-defined defaults
for this run.

## Runtime knobs

### Bar magnifier

Configured per-call on #run_backtest_full:

```c
run_backtest_full(s, bars, n, "5", "60",
                  /*magnifier=*/1, /*samples=*/4,
                  PF_MAGNIFIER_ENDPOINTS, &report);
```

Volume-weighted sampling is a separate sticky toggle:

```c
strategy_set_magnifier_volume_weighted(s, 1);
```

See [Bar magnifier](@ref magnifier) for the sampling model.

### Trace recording

```c
strategy_set_trace_enabled(s, 1);
```

Captures `// @pf-trace name=expr` pragma values per bar into
`pf_report_t::trace`. Zero-cost when disabled. See
[Report schema § Trace records](@ref report_schema).

### Trade start gate

```c
/* Only allow strategy.entry/exit/close/order to fire on or after this bar. */
strategy_set_trade_start_time(s, 1700000000000LL);
```

Earlier bars **still execute user code** and warm TA / series state.
Only order commands are ignored. Use this to leave room for indicators
to stabilize before trades start.

## Configuration is per-handle

Configuration values stick to the **handle**, not the run. They apply
to every #run_backtest on that handle until overwritten or
#strategy_free is called.

One-shot backtests reset broker and report state before every run, while these
configuration values persist on the handle. Fresh handles remain a clear
default for parameter sweeps (see [Lifecycle](@ref lifecycle)):

```c
for (int len = 10; len <= 30; len += 2) {
    pf_strategy_t s = strategy_create(NULL);
    strategy_set_override(s, "initial_capital", "100000");

    char buf[16]; snprintf(buf, sizeof buf, "%d", len);
    strategy_set_input(s, "Length", buf);

    pf_report_t r = {0};
    run_backtest(s, bars, n, &r);
    report_free(&r);
    strategy_free(s);
}
```
