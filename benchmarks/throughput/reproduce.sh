#!/usr/bin/env bash
set -e

# Resolve script directory and engine root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== PineForge Performance & Optimization Reproduction Pipeline ==="
echo "Engine root: $ENGINE_ROOT"
echo "reproduce directory: $SCRIPT_DIR"

if [[ "${RUNNER:-native}" == "docker" ]]; then
    # SECONDARY path: time throughput INSIDE the pineforge-release image
    # (run_json.py --bench, magnifier OFF → base-bar throughput). No host C++
    # toolchain. GBench (native, below) stays the authoritative published source.
    echo ""
    echo "Step 1+2 (docker): timing throughput via pineforge-release image (--bench)..."
    python3 "$SCRIPT_DIR/time_throughput_docker.py" \
        --strategies "$ENGINE_ROOT/benchmarks/assets/strategies" \
        --ohlcv "$ENGINE_ROOT/benchmarks/assets/data/ETHUSDT_15.csv" \
        ${IMAGE:+--image "$IMAGE"} > "$SCRIPT_DIR/throughput_docker.json"

    echo ""
    echo "Step 3 (docker): parsing results and generating quartile plot..."
    python3 "$SCRIPT_DIR/plot_quartile.py" \
        --results "$SCRIPT_DIR/throughput_docker.json" --format subproc
else
    # 1. Compile the engine and strategies
    echo ""
    echo "Step 1: Compiling engine and strategies in Release mode..."
    cmake -B "$ENGINE_ROOT/build" -S "$ENGINE_ROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPINEFORGE_BUILD_TESTS=ON \
        -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON
    cmake --build "$ENGINE_ROOT/build" --target bench_strategies -j4

    # 2. Run Google Benchmarks and export results to JSON
    echo ""
    echo "Step 2: Executing throughput benchmarks and exporting results..."
    "$ENGINE_ROOT/build/bin/pineforge_bench" \
        --benchmark_filter=".*throughput/no_magnifier.*" \
        --benchmark_out="$SCRIPT_DIR/benchmark_results.json" \
        --benchmark_out_format=json

    # 3. Compute stats and draw quartile plot using Matplotlib
    echo ""
    echo "Step 3: Parsing benchmark results and generating quartile plot..."
    python3 "$SCRIPT_DIR/plot_quartile.py"
fi

# 4. Run parameter sweep / grid search on wunder-bots
echo ""
echo "Step 4: Running strategy grid search optimization sweep..."
python3 "$SCRIPT_DIR/grid_search_repro.py"

echo ""
echo "=== REPRODUCTION RUN COMPLETED SUCCESSFULLY ==="
echo "Matplotlib plot: $SCRIPT_DIR/throughput_quartiles.png"
echo "Benchmark JSON:  $SCRIPT_DIR/benchmark_results.json"
