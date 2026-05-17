#!/usr/bin/env bash
# scripts/regen_validation_artifacts.sh — regenerate corpus validation
# report in all 3 formats (Markdown, HTML, PDF) from current corpus state.
#
# Pipeline:
#   1. regen_validation_report.py    → corpus/validation_report.md
#   2. pandoc standalone HTML        → corpus/validation_report.html
#   3. headless Chrome print-to-pdf  → corpus/validation_report.pdf
#
# Prerequisite: the corpus must have up-to-date engine_trades.csv files
# next to each strategy.pine — i.e. scripts/run_corpus.sh has been run
# (which itself calls regen_validation_report.py at the end and produces
# only the .md). This script picks up where that leaves off and adds the
# HTML + PDF artifacts using the same Markdown source.
#
# Honours these env vars:
#   PANDOC      — pandoc binary (default: pandoc)
#   CHROME      — Chrome / Chromium / Edge binary
#                 (default: /Applications/Google Chrome.app/Contents/MacOS/Google Chrome)
#   SKIP_MD     — set to 1 to skip the regen_validation_report.py step
#                 (use when md is already current from run_corpus.sh)
#
# Output files live at corpus/validation_report.{md,html,pdf}.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS_DIR="${ROOT_DIR}/corpus"
MD="${CORPUS_DIR}/validation_report.md"
HTML="${CORPUS_DIR}/validation_report.html"
PDF="${CORPUS_DIR}/validation_report.pdf"

PANDOC="${PANDOC:-pandoc}"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"

# --- Step 1: regenerate Markdown -------------------------------------------
if [[ "${SKIP_MD:-0}" != "1" ]]; then
    echo "[1/3] regen Markdown via regen_validation_report.py …"
    python3 "${ROOT_DIR}/scripts/regen_validation_report.py" --output "${MD}"
else
    echo "[1/3] SKIP_MD=1 — reusing existing ${MD}"
    [[ -f "${MD}" ]] || { echo "ERROR: ${MD} missing; cannot SKIP_MD"; exit 1; }
fi

# --- Step 2: Markdown → standalone HTML ------------------------------------
echo "[2/3] pandoc Markdown → HTML …"
command -v "${PANDOC}" >/dev/null 2>&1 || {
    echo "ERROR: pandoc not found at '${PANDOC}'. Install via:"
    echo "  brew install pandoc   (macOS)"
    echo "  apt install pandoc    (Debian/Ubuntu)"
    exit 1
}
CSS="${ROOT_DIR}/scripts/validation_report.css"
"${PANDOC}" --standalone --metadata "title=PineForge Corpus Validation Report" \
    --from gfm --to html5 \
    --css "${CSS}" --embed-resources \
    --output "${HTML}" "${MD}"

# --- Step 3: HTML → PDF via headless Chrome --------------------------------
echo "[3/3] Chrome headless → PDF …"
if [[ ! -x "${CHROME}" ]]; then
    echo "WARNING: Chrome not found at '${CHROME}'. Skipping PDF generation."
    echo "         Override with CHROME=/path/to/chrome to enable."
    exit 0
fi
# Chrome prints whatever's at the URL. file:// scheme needs absolute path.
# Chrome's --print-to-pdf honours CSS @page rules; we set landscape A4
# in scripts/validation_report.css so the wide per-strategy table fits.
# --virtual-time-budget gives the page CSS time to apply before snapshot.
"${CHROME}" \
    --headless \
    --disable-gpu \
    --no-pdf-header-footer \
    --virtual-time-budget=5000 \
    --print-to-pdf="${PDF}" \
    --print-to-pdf-no-header \
    "file://${HTML}" 2>/dev/null

echo
echo "Done. Artifacts at:"
echo "  ${MD}"
echo "  ${HTML}"
echo "  ${PDF}"
