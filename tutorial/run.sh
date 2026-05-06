#!/usr/bin/env bash
# PineForge tutorial — one-shot build + run.
#
# Configures CMake (if needed), builds the tutorial MACD strategy.so,
# and runs the Python harness against the checked-in 7-day BTCUSDT 15m
# OHLCV. Re-runs are incremental (CMake skips up-to-date targets).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" -DCMAKE_BUILD_TYPE=Release
fi

cmake --build "${BUILD_DIR}" --target strategy_tutorial_macd -j

python3 "${SCRIPT_DIR}/run.py" "$@"
