# FFI from Python {#ffi_python}

@tableofcontents

The C ABI is FFI-friendly by design: 10 functions, 6 POD structs, one
enum, no callbacks, no opaque types except `pf_strategy_t` (which is
`void*`). This page shows the canonical `ctypes` wiring for Python; any
language with a C-FFI (Rust `libc`, Go `cgo`, Node `ffi-napi`,
Julia `ccall`) follows the same shape.

## Full ctypes mirror

A complete, paste-able mirror of `<pineforge/pineforge.h>`:

```python
import ctypes

class pf_bar_t(ctypes.Structure):
    _fields_ = [
        ("open",      ctypes.c_double),
        ("high",      ctypes.c_double),
        ("low",       ctypes.c_double),
        ("close",     ctypes.c_double),
        ("volume",    ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]

class pf_trade_t(ctypes.Structure):
    _fields_ = [
        ("entry_time",   ctypes.c_int64),
        ("exit_time",    ctypes.c_int64),
        ("entry_price",  ctypes.c_double),
        ("exit_price",   ctypes.c_double),
        ("pnl",          ctypes.c_double),
        ("pnl_pct",      ctypes.c_double),
        ("is_long",      ctypes.c_int),
        ("max_runup",    ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty",          ctypes.c_double),
    ]

class pf_security_diag_t(ctypes.Structure):
    _fields_ = [
        ("sec_id",         ctypes.c_int),
        ("feed_count",     ctypes.c_int64),
        ("complete_count", ctypes.c_int64),
        ("partial_count",  ctypes.c_int64),
    ]

class pf_trace_entry_t(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id",   ctypes.c_int32),
        ("value",     ctypes.c_double),
    ]

class pf_report_t(ctypes.Structure):
    _fields_ = [
        ("total_trades",                 ctypes.c_int),
        ("trades",                       ctypes.POINTER(pf_trade_t)),
        ("trades_len",                   ctypes.c_int),
        ("net_profit",                   ctypes.c_double),

        ("input_bars_processed",         ctypes.c_int64),
        ("script_bars_processed",        ctypes.c_int64),

        ("security_feeds_total",         ctypes.c_int64),
        ("security_complete_total",      ctypes.c_int64),
        ("security_partial_total",       ctypes.c_int64),

        ("magnifier_sub_bars_total",     ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),

        ("input_tf_seconds",             ctypes.c_int),
        ("script_tf_seconds",            ctypes.c_int),
        ("script_tf_ratio",              ctypes.c_int),
        ("needs_aggregation",            ctypes.c_int),
        ("bar_magnifier_enabled",        ctypes.c_int),

        ("security_diag",                ctypes.POINTER(pf_security_diag_t)),
        ("security_diag_len",            ctypes.c_int),

        ("trace",                        ctypes.POINTER(pf_trace_entry_t)),
        ("trace_len",                    ctypes.c_int),
        ("trace_names",                  ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len",              ctypes.c_int),
    ]

class pf_version_t(ctypes.Structure):
    _fields_ = [
        ("major",      ctypes.c_int),
        ("minor",      ctypes.c_int),
        ("patch",      ctypes.c_int),
        ("commit_sha", ctypes.c_char_p),
    ]

# Magnifier distribution
PF_MAGNIFIER_UNIFORM      = 0
PF_MAGNIFIER_COSINE       = 1
PF_MAGNIFIER_TRIANGLE     = 2
PF_MAGNIFIER_ENDPOINTS    = 3   # default
PF_MAGNIFIER_FRONT_LOADED = 4
PF_MAGNIFIER_BACK_LOADED  = 5
```

## Loading a strategy `.so`

Each compiled PineForge strategy `.so` exports the public ABI symbols
itself. Open it with `ctypes.CDLL`:

```python
lib = ctypes.CDLL("./my_strategy.so")

lib.strategy_create.argtypes  = [ctypes.c_char_p]
lib.strategy_create.restype   = ctypes.c_void_p

lib.strategy_free.argtypes    = [ctypes.c_void_p]
lib.strategy_free.restype     = None

lib.run_backtest.argtypes     = [ctypes.c_void_p,
                                 ctypes.POINTER(pf_bar_t), ctypes.c_int,
                                 ctypes.POINTER(pf_report_t)]
lib.run_backtest.restype      = None

lib.run_backtest_full.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(pf_bar_t), ctypes.c_int,
    ctypes.c_char_p, ctypes.c_char_p,    # input_tf, script_tf
    ctypes.c_int, ctypes.c_int, ctypes.c_int,  # magnifier, samples, dist
    ctypes.POINTER(pf_report_t),
]
lib.run_backtest_full.restype = None

lib.report_free.argtypes      = [ctypes.POINTER(pf_report_t)]
lib.report_free.restype       = None

lib.strategy_set_input.argtypes    = [ctypes.c_void_p,
                                      ctypes.c_char_p, ctypes.c_char_p]
lib.strategy_set_input.restype     = None

lib.strategy_set_override.argtypes = [ctypes.c_void_p,
                                      ctypes.c_char_p, ctypes.c_char_p]
lib.strategy_set_override.restype  = None
```

## End-to-end run

```python
import csv

# Load OHLCV
bars = (pf_bar_t * n)()
with open("ohlcv.csv") as f:
    for i, row in enumerate(csv.DictReader(f)):
        bars[i] = pf_bar_t(
            float(row["open"]), float(row["high"]),
            float(row["low"]),  float(row["close"]),
            float(row["volume"]), int(row["timestamp"]),
        )

s      = lib.strategy_create(b"{}")
report = pf_report_t()

lib.strategy_set_override(s, b"initial_capital", b"100000")
lib.strategy_set_input   (s, b"Length",          b"21")

lib.run_backtest_full(s, bars, n, b"15", b"15",
                      0, 4, PF_MAGNIFIER_ENDPOINTS,
                      ctypes.byref(report))

print(f"trades:    {report.trades_len}")
print(f"net pnl:   {report.net_profit:+.2f}")
for i in range(report.trades_len):
    t = report.trades[i]
    print(f"  {'L' if t.is_long else 'S'} "
          f"{t.entry_price:.4f}->{t.exit_price:.4f}  pnl={t.pnl:+.2f}")

lib.report_free(ctypes.byref(report))
lib.strategy_free(s)
```

## Common pitfalls

| Pitfall | Fix |
| --- | --- |
| Forgot `ctypes.byref(report)` and got a segfault. | The report is **out** — pass by reference. |
| `commit_sha` truncated. | Use `c_char_p` and `.decode("utf-8")`, not a fixed-size buffer. |
| `int` parameters silently truncated. | Set `argtypes` explicitly — never rely on inference. |
| Tried to `report_free` twice. | Safe — it's idempotent. |
| Held `report.trace_names[i]` past `strategy_free`. | The strings live on the strategy. Copy before freeing. |
| Tried to share a handle across threads. | Don't — handles are not thread-safe. One handle per worker. |

## See also

- The full working example lives in
  [`tutorial/run.py`](https://github.com/fullpass-4pass/pineforge-engine/blob/main/tutorial/run.py)
  and a parameter-sweep variant in
  [`tutorial/run_advanced.py`](https://github.com/fullpass-4pass/pineforge-engine/blob/main/tutorial/run_advanced.py).
- [Tutorial: MACD](@ref tutorial_macd) walks through the harness end-to-end.
