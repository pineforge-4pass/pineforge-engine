#!/usr/bin/env python3
import ctypes
import csv
import itertools
from pathlib import Path

class pf_bar_t(ctypes.Structure):
    _fields_ = [
        ("open", ctypes.c_double),
        ("high", ctypes.c_double),
        ("low", ctypes.c_double),
        ("close", ctypes.c_double),
        ("volume", ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]

class pf_trade_t(ctypes.Structure):
    _fields_ = [
        ("entry_time", ctypes.c_int64),
        ("exit_time", ctypes.c_int64),
        ("entry_price", ctypes.c_double),
        ("exit_price", ctypes.c_double),
        ("pnl", ctypes.c_double),
        ("pnl_pct", ctypes.c_double),
        ("is_long", ctypes.c_int),
        ("max_runup", ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty", ctypes.c_double),
    ]

class pf_security_diag_t(ctypes.Structure):
    _fields_ = [
        ("sec_id", ctypes.c_int),
        ("feed_count", ctypes.c_int64),
        ("complete_count", ctypes.c_int64),
        ("partial_count", ctypes.c_int64),
    ]

class pf_trace_entry_t(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id", ctypes.c_int32),
        ("value", ctypes.c_double),
    ]

class pf_report_t(ctypes.Structure):
    _fields_ = [
        ("total_trades", ctypes.c_int),
        ("trades", ctypes.POINTER(pf_trade_t)),
        ("trades_len", ctypes.c_int),
        ("net_profit", ctypes.c_double),

        ("input_bars_processed", ctypes.c_int64),
        ("script_bars_processed", ctypes.c_int64),

        ("security_feeds_total", ctypes.c_int64),
        ("security_complete_total", ctypes.c_int64),
        ("security_partial_total", ctypes.c_int64),

        ("magnifier_sub_bars_total", ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),

        ("input_tf_seconds", ctypes.c_int),
        ("script_tf_seconds", ctypes.c_int),
        ("script_tf_ratio", ctypes.c_int),
        ("needs_aggregation", ctypes.c_int),
        ("bar_magnifier_enabled", ctypes.c_int),

        ("security_diag", ctypes.POINTER(pf_security_diag_t)),
        ("security_diag_len", ctypes.c_int),

        ("trace", ctypes.POINTER(pf_trace_entry_t)),
        ("trace_len", ctypes.c_int),
        ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len", ctypes.c_int),
    ]

def main():
    repo_root = Path(__file__).parent.parent.resolve()
    dylib_path = repo_root / "benchmarks/assets/strategies/19-scalping-wunder-bots/strategy.dylib"
    csv_path = repo_root / "benchmarks/assets/data/ETHUSDT_15.csv"

    if not dylib_path.exists():
        # Fallback to shared library extension suffix on Linux
        dylib_path = dylib_path.with_suffix(".so")

    if not dylib_path.exists():
        print(f"Error: Compiled strategy library not found at: {dylib_path}")
        print("Please compile the strategies first using: cmake --build build --target bench_strategies")
        return

    if not csv_path.exists():
        print(f"Error: OHLCV CSV file not found at: {csv_path}")
        return

    # Load shared library
    lib = ctypes.CDLL(str(dylib_path))

    # Declare functions
    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype = ctypes.c_void_p

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.strategy_free.restype = None

    lib.strategy_set_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_input.restype = None

    lib.run_backtest.argtypes = [ctypes.c_void_p, ctypes.POINTER(pf_bar_t), ctypes.c_int, ctypes.POINTER(pf_report_t)]
    lib.run_backtest.restype = None

    lib.report_free.argtypes = [ctypes.POINTER(pf_report_t)]
    lib.report_free.restype = None

    # Load CSV
    bars_list = []
    print(f"Loading CSV data from {csv_path.name}...")
    with open(csv_path, "r") as f:
        reader = csv.reader(f)
        next(reader)  # Skip header: timestamp,open,high,low,close,volume
        for row in reader:
            if len(row) >= 6:
                b = pf_bar_t()
                b.timestamp = int(row[0])
                b.open = float(row[1])
                b.high = float(row[2])
                b.low = float(row[3])
                b.close = float(row[4])
                b.volume = float(row[5])
                bars_list.append(b)

    n = len(bars_list)
    bars_array = (pf_bar_t * n)(*bars_list)
    print(f"Loaded {n} bars successfully.")

    # Grid Search Parameters
    fast_ma_range = [7, 9, 11]
    slow_ma_range = [19, 21, 23]
    risk_reward_range = [1.5, 2.0, 2.5]

    best_profit = -float("inf")
    best_params = None

    print(f"Starting parameter sweep on 19-scalping-wunder-bots strategy across {len(fast_ma_range)*len(slow_ma_range)*len(risk_reward_range)} combinations...")

    for fast_ma, slow_ma, rr in itertools.product(fast_ma_range, slow_ma_range, risk_reward_range):
        s = lib.strategy_create(None)
        if not s:
            print("Failed to create strategy instance!")
            continue

        # Set inputs
        lib.strategy_set_input(s, b"Fast MA Length", str(fast_ma).encode())
        lib.strategy_set_input(s, b"Slow MA Length", str(slow_ma).encode())
        lib.strategy_set_input(s, b"Risk:Reward Ratio", f"{rr:.1f}".encode())

        report = pf_report_t()
        lib.run_backtest(s, bars_array, n, ctypes.byref(report))

        profit = report.net_profit
        trades = report.total_trades

        print(f"Params: Fast_MA={fast_ma}, Slow_MA={slow_ma}, R:R={rr:.1f} | Net Profit = {profit:.2f} | Trades = {trades}")

        if profit > best_profit:
            best_profit = profit
            best_params = (fast_ma, slow_ma, rr)

        lib.report_free(ctypes.byref(report))
        lib.strategy_free(s)

    print("\n=== OPTIMIZATION COMPLETE ===")
    print(f"Best Profit: {best_profit:.2f}")
    print(f"Best Parameters: Fast MA = {best_params[0]}, Slow MA = {best_params[1]}, Risk:Reward = {best_params[2]:.1f}")

if __name__ == "__main__":
    main()
