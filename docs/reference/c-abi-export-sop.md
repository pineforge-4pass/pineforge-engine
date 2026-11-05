## SOP: adding a runtime `PF_API` export (CI gate — recurring failure)

CI runs `python3 scripts/check_c_abi_runtime.py` as the `source-guard-c-abi`
stage of `scripts/ci_preflight.py` and of every `scripts/ci_verify.py` profile,
before anything is built. It pins the exact set of `PF_API` symbols implemented
in `src/c_abi.cpp` against the hardcoded `EXPECTED_RUNTIME` frozenset in that
script. Adding (or removing) a runtime export WITHOUT updating that list fails
the `source-guard-c-abi` stage, and with it preflight and every profile, even
though the build and ctest would be green.

Checklist when touching runtime exports — update ALL of these together:

1. `src/c_abi.cpp` — the implementation (and its file-header symbol comment).
2. `include/pineforge/pineforge.h` — the `PF_API` declaration (+ doxygen).
3. `scripts/check_c_abi_runtime.py` — add the symbol to `EXPECTED_RUNTIME` and
   bump `EXPECTED_PUBLIC_DECLARATIONS` / `EXPECTED_RUNTIME_IMPLEMENTATIONS`.
4. Python ctypes harnesses if consumers must call it
   (`scripts/run_strategy.py`, `tutorial/run*.py`, `docker/run_json.py`,
   `benchmarks/throughput/grid_search_repro.py`).
5. README symbol table if it enumerates exports.

Before pushing: `python3 scripts/check_c_abi_runtime.py` (must exit 0).
Per-strategy symbols (strategy_create, run_backtest, …) are NOT in this list —
they are codegen-emitted; the checker enforces exactly that split.
