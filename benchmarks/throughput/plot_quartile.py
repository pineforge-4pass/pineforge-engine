#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

# Hardcoded fallback data from the canonical benchmark sweep
FALLBACK_DATA = {
    "30-atr-trailing-stop": 10.2842, "89-session-ny-spring-forward-dst-01": 4.43835, "22-hma-cross": 8.44481,
    "73-bracket-tp-sl-oca-reduce-isolate-01": 12.9491, "47-supertrend-adx-filter": 10.9581,
    "60-recompute-alma-sar-corr-magnifier-01": 9.81446, "66-bracket-entry-exit-same-pass-attach-01": 8.66733,
    "01-sma-cross": 15.6139, "77-composite-scalping-fast-ma-cross-trigger-01": 14.6982, "41-volume-breakout": 8.06837,
    "92-session-hour-minute-pulse-filter-01": 9.75799, "17-bos-curv": 5.23026, "19-scalping-wunder-bots": 6.71711,
    "72-bracket-same-id-exit-replace-01": 11.3443, "32-momentum-roc": 10.1168, "07-scalping-strategy": 9.73676,
    "58-oca-multi-bracket-isolation-01": 10.2818, "67-bracket-exit-stop-limit-trail-same-bar-01": 8.29065,
    "10-market-shift": 3.94368, "90-ta-hma-55-close-cross-01": 8.76282, "57-oca-exit-bracket-internal-cancel-01": 15.8048,
    "36-pivot-array-breakout": 11.808, "54-composite-ies-integration-01": 5.48092, "42-ma-stack-array": 9.49646,
    "46-rsi-bands": 6.64375, "35-ema-ribbon-loop": 13.6557, "44-median-cross": 4.12015, "49-partial-exit-qty-percent": 7.29115,
    "02-inside-bar": 10.5221, "20-bb-squeeze": 7.91206, "51-order-deferred-flip-guaranteed-gap-stops-01": 6.97677,
    "62-analyzer-parity-percent-of-equity-sizing-01": 11.739, "79-composite-kanuck-kama-state-recurrence-01": 10.0005,
    "05-stoch-rsi": 8.81798, "40-dual-thrust": 9.46596, "09-kkb-kalman": 3.82787, "04-macd-histogram": 11.0421,
    "06-liquidity-sweep": 4.39122, "78-cap-max-intraday-filled-orders-isolate-01": 9.97632, "21-dmi-adx-trend": 10.9347,
    "64-composite-vcp-cumulative-volume-delta-01": 10.0343, "99-matrix-eigen-rank-deficient-cov-01": 2.85621,
    "95-cap-risk-gates-allow-max-intraday-01": 5.04925, "93-analyzer-parity-stop-limit-timing-01": 8.10091,
    "16-volty-expan": 3.79325, "48-bracket-exit-tp-sl": 11.3662, "80-magnifier-tick-dist-volume-weighted-on-01": 13.2497,
    "45-multi-indicator-score": 6.85971, "61-analyzer-parity-edge-margin-50-pct-01": 14.9753, "63-analyzer-parity-small-equity-fraction-01": 11.3167,
    "70-bracket-narrow-stop-limit-with-trail8-01": 8.55894, "26-aroon-oscillator": 7.64056, "56-composite-vcp-integration-01": 2.42044,
    "68-bracket-exit-three-way-set-once-entry-01": 9.50886, "08-4ema-rsi": 6.21109, "52-barstate-isconfirmed-magnifier-off-01b": 15.7893,
    "18-kanuck": 1.13966, "100-matrix-bool-mask-no-transpose-01": 6.17178, "15-stochastic-slow": 10.1032,
    "29-chandelier-exit": 9.89814, "59-order-deferred-flip-pooc-cross-bar-01": 6.14561, "12-keltner": 12.6169,
    "39-candle-pattern": 11.4942, "71-bracket-partial-exit-qty-percent-01": 8.83626, "13-stoch-slow-k-d-cross": 8.3867,
    "65-bracket-atr-trail-series-int-points-01": 9.44458, "11-greedy": 13.1655, "55-composite-ies-pressure-gauge-01": 11.0542,
    "97-composite-scalping-integration-01": 12.8678, "24-tsi-signal": 12.5412, "69-bracket-exit-tp-sl-fixed-01": 14.94,
    "86-order-range-expansion-pending-stop-01": 8.27964, "84-na-nz-fixnan-history-chain-01": 11.8233, "83-matrix-bool-mask-explicit-utc-tz-01": 1.07311,
    "82-matrix-covariance-eigen-pca-01": 2.24329, "75-composite-4emarsi-rsi-pullback-latch-01": 13.8781, "81-magnifier-tick-dist-endpoints-rsi-cross-08a": 10.9636,
    "50-close-immediate-vs-next-bar": 6.45153, "03-supertrend": 16.5214, "94-ta-hma-fast-slow-cross-01": 10.0116,
    "23-cci-momentum": 9.22683, "85-oca-raw-strategy-order-reduce-01": 13.874, "27-donchian-breakout": 9.91889,
    "88-order-stop-entry-cancel-opposite-01": 5.4072, "25-linreg-channel": 7.34472, "98-magnifier-tick-dist-endpoints-01": 18.1,
    "28-elder-ray": 10.0967, "14-pivot-ext": 9.83478, "38-adaptive-ma-func": 10.5381, "74-bracket-trail-points-no-offset-explicit-01": 9.29146,
    "76-analyzer-parity-choch-bos-isolator-01": 14.1159, "33-mean-reversion-bb": 9.28362, "53-barstate-isconfirmed-magnifier-on-01a": 25.8957,
    "87-pyramid-deferred-flip-close-all-01": 7.833, "91-pyramid-close-id-grouping-01": 3.69034, "96-composite-ies-rsi-macd-momentum-01": 7.94107,
    "31-vwma-divergence": 13.5272, "34-dual-ma-switch": 14.7955, "43-swing-pivot-atr": 7.7213, "37-range-filter-while": 21.4964
}

def load_dynamic_data(json_path: Path) -> dict[str, float]:
    """Load throughput results from Google Benchmark JSON output."""
    if not json_path.exists():
        print(f"Warning: dynamic results file '{json_path}' not found. Using fallback data.")
        return FALLBACK_DATA

    try:
        with open(json_path, "r") as f:
            bench_json = json.load(f)

        results = {}
        for b in bench_json.get("benchmarks", []):
            name = b.get("name", "")
            # Filter specifically for throughput / no_magnifier tests
            if "throughput/no_magnifier" in name:
                # Extract strategy slug from name (e.g., '01-sma-cross/throughput/no_magnifier/...')
                slug = name.split("/")[0]
                # Extract items_per_second and convert to Millions of items per second (M/s)
                items_sec = b.get("items_per_second")
                if items_sec is not None:
                    results[slug] = float(items_sec) / 1e6

        if not results:
            print("Warning: No matching throughput metrics in JSON. Using fallback data.")
            return FALLBACK_DATA

        return results
    except Exception as e:
        print(f"Error parsing JSON: {e}. Using fallback data.")
        return FALLBACK_DATA

def load_subproc_data(json_path: Path) -> dict[str, float]:
    """Load throughput results from the docker --bench driver
    (time_throughput_docker.py): {slug: {m_per_s, ...}} → {slug: M/s}.

    This is the SECONDARY, no-toolchain path. The from-source GBench JSON
    (load_dynamic_data) stays authoritative for the published chart.
    """
    if not json_path.exists():
        print(f"Warning: subproc results file '{json_path}' not found. Using fallback data.")
        return FALLBACK_DATA
    try:
        with open(json_path, "r") as f:
            raw = json.load(f)
        results = {}
        for slug, rec in raw.items():
            m = rec.get("m_per_s") if isinstance(rec, dict) else rec
            if m is not None:
                results[slug] = float(m)
        if not results:
            print("Warning: No m_per_s entries in subproc JSON. Using fallback data.")
            return FALLBACK_DATA
        return results
    except Exception as e:
        print(f"Error parsing subproc JSON: {e}. Using fallback data.")
        return FALLBACK_DATA


def main():
    ap = argparse.ArgumentParser(description="PineForge throughput quartile plot")
    ap.add_argument("--results", type=Path, default=None,
                    help="results JSON (default: benchmark_results.json next to this script)")
    ap.add_argument("--format", choices=["gbench", "subproc"], default="gbench",
                    help="gbench=from-source GBench JSON (default/authoritative); "
                         "subproc=docker --bench driver output (time_throughput_docker.py)")
    args = ap.parse_args()

    json_path = args.results or (Path(__file__).parent / "benchmark_results.json")
    if args.format == "subproc":
        results = load_subproc_data(json_path)
    else:
        results = load_dynamic_data(json_path)

    data = list(results.values())
    data.sort()

    # Calculate exact quartiles
    min_v = data[0]
    q1 = np.percentile(data, 25)
    med = np.percentile(data, 50)
    q3 = np.percentile(data, 75)
    max_v = data[-1]

    print("\n--- THROUGHPUT ANALYSIS ---")
    print(f"Total Strategies (N): {len(data)}")
    print(f"Minimum: {min_v:.3f} M/s")
    print(f"Q1 (25th Percentile): {q1:.3f} M/s")
    print(f"Median (50th Percentile): {med:.3f} M/s")
    print(f"Q3 (75th Percentile): {q3:.3f} M/s")
    print(f"Maximum: {max_v:.3f} M/s")

    # Render boxplot using Matplotlib
    plt.figure(figsize=(10, 6))

    # Set dark theme styling
    plt.style.use('seaborn-v0_8-whitegrid' if 'seaborn-v0_8-whitegrid' in plt.style.available else 'default')

    bp = plt.boxplot(data, vert=False, patch_artist=True, widths=0.4,
                     boxprops=dict(facecolor="#58a6ff", color="#1f6feb", linewidth=2),
                     whiskerprops=dict(color="#1f6feb", linewidth=2),
                     capprops=dict(color="#1f6feb", linewidth=2),
                     medianprops=dict(color="#d12420", linewidth=3),
                     flierprops=dict(marker='o', markerfacecolor='#f78166', markersize=8, markeredgecolor='none'))

    # Individual scatter points (jittered slightly for visibility)
    y_jitter = np.random.normal(1, 0.04, size=len(data))
    plt.scatter(data, y_jitter, color="#f78166", alpha=0.4, label="Individual Strategy", zorder=3, edgecolors='none', s=40)

    # Label key metrics on the chart
    plt.text(min_v, 1.25, f"Min\n{min_v:.2f}", ha='center', fontweight='bold', color="#1f6feb")
    plt.text(q1, 0.73, f"Q1\n{q1:.2f}", ha='center', fontweight='bold', color="#1f6feb")
    plt.text(med, 1.25, f"Median\n{med:.2f}", ha='center', fontweight='bold', color="#d12420")
    plt.text(q3, 0.73, f"Q3\n{q3:.2f}", ha='center', fontweight='bold', color="#1f6feb")
    plt.text(max_v, 1.25, f"Max\n{max_v:.2f}", ha='center', fontweight='bold', color="#1f6feb")

    plt.title("PineForge Strategy Backtest Throughput Distribution (N=100)", fontsize=14, fontweight='bold', pad=20)
    plt.xlabel("Backtest Throughput (Millions of Bars per Second - M/s)", fontsize=12, labelpad=10)
    plt.yticks([], []) # Hide standard y-axis numbers
    plt.ylim(0.5, 1.6)
    plt.legend(loc="lower right", frameon=True, facecolor="white", edgecolor="#e1e4e8")
    plt.tight_layout()

    output_path = Path(__file__).parent / "throughput_quartiles.png"
    plt.savefig(output_path, dpi=150)
    print(f"\nMatplotlib chart successfully saved to: {output_path.resolve()}")

if __name__ == "__main__":
    main()
