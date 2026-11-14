#!/usr/bin/env bash
# Run from the engine worktree root, through the parent's spark2 test job.
set -euo pipefail
if [[ "${1:-}" == "--json" ]]; then
  shift
  exec python3 -B tests/test_run_json_syminfo_values.py --q716-json "$@"
fi
exec python3 -B tests/test_run_json_syminfo_values.py --q716-live-caller "$@"
