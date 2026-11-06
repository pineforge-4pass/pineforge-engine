## Refactor workers (native-engine programme, R4 and later)

Applies to any agent — Claude, Codex, OpenCode — implementing a slice of the
native-engine refactor in this repository. The maintainers' campaign
repository (`pineforge-workflow`, private; its `AGENTS.md`, "Roles and dispatch")
governs who dispatches, reviews and measures; this section is what binds you here.

- You work in the worktree and branch your brief names, on the files it
  lists as yours, and nowhere else. A need in another file is reported to the
  supervisor, never edited quietly. You never push; the supervisor opens the
  PR after the one campaign sweep.
- A neutral refactor never opens the measured sources
  (`src/native_execution_consumer.cpp`, `src/native_order.cpp`,
  `src/engine_execution.cpp`, `src/engine_orders.cpp`, `src/engine_run.cpp`,
  `src/engine_path_resolve.cpp`, and the source-layer `src/source/*` units) and
  never edits the frozen native headers the settlement ABI checker names; the
  fixed parity population must stay byte-identical by construction, which the
  supervisor proves with the sweep, not you.
- C++17 only (`rg 'bit_cast|<bit>' src include tests scripts` = 0). Every
  added durable state is hashed. Existing numeric assertions and fixture
  outputs stay unchanged unless the contract lists the extension.
- Every new test unit is compiled first against the frozen previous-epoch
  header closure and its first diagnostic is recorded (fail-before). A test
  whose body is a seed row only, a `return 0` main, a disabled row or a TODO
  placeholder is a blocker to report, never evidence.
- Verification is local: `python3 scripts/ci_preflight.py`, then
  `python3 scripts/ci_verify.py release --build-dir <your dir> --jobs N` (all
  suites, the settlement ABI matrix, the checker self-tests); debug,
  sanitizers and native profiles when the brief asks. No campaign sweep from
  a worker.
- Your handoff is a report: HEAD sha, `git diff --stat <base>..HEAD`, the
  verification summary lines and log paths, every deviation from the
  contract with its reason, and every open question. The supervisor judges
  the slice by its executed result: no regression in the campaign sweep, the
  engine closer to an independent backtest + forward-execution state
  machine, and Pine-parity behaviour kept in codegen with this kernel clean.
