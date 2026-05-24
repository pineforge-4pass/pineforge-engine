# PineForge Performance & Optimization Reproduction Package

This directory contains the tools, scripts, and instructions to reproduce the high-performance backtest throughput benchmarks and strategy parameter optimizations for the PineForge engine.

## Materials Included

1. **`reproduce.sh`**: The end-to-end automation bash script.
2. **`plot_quartile.py`**: Python script using NumPy and Matplotlib to parse benchmark JSON, calculate exact throughput quartiles, and generate the boxplot chart (`throughput_quartiles.png`).
3. **`grid_search_repro.py`**: Multi-parameter grid search optimizer script using ctypes FFI to sweep parameters on the compiled `19-scalping-wunder-bots` library.

## How to Reproduce

### Prerequisites

Ensure you have Python 3, CMake, a C++17 compiler, and Python plotting dependencies installed:

```bash
pip install matplotlib numpy
```

### Run End-to-End Pipeline

Simply execute the main wrapper script from this directory:

```bash
chmod +x reproduce.sh
./reproduce.sh
```

This script will:
1. Recompile the backtest engine and all 100 benchmark strategies in **Release mode**.
2. Run Google Benchmark suites across all strategies for dynamic throughput measurement, exporting results to `benchmark_results.json`.
3. Read the benchmark results, compute the exact quartiles (Min, Q1, Median, Q3, Max), identify the position of the optimized Wunder-Bots strategy, and render a high-quality distribution boxplot to `throughput_quartiles.png`.
4. Run the parameter sweep grid search across 27 MA and Risk-to-Reward ratio combinations on `19-scalping-wunder-bots` to find and print the parameter configuration that maximizes strategy profit.
