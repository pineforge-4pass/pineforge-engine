# CR-150 L7B handoff

HEAD: `e29f8c4ad0c8c746d593ed0a452e1a7542ed68a5`
Base: `ee3083d4a23fdae23d2cfa6ac254a8066de1b780`

Commits:

- `bd0191de8` — red point-source equity metrics witness.
- `e29f8c4ad` — array point-source implementation and citation refresh.

`git diff --stat ee3083d4a23fdae23d2cfa6ac254a8066de1b780..HEAD`:

```text
 docs/adr/0001-kernel-adapter-boundary.md |   4 +-
 docs/design/native-feature-parity.md     |   4 +-
 docs/native-refactor-progress.md         |   2 +-
 docs/pages/native-engine.md              |   2 +-
 src/engine_metrics.cpp                   | 169 +++++++++++++++++++++++--------
 tests/CMakeLists.txt                     |   5 +
 tests/test_metrics_point_source.cpp      | 139 +++++++++++++++++++++++++
 7 files changed, 276 insertions(+), 49 deletions(-)
```

## What changed

- `src/engine_metrics.cpp:23-31` defines the contracted `metrics::ArrayCurve`
  source, returning exact `pf_equity_point_t` values by value.
- `src/engine_metrics.cpp:52-124` replaces the temporary monthly-return and
  per-bar-return vectors with two-pass generators. The generated values and
  plain left folds retain the old operation order while using O(1) state.
- `src/engine_metrics.cpp:263-365` makes the equity computation a
  point-source template, including drawdown/run-up, CAGR, monthly and per-bar
  Sharpe/Sortino, and the observation-count denominator. The existing array
  overload delegates through `ArrayCurve` at `:374-401`; the ArrayCurve
  instantiation is emitted at `:367-372`.
- `tests/test_metrics_point_source.cpp:1-139` covers exact source reads,
  empty and month-boundary curves, zero/negative equity, a zoned walk, and the
  selected-window observation denominator against
  `tests/equity_stats_reference.hpp`.
- `tests/CMakeLists.txt:7091-7094` registers the new unit after the P00
  line-pinned block. The existing `src/engine_metrics.cpp` entry is already
  wired in `CMakeLists.txt:121`.
- The four documentation citations that moved with `engine_metrics.cpp` were
  refreshed in the same implementation commit.

## Red test and fail-before reason

The red witness was added first in `bd0191de8`. On the base tree,
`include/pineforge/metrics_stream.hpp` contains only declarations: there are no
`ArrayCurve` constructor/size/index definitions and no
`compute_equity_stats<ArrayCurve>` instantiation. Linking the witness therefore
fails on the missing L7B symbols; the expected metric comparisons are unchanged.
The witness comment records the base SHA and this reason at
`tests/test_metrics_point_source.cpp:6-9`.

## Verification

Per the lane brief, no build, compile, test, corpus sweep, preflight, or CI
command was run on this machine. The only local check was `git diff --check`,
which passed. No verification logs were generated; CORE-TOP must run the
required lab proof and parity checks.

## Scope and open items

- No mechanical helper or fixture adaptation was needed.
- No frozen file was edited. `OPEN FROZEN`: none.
- L7A's trade accumulator, L6b-4/L7C's `SpillCurve`, and sink integration
  remain outside this lane as specified by the parallel plan.
- No deviations or open implementation questions remain for this slice.
