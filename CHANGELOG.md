# Changelog

Notable changes to pineforge-engine. From 1.0.0 the version number follows
semantic versioning over the surfaces the
[public contract](docs/pages/public-contract.md) lists. Releases before 1.0.0
are summarized in the README's *Releases* section and on the GitHub releases
page.

## 1.0.0

Release candidates are tagged `v1.0.0-rc.N`. This entry covers every change
since v0.13.1, the last release: `VERSION` read 0.14.0 from 439bd520 on, and
0.14.0 was never tagged. The engine and pineforge-codegen ship 1.0.0
together, and from 1.0.0 the only supported pair is the same version,
prerelease included.

### What 1.0 means

- **The kernel runs without any Pine adapter.** A C++ host
  (`NativeStrategyHost`, `<pineforge/native_host.hpp>` and the native headers)
  or a C host (`<pineforge/native_c_api.h>`) drives the execution kernel that
  generated PineScript strategies run on: batch and stream runs, the native
  request vocabulary, margin and liquidation, risk limits, FX conversion, the
  per-bar broker-state hash and the report. `-DPINEFORGE_BUILD_SOURCE_LAYER=OFF`
  builds that kernel alone, `libpineforge_kernel.a` (`PineForge::kernel`,
  #257), and `examples/native` holds 18 Pine-free hosts (16 C++, 2 C) that run
  as CTest rows.
- **Pine is an adapter over that kernel.** The Pine source layer was cut out of
  `BacktestEngine` (#253), and generated Pine execution was lowered onto the
  native kernel, retiring the legacy loop (#254). Its host
  (`PineStrategyHost`) and headers live under `include/pineforge/source/`.
- **The version number now promises something:** the C ABI, the native C++
  API, the script ABI epoch and its state-hash values, the 1.0 C-surface
  boundary and the pairing with codegen, as the
  [public contract](docs/pages/public-contract.md) states.

### Breaking changes: what a 0.x user must act on

- **C ABI version 4.** `PF_ABI_VERSION` is 4 (0.13.1: 3). `pf_report_t`
  appends `broker_state_hash` and `broker_state_hash_len`, and it is
  caller-allocated, so a mirror built for ABI 3 under-sizes it: update FFI
  mirrors and check `pf_abi_version() == PF_ABI_VERSION` before a run
  (439bd520, d9b80809). No `PF_API` function was removed or renamed; `pineforge.h`
  declares 34 more and `native_c_api.h` adds the native host API.
- **Two C fields renamed.** `pf_equity_stats_t::sharpe_tv` / `sortino_tv` are
  `sharpe_monthly` / `sortino_monthly`, at the same offsets (48 and 56); the
  old spellings, kept as deprecated aliases during development, are removed
  for 1.0. Rename them in C code and in ctypes or Rust mirrors. The JSON
  report keys stay `sharpe_tv` / `sortino_tv`. Development builds' C++
  `exit_legs::Domain::Coof` / `MagnifierCoof` are likewise gone; the
  enumerators are `FillRecalc` / `MagnifierFillRecalc`, with the same values.
- **Regenerate and relink every generated strategy.** The C++ a generated
  strategy compiles against now carries the script ABI epoch
  `engine_script_run_v19` (an inline namespace 0.13.1 did not have), so an
  object built against 0.x headers does not link. Transpile with the
  pineforge-codegen release of the same version (engine 1.0.0 with codegen
  1.0.0; `v1.0.0-rc.N` with codegen `1.0.0-rc.N`) and rebuild. `PF_ABI_VERSION`
  equality alone does not make a pair.
- **State hashes are new, and v19's.** The broker-state hash, the per-bar hash
  rows, the stream fingerprint and the native continuation hash did not exist
  in 0.13.1; their values are the v19 epoch's (#282, #283, #284, #286), and a
  value recorded by a development build before 1.0.0 is not comparable. From
  1.0.0 their recipe is fixed for all of 1.x.
- **Event retention defaults to a window.** `NativeRunSpec::event_retention`
  defaults to `NativeEventRetention::Window`, which keeps the command journal
  only until every reader has consumed it (#283). A C++ host that reads the
  whole run's events after the run sets `Full` (or `Commands`). A C host that
  does not send the retention word keeps `PF_NATIVE_EVENT_RETENTION_FULL`.
- **`-ffp-contract=off` reaches consumers.** The CMake package hands it to
  every translation unit that links `PineForge::pineforge` or
  `PineForge::kernel` (#282); 0.13.1 compiled the library alone with it. A
  build outside CMake compiles generated strategy code with
  `-ffp-contract=off` itself (docs/pages/integration-cmake.md).
- **One header moved.** `<pineforge/pine_float_compare.hpp>` is now
  `<pineforge/source/pine_float_compare.hpp>`, a source-layer header
  (e71e589c); new code includes `<pineforge/ta_compare_band.hpp>`. A
  kernel-only build installs neither `include/pineforge/source/` nor
  `include/pineforge/compat/`.
- **CMake pins move to 1.** The package's version file is `SameMajorVersion`,
  so `find_package(PineForge 0.N ...)` does not find a 1.x install: pin
  `find_package(PineForge 1.0 REQUIRED)` (docs/pages/integration-cmake.md).
- **Versions may carry a prerelease.** `VERSION` is `X.Y.Z` or `X.Y.Z-rc.N`.
  `PINEFORGE_VERSION_STRING`, `PineForge_VERSION` and `pf_version_get()` stay
  numeric; the full version is `PINEFORGE_VERSION_FULL`, the package's new
  `PineForge_VERSION_FULL` and `pf_version_string()`, so
  `find_package(PineForge 1.0.0 EXACT)` also accepts `1.0.0-rc.N`. The CMake
  smoke consumer (`cmake/smoke_consumer`) prints `pf_version_string()`.

### Behaviour that moves results against 0.13.1

These change a strategy's trades or report values; each commit states the
TradingView behaviour it matches.

- `str.tostring` / `str.format` render numbers by the rule TradingView's tapes
  pin (3872c46a, #287).
- Exact quantities: a Transact that crosses the book is charged exactly its
  units, a close that spans lots and ends inside one exactly its request, and
  a close whose binary64 sum reaches its request exactly at a lot closes that
  lot whole (deb344b1, 99cf9252, 1e9e0761; #287).
- Indicators: `ta::ALMA` takes the floor input, `ta::KC` / `ta::KCW` take
  use_true_range, KC's middle band is its source's EMA on every bar,
  `ta::AnchoredVWAP` restarts on a caller-supplied anchor, and
  `ta::PivotPointLevels` computes an anchored period's pivot levels
  (f2ae16c7, de612473, 24c173ed, be41c1bb, 1eef78eb; #281); the six-argument
  `ta::pivot_point_levels` overload computes every type (adb8f2b6, #287).
- Sessions: `session.isfirstbar` / `session.islastbar` are the kernel's
  session-day facts, a session ends at the session day, and an aggregated
  chart and a stream's realtime bar read `session.islastbar` as TradingView
  does (c68fab7e, b531a8d9, 8b843499, be19463d).
- A margin call never revives an exit the script cancelled (4b00da92, #287).
