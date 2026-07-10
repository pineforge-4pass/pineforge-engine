# FFI from Python {#ffi_python}

@tableofcontents

The C ABI is FFI-friendly by design: a compact function set, 11 POD
structs, one enum, no callbacks, no opaque types except `pf_strategy_t`
(which is `void*`). This page shows the canonical `ctypes` wiring for Python; any
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

class pf_trade_tick_t(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("sequence",  ctypes.c_uint64),
        ("price",     ctypes.c_double),
        ("quantity",  ctypes.c_double),
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
        ("commission",      ctypes.c_double),   # ABI v2
        ("entry_bar_index", ctypes.c_int32),    # ABI v2
        ("exit_bar_index",  ctypes.c_int32),    # ABI v2
    ]

class pf_trade_stats_t(ctypes.Structure):
    """ABI v2 — one block each for all / long-only / short-only trades."""
    _fields_ = [
        ("num_trades", ctypes.c_int32), ("num_wins", ctypes.c_int32),
        ("num_losses", ctypes.c_int32), ("num_even", ctypes.c_int32),
        ("percent_profitable", ctypes.c_double),
        ("net_profit", ctypes.c_double), ("net_profit_pct", ctypes.c_double),
        ("gross_profit", ctypes.c_double), ("gross_profit_pct", ctypes.c_double),
        ("gross_loss", ctypes.c_double), ("gross_loss_pct", ctypes.c_double),
        ("profit_factor", ctypes.c_double),
        ("avg_trade", ctypes.c_double), ("avg_trade_pct", ctypes.c_double),
        ("avg_win", ctypes.c_double), ("avg_win_pct", ctypes.c_double),
        ("avg_loss", ctypes.c_double), ("avg_loss_pct", ctypes.c_double),
        ("ratio_avg_win_avg_loss", ctypes.c_double),
        ("largest_win", ctypes.c_double), ("largest_win_pct", ctypes.c_double),
        ("largest_loss", ctypes.c_double), ("largest_loss_pct", ctypes.c_double),
        ("commission_paid", ctypes.c_double),
        ("expectancy", ctypes.c_double),
        ("max_consecutive_wins", ctypes.c_int32), ("max_consecutive_losses", ctypes.c_int32),
        ("avg_bars_in_trade", ctypes.c_double), ("avg_bars_in_wins", ctypes.c_double),
        ("avg_bars_in_losses", ctypes.c_double),
    ]

class pf_equity_stats_t(ctypes.Structure):
    """ABI v2 — equity-curve-derived stats (all-trades only)."""
    _fields_ = [
        ("max_equity_drawdown", ctypes.c_double), ("max_equity_drawdown_pct", ctypes.c_double),
        ("max_equity_runup", ctypes.c_double), ("max_equity_runup_pct", ctypes.c_double),
        ("buy_hold_return", ctypes.c_double), ("buy_hold_return_pct", ctypes.c_double),
        ("sharpe_tv", ctypes.c_double), ("sortino_tv", ctypes.c_double),
        ("sharpe_bar", ctypes.c_double), ("sortino_bar", ctypes.c_double),
        ("cagr", ctypes.c_double), ("calmar", ctypes.c_double),
        ("recovery_factor", ctypes.c_double), ("time_in_market_pct", ctypes.c_double),
        ("open_pl", ctypes.c_double),
    ]

class pf_metrics_t(ctypes.Structure):
    """ABI v2 — composite metrics container."""
    _fields_ = [("all", pf_trade_stats_t), ("longs", pf_trade_stats_t),
                ("shorts", pf_trade_stats_t), ("equity", pf_equity_stats_t)]

class pf_equity_point_t(ctypes.Structure):
    """ABI v2 — one per-script-bar equity point."""
    _fields_ = [("time_ms", ctypes.c_int64), ("equity", ctypes.c_double),
                ("open_profit", ctypes.c_double)]

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

        # ABI v2: computed metrics + per-script-bar equity curve
        ("metrics",                      pf_metrics_t),
        ("equity_curve",                 ctypes.POINTER(pf_equity_point_t)),
        ("equity_curve_len",             ctypes.c_int64),  # int64 in the C header, NOT c_int
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

## Loading a strategy .so

Each compiled PineForge strategy `.so` exports the public ABI symbols
itself. Open it with `ctypes.CDLL`:

```python
lib = ctypes.CDLL("./my_strategy.so")

# ABI guard — pf_report_t is CALLER-allocated, so running an old .so
# against the v2 mirror above (or vice versa) silently corrupts memory.
# Verify the .so's layout version before any run:
EXPECTED_PF_ABI = 2   # PF_ABI_VERSION in <pineforge/pineforge.h>
try:
    lib.pf_abi_version.restype = ctypes.c_int
    abi = lib.pf_abi_version()
except AttributeError:
    raise RuntimeError(".so predates pf_abi_version (ABI v1); rebuild it")
if abi != EXPECTED_PF_ABI:
    raise RuntimeError(f"ABI mismatch: .so={abi}, mirror={EXPECTED_PF_ABI}")

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

lib.strategy_stream_begin.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(pf_bar_t), ctypes.c_int,
    ctypes.c_char_p, ctypes.c_char_p,
]
lib.strategy_stream_begin.restype = ctypes.c_int

lib.strategy_stream_push_tick.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(pf_trade_tick_t)]
lib.strategy_stream_push_tick.restype = ctypes.c_int

lib.strategy_stream_push_ticks.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(pf_trade_tick_t), ctypes.c_int]
lib.strategy_stream_push_ticks.restype = ctypes.c_int

lib.strategy_stream_advance_time.argtypes = [ctypes.c_void_p, ctypes.c_int64]
lib.strategy_stream_advance_time.restype = ctypes.c_int

lib.strategy_stream_end.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.strategy_stream_end.restype = ctypes.c_int

lib.strategy_stream_fill_report.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(pf_report_t)]
lib.strategy_stream_fill_report.restype = ctypes.c_int

lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
lib.strategy_get_last_error.restype = ctypes.c_char_p

lib.report_free.argtypes      = [ctypes.POINTER(pf_report_t)]
lib.report_free.restype       = None

lib.strategy_set_input.argtypes    = [ctypes.c_void_p,
                                      ctypes.c_char_p, ctypes.c_char_p]
lib.strategy_set_input.restype     = None

lib.strategy_set_override.argtypes = [ctypes.c_void_p,
                                      ctypes.c_char_p, ctypes.c_char_p]
lib.strategy_set_override.restype  = None
```

## Historical to realtime stream

Warmup and realtime calls operate on the same handle. The plural push accepts
one contiguous array and is useful for replay; it does not create chunks or
session boundaries inside the strategy.

```python
state = lib.strategy_create(None)

if lib.strategy_stream_begin(state, history, history_n, b"1", b"1") != 0:
    raise RuntimeError(lib.strategy_get_last_error(state).decode())

if lib.strategy_stream_push_ticks(state, ticks, len(ticks)) != 0:
    raise RuntimeError(lib.strategy_get_last_error(state).decode())

if lib.strategy_stream_advance_time(state, session_end_ms) != 0:
    raise RuntimeError(lib.strategy_get_last_error(state).decode())
if lib.strategy_stream_end(state, 0) != 0:
    raise RuntimeError(lib.strategy_get_last_error(state).decode())

report = pf_report_t()
if lib.strategy_stream_fill_report(state, ctypes.byref(report)) != 0:
    raise RuntimeError(lib.strategy_get_last_error(state).decode())

lib.report_free(ctypes.byref(report))
lib.strategy_free(state)
```

The complete runnable version is
[`tutorial/run_stream.py`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/tutorial/run_stream.py).
See [Historical to realtime streaming](@ref streaming) for validation,
clock advancement, and partial-bar rules.

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
  [`tutorial/run.py`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/tutorial/run.py)
  and a parameter-sweep variant in
  [`tutorial/run_advanced.py`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/tutorial/run_advanced.py).
- [Tutorial: MACD](@ref tutorial_macd) walks through the harness end-to-end.
