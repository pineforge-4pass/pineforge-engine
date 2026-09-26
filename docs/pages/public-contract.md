# Public contract for 1.0 {#public_contract}

@tableofcontents

From 1.0.0 the engine's version number is a semantic version over the surfaces
this page lists, and over nothing else: a PATCH release fixes behaviour, a
MINOR release adds to a surface, and only a MAJOR release removes or changes
one. Each rule below names the checker or the CTest row that holds it on this
tree; where no checker holds part of a rule, the page says so. The codegen
states its half in its own `docs/PUBLIC_CONTRACT.md` (pineforge-codegen); the
two releases pair as the last section below says.

## Versions

- A release's version is `X.Y.Z`, or `X.Y.Z-rc.N` (N from 1) for a release
  candidate; its tag is `vX.Y.Z` or `vX.Y.Z-rc.N`. `release.yml` computes it
  with `scripts/release_version.py`, which refuses any other spelling, a
  version that does not sort above `VERSION` in semver order (`X.Y.Z-rc.N`
  below `X.Y.Z`) and a tag that exists (`scripts/test_release_version.py`, a
  `ci_preflight` stage).
- A release candidate is a GitHub prerelease, and its dispatch to the release
  hub carries `prerelease: true`: `.github/workflows/release.yml` reads both
  from the script's `prerelease` output, which `scripts/test_release_version.py`
  holds. The `workflow-lint` stage of `ci_preflight` lints the workflow's
  syntax, expressions and shell, not its behaviour; a `dry_run` dispatch
  rehearses it without publishing.
- CMake's `project(VERSION)`, the package's `PineForge_VERSION`,
  `PINEFORGE_VERSION_MAJOR` / `_MINOR` / `_PATCH`, `PINEFORGE_VERSION_STRING`
  and `pf_version_get()` carry the numeric `X.Y.Z`. The package's
  `PineForge_VERSION_FULL`, `PINEFORGE_VERSION_FULL`, `pf_version_string()`,
  the tag and the release tarball's name carry the full version, `-rc.N`
  included (@ref abi_stability, "Version macros"). `test_cmake_version_source`
  holds the resolution from `VERSION` and from a tag; the installed-package
  smoke test `scripts/ci_verify.py` runs (`cmake/smoke_consumer`) holds that
  the package, the installed header and the library name one full version,
  `VERSION` exactly; and `release.yml` verifies every staged tarball with
  `scripts/release_version.py check-install` before it packs it.

## The C ABI

The C ABI is `<pineforge/pineforge.h>` and the native host API
`<pineforge/native_c_api.h>` it includes: the `PF_API` functions, the `pf_*`
structs and enumerations, and the `PF_*` constants.

- **`PF_ABI_VERSION` is 4 for every 1.x release.** It versions the layouts a
  caller allocates or strides over (`pf_report_t`, `pf_trade_t`, the metrics
  structs), and a consumer checks `pf_abi_version() == PF_ABI_VERSION` before
  it runs a strategy. `test_c_abi` checks that equality, `test_live_run_status`
  pins the value 4, and `test_run_strategy_range_end` pins the Python
  harness's `EXPECTED_PF_ABI` to it.
- **Functions are append-only.** No `PF_API` function is removed or renamed in
  1.x; new ones are added. `scripts/check_c_abi_runtime.py` (a source guard of
  every `ci_verify` profile and of `ci_preflight`) pins the exact name sets
  `EXPECTED_RUNTIME` and `EXPECTED_NATIVE_C_API` and the declaration counts, so
  an unrecorded addition, removal or rename fails. It reads names only: a
  signature change that keeps its name is caught only where a test calls the
  function, the frozen v1 closure below among them.
- **Layouts are frozen.** A field of a published struct never moves, changes
  type or disappears in 1.x. A size-prefixed native struct (`struct_size`,
  `version`) grows only by an appended tail, and the runtime keeps accepting
  every length it published (each `*_SIZE` constant). The `static_assert`s in
  `src/c_abi.cpp` pin the size and field offsets of the structs the header
  shares with the runtime — `pf_bar_t`, `pf_trade_tick_t`, `pf_trade_t`,
  `pf_report_t`, `pf_security_diag_t`, `pf_trace_entry_t` — to their internal
  C++ twins, and `pf_equity_stats_t` / `pf_metrics_t` to literal offsets and
  sizes; `src/native_c_host.cpp` pins the published lengths of the native
  structs that grew by tails;
  `test_native_c_api_frozen_header` compiles the frozen `PF_NATIVE_API_VERSION`
  1 header closure (`tests/fixtures/native_c_api/v1`) and runs it against the
  current runtime; `test_c_abi` and `test_native_c_api_c99` check the equity
  offsets from C. These pins are against the internal twins and the frozen v1
  closure; no checker compares `pf_report_t` or `pf_trade_t` with a 1.0.0
  baseline.
- **Enumerators are append-only.** An enumerator keeps its value in 1.x; a new
  one takes a new value. `scripts/check_native_c_api_surface.py` holds every
  C enumeration of `native_c_api.h` and `pineforge.h` equal, value for value,
  to its kernel twin (one named exclusion) or declares it C-only with a
  reason, and
  `src/c_abi.cpp`'s `static_assert`s pin the `pf_magnifier_distribution_t`
  values. No checker compares a value with the previous release.

## The C++ native API

The native C++ API is the kernel's host surface: `<pineforge/native_host.hpp>`,
`<pineforge/native_run_spec.hpp>`, `<pineforge/native_fx_curve.hpp>`,
`<pineforge/native_order.hpp>`, `<pineforge/native_calendar.hpp>`,
`<pineforge/market_driver.hpp>`, `<pineforge/order_action.hpp>`,
`<pineforge/execution.hpp>`, the header-only `<pineforge/native_toolkit.hpp>`
and `<pineforge/native_module.hpp>` (@ref native_engine).

- **Within 1.x it is source-compatible:** a host that compiles against 1.0.0's
  native headers compiles against every later 1.x release. The top-level
  examples (`examples/native`, 16 C++ and 2 C hosts) build and run as the
  `example_*` CTest rows of the `release` and `kernel` profiles;
  `scripts/check_native_include_independence.py` (the
  `native-include-independence` stage of the `release`, `native` and `kernel`
  profiles) compiles every native root header and every example against the
  installed headers with the Pine source layer removed;
  `scripts/check_native_c_api_surface.py` fails when a public
  `NativeStrategyHost` member appears or disappears without its row in
  `native_c_api.h`'s COVERAGE block; `scripts/check_native_cpp_versions.py`
  pins every native type to its versioned inline namespace and the member
  order of the structs it names. Each of them holds the current tree against
  itself: no checker yet compiles a host written against 1.0.0's headers
  against a later release, so across releases review holds the promise, until
  a frozen 1.0.0 native header closure joins these rows the way
  `tests/fixtures/native_c_api/v1` does for the C API.
- **It is not binary-compatible across releases.** Rebuild every C++ object
  that includes an engine header — a native host, a generated strategy —
  against the headers and the archive of the release it links. The C++
  layouts change inside an epoch; `engine.hpp` states the rule where the
  script ABI epoch opens.

## The script ABI epoch and state hashes

- **The script ABI epoch is `engine_script_run_v19` for every 1.x release.**
  It is the inline namespace of `BacktestEngine`, `NativeStrategyHost` and the
  execution consumer, and `PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19` announces
  it. `scripts/check_native_cpp_versions.py` and
  `scripts/check_aggregate_cpp_versions.py` pin the one live epoch; the CTest
  rows `test_script_cpp_abi`, `test_settlement_cpp_abi` and
  `test_aggregate_cpp_versions_runtime` link callers built against the frozen
  v18 headers (`host-fc7aad6`) and against the live ones, and require the
  mismatched pairs to fail on the epoch-qualified
  `BacktestEngine::broker_state_hash` symbol (`scripts/ci_verify.py` requires
  their prepared receipts, so none of them skips there).
- **State-hash values are stable within the epoch.** The recipe of each state
  hash — the broker-state hash (`strategy_broker_state_hash`, the per-bar
  `pf_report_t::broker_state_hash` rows), the stream fingerprint
  (`strategy_stream_state_hash`) and the native continuation hash — belongs to
  the epoch, so from 1.0.0 every 1.x engine folds one state to one value, and
  a new recipe is a new epoch and a major release. Values from builds before
  1.0.0 are not comparable. `scripts/check_aggregate_cpp_versions.py` and
  `scripts/check_broker_state_hash_coverage.py` pin the domain tags
  `pineforge-broker-state/v19` and `pineforge-source-adapter/v4` and the
  stream fingerprint's version 19; witness rows pin values —
  `test_native_host_hash_extension`, `test_native_lean_path`,
  `test_native_match_hash_witness`, `test_native_continuation_view`,
  `test_adapter_quiet_bar`, `test_publication_witness` among them. Two
  limits: the native continuation hash folds the resolved timezone's
  resource digest, so a tzdata release that rewrites a zone moves the value of
  a run in that zone (`test_native_report_truth` states why it pins no raw
  continuation value), and no row pins a `strategy_stream_state_hash` value
  directly.

## The 1.0 C-surface boundary

The C surface is a documented subset of the native C++ API in 1.0, not its
twin. What a C host cannot reach is the table *The 1.0 C boundary* in
@ref native_engine (section *Driving the kernel from C*): every C++ capability
the 1.0 C surface does not expose, with its reason, the C route where there is
one, and the checker row that pins it -- a `[--]` row of `native_c_api.h`'s
COVERAGE block, a named `ENUM_TWINS` exclusion, or a `C_V1_EXCLUSIONS` row of
`scripts/check_native_c_api_surface.py`. The checker fails when a public
`NativeStrategyHost` member has no COVERAGE row, when a C enumeration is
neither twinned to its kernel enumeration nor declared C-only, when a
`C_V1_EXCLUSIONS` row's C++ declaration goes or its C spelling appears, and
when that table cites a `C_V1_EXCLUSIONS` row once too few or too many.
Four of its rows are planned for 1.1.0: a non-mutating execution preview, the
origin and label of an applied event, a closed-trade entry-comment accessor
and a replace-options word. Nothing in 1.0 promises C and C++ parity beyond
the fields and calls the C surface declares.

## Pairing with codegen

- **From 1.0.0 the engine and the codegen share one version number, and the
  only supported pair is the same `X.Y.Z`, prerelease included:** engine
  `v1.0.0-rc.1` with codegen `1.0.0-rc.1` (PyPI spells it `1.0.0rc1`), engine
  `v1.0.0` with codegen `1.0.0`. Any other pair is unsupported: a release
  candidate with its final release, two minors, or two builds that agree only
  on `PF_ABI_VERSION`. Generated strategy code compiles against the engine's
  internal C++ headers, which the C ABI does not cover and the script ABI
  checks above version (@ref abi_stability, "What's *not* guaranteed").
  Regenerate the C++ and relink the strategy library on every pair change.
- **The release hub enforces it.** `release.yml` dispatches `engine-release`
  to pineforge-release with `client_payload` `{version: "vX.Y.Z[-rc.N]",
  prerelease: true|false, run_id}`; codegen dispatches its own version. The
  hub builds and publishes an image only for an engine and a codegen of the
  same version, and publishes a prerelease pair under its exact version, never
  a stable or `latest` tag; that check lives in the pineforge-release
  repository, not on this tree.

## Compatibility boundary

Not covered by the version number:

- the C++ headers generated strategies compile against (`engine.hpp`,
  `ta.hpp`, the Pine source layer under `include/pineforge/source/` and
  `include/pineforge/compat/`), which the pairing rule covers instead;
- any symbol without `PF_API`, and the text of log lines;
- the Python harnesses under `scripts/`, `tutorial/` and `benchmarks/`.

The JSON report keys of `docker/run_json.py` are the release image's schema
(@ref report_schema); `scripts/test_report_schema_keys.py` pins the
`metrics.equity` keys, which kept `sharpe_tv` / `sortino_tv` when the C
fields' pre-1.0 spellings were removed. What changed at 1.0 for a 0.x user is
in the repository's `CHANGELOG.md`.
