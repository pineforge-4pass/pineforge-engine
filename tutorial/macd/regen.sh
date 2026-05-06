#!/usr/bin/env bash
# Regenerate tutorial/macd/generated.cpp from strategy.pine using the
# (proprietary) pineforge-codegen transpiler.
#
# Expects a sibling checkout at ../pineforge-codegen relative to the
# pineforge-engine repo root. Set PINEFORGE_CODEGEN_DIR to override.
#
# This script is for maintainers. Tutorial users do not need to run it
# — generated.cpp is checked in and stays in sync with strategy.pine.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CODEGEN_DIR="${PINEFORGE_CODEGEN_DIR:-${REPO_ROOT}/../pineforge-codegen}"

if [[ ! -d "${CODEGEN_DIR}" ]]; then
    echo "error: pineforge-codegen not found at ${CODEGEN_DIR}" >&2
    echo "       set PINEFORGE_CODEGEN_DIR to the checkout path" >&2
    exit 1
fi

PYTHONPATH="${CODEGEN_DIR}" python3 -c "
from pineforge_codegen import transpile
import sys
src = open('${SCRIPT_DIR}/strategy.pine').read()
sys.stdout.write(transpile(src))
" > "${SCRIPT_DIR}/generated.cpp.tmp"

mv "${SCRIPT_DIR}/generated.cpp.tmp" "${SCRIPT_DIR}/generated.cpp"
echo "regenerated: ${SCRIPT_DIR}/generated.cpp"
