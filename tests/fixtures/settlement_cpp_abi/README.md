# Settlement C++ ABI proof

`e60e571/manifest.json` pins the complete 52-file public header closure of
R2 commit `e60e57156a54bb1c74c0c0a4f5a338fd924c046d`, tree
`e209c89035ec884774f8f3e68e1dc118f26aaf51`. Each entry records bytes, SHA-256
and Git blob identity. Binary libraries are **not** checked into this fixture.

`0e18690/manifest.json` similarly pins the complete 54-file header closure of
R3 commit `0e18690db3fb6bb4705841c2a86936dfa206380a`, tree
`c29545d262bf6dcc22e46aa3382422f2e9c68378`. This provider has selected settlement
and account projection, and predates the four `ReverseTo` methods.

The third provider is the native host epoch-13 commit
`c3ed45516721d3185fcd2f50bb293793304bc6e6`, tree
`bb80c4767dddc0e5c9ae172672edd955ad344890`, whose header closure is pinned by
`../native_cpp_abi/host-c3ed455/manifest.json` (shared with the native ABI
checker) and prepared under `native-abi-v13/`. It supplies the epoch-13 side of
the constructor/vtable, host observation, return-only `native_events()`, core
request and driver pairings. It has no current-execution declarations.

The fourth provider is native host epoch 14 at commit
`f736676ea9a558dc664b18f099a488b3a2c0067f`, tree
`c69421f0f86d23aa48eeb2c79bf7f475a4db0e83`. Its 55-header closure is frozen at
`../native_cpp_abi/host-f736676/manifest.json` with `headers.tar` SHA-256
`37e9340e0a985db118006e7e3b265e0191445285ce5e8fd8fc77f1578275e28e`, and its
full archive is prepared under `native-abi-v14/`.

The fifth prepared provider is the frozen same-epoch v15 source-layer base at
commit `e7cdf052fa44d4c98035804db7b8399d3a5a37b2`, tree
`dea028ca5664f78c055b1588820a4f7cce5b137f`. Its 56-header closure is frozen
at `../native_cpp_abi/host-e7cdf05/manifest.json` with `headers.tar` SHA-256
`189a0e99ff60f7c9284243117fe501ebf9a9fb6269c787dad35957d0ca7a6ed3`, and its
real archive is prepared under `native-abi-v15-frozen/`.

All four historical providers, the frozen v15 provider, and the live v16
archive remain required in the six-archive matrix. e60/0e retain engine-only
roles; v13/v14/frozen-v15/live-v16 supply the host/order/driver domains. The
v15↔v16 source/host rejection pairs are mandatory; the selected-method
rejection uses R2.

The new settlement checker supplements `check_native_cpp_abi.py` and
`check_script_cpp_abi.py`; retain those existing checks and their old epoch
and UBSan RTTI controls. No generated pairing executable is run.

## Prepare the real historical providers explicitly

The shared `ci_verify.py` profiles use `PINEFORGE_VERSION_SOURCE=FILE`, so
verification version identity is independent of tags or checkout depth. They
fetch each pinned old commit without tags only when it is missing and reuse
matching prepared archives on subsequent runs. For manual preparation, configure
the current build first, then fetch each old object if needed, outside CTest:

```sh
git fetch --no-tags --depth=1 origin e60e57156a54bb1c74c0c0a4f5a338fd924c046d
python3 scripts/prepare_settlement_cpp_abi_base.py \
  --source-repo . --current-build build \
  --output build/settlement-abi-base --jobs 4
git fetch --no-tags --depth=1 origin 0e18690db3fb6bb4705841c2a86936dfa206380a
python3 scripts/prepare_settlement_cpp_abi_base.py \
  --source-repo . --current-build build \
  --output build/settlement-abi-prior --jobs 4 \
  --commit 0e18690db3fb6bb4705841c2a86936dfa206380a \
  --tree c29545d262bf6dcc22e46aa3382422f2e9c68378 \
  --header-manifest tests/fixtures/settlement_cpp_abi/0e18690/manifest.json
git fetch --no-tags --depth=1 origin c3ed45516721d3185fcd2f50bb293793304bc6e6
python3 scripts/prepare_settlement_cpp_abi_base.py \
  --source-repo . --current-build build \
  --output build/native-abi-v13 --jobs 4 \
  --commit c3ed45516721d3185fcd2f50bb293793304bc6e6 \
  --tree bb80c4767dddc0e5c9ae172672edd955ad344890 \
  --header-manifest tests/fixtures/native_cpp_abi/host-c3ed455/manifest.json
git fetch --no-tags --depth=1 origin f736676ea9a558dc664b18f099a488b3a2c0067f
python3 scripts/prepare_settlement_cpp_abi_base.py \
  --source-repo . --current-build build \
  --output build/native-abi-v14 --jobs 4 \
  --commit f736676ea9a558dc664b18f099a488b3a2c0067f \
  --tree c69421f0f86d23aa48eeb2c79bf7f475a4db0e83 \
  --header-manifest tests/fixtures/native_cpp_abi/host-f736676/manifest.json
python3 scripts/prepare_settlement_cpp_abi_base.py \
  --source-repo . --current-build build \
  --output build/native-abi-v15-frozen --jobs 4 \
  --commit e7cdf052fa44d4c98035804db7b8399d3a5a37b2 \
  --tree dea028ca5664f78c055b1588820a4f7cce5b137f \
  --header-manifest tests/fixtures/native_cpp_abi/host-e7cdf05/manifest.json
```

For the sanitizer lane, replace the build paths with `build-asan`; for the
native-runner lane use `build-live`. The preparation script copies the actual
current CMake compiler/build-type/flags/sanitizer settings, Eigen package or
already populated FetchContent source, and platform options. It builds each full pinned static library
with tests, tutorial, runner and corpus disabled. Source comes only from
immutable local Git objects. Dependencies must already be available in the current build; its downloaded
Eigen source is reused when needed. FetchContent stays disconnected. The script itself performs no Git/dependency
network fetch and runs no test or strategy binary.

Every preparation output must be new. Keep an existing receipt only for the
same compiler/configuration; the checker rejects mismatches. The receipt
binds archive/header bytes, actual compiler implementation/version/target,
configuration, compilation database and source identity. The version header
is generated from the archived base's VERSION fallback; it is retained and
hashed, not mislabeled as a live Git checkout descriptor.

## Check the integrated current archive

```sh
python3 scripts/check_settlement_cpp_abi.py \
  --compiler /path/to/current/c++ --library build/lib/libpineforge.a \
  --include include --generated-include build/include \
  --base-receipt build/settlement-abi-base/receipt.json \
  --prior-receipt build/settlement-abi-prior/receipt.json \
  --v13-receipt build/native-abi-v13/receipt.json \
  --v14-receipt build/native-abi-v14/receipt.json \
  --v15-frozen-receipt build/native-abi-v15-frozen/receipt.json \
  --receipt build/settlement-abi-receipt.json
```

`--v13-receipt`, `--v14-receipt`, and `--v15-frozen-receipt` are mandatory in the full matrix; only the
partial development modes below may omit them. CMake supplies these through
`PINEFORGE_NATIVE_ABI_V13_RECEIPT`, `PINEFORGE_NATIVE_ABI_V14_RECEIPT`, and
`PINEFORGE_NATIVE_ABI_V15_FROZEN_RECEIPT`.
Pass `--extra-flag=-fsanitize=address,undefined` for
an instrumented archive, as the existing CMake ABI guards do. The compiler must
match the current build and all five prepared providers. A preserved Mac Release archive cannot stand
in for a Linux, Debug or sanitizer provider. Missing artifacts fail with the preparation
command; no skip, stub or implicit network fallback is available.

The checker:

* authenticates all four historical header inventories, the frozen v15 closure, and all six archives, checks old defined symbols,
  and requires current archive freshness against all `src`, `include`,
  `cmake` files and `CMakeLists.txt`;
* freezes execution aggregate field/order/type/status encodings, Action and CloseScope
  alternatives, native header contracts, engine storage declarations and
  virtual methods; separately freezes selected/projection headers and compares
  their layouts against 0e18690;
* compares, unconditionally and in full, each historical provider's engine
  storage inventory (252 declarations, 251 of them non-static data members) and
  virtual method inventory, plus every compiler-emitted layout word — all 789
  against e60e571 and all 793 against 0e18690, which is the offset, size and
  alignment of each of the 251 engine data members alongside the returned
  structures, `sizeof`/`alignof` of the engine and the native and
  selected/projection aggregates. An epoch transition exempts no inventory
  entry and no layout word; `layout.comparedWords` and
  `priorLayout.comparedWords` always equal their `wordCount`;
* compares every frozen native header's text. The reviewed
  `engine_script_run_v13` → `engine_script_run_v15`,
  `engine_script_run_v14` → `engine_script_run_v15`, and
  `engine_script_run_v15` → `engine_script_run_v16` transitions use their
  exact checker inventories. The v15→v16 comparison consumes the authenticated
  source-layer relocation manifest: it permits only the listed moved storage
  and seams, measures `source::PendingOrder` separately, and fails on every
  extra difference. Current epochs are `native_order_v4`, host v16, unchanged
  `native_driver_v4` and consumer v6. Each actual difference is recorded in
  `frozenShape.exemptedHeaders` with both digests and its transition; an
  exempted header that did not change records nothing, and any other differing
  header raises. `native_order_identity.hpp`, `native_run_spec.hpp` and
  `native_calendar.hpp` stay comment-stripped frozen throughout. The reviewed
  current headers have matching byte pins.
* compiles each old/new caller before any link result is interpreted;
* links old Book/singleton/lifecycle, old private F8/F11 wrappers and old
  return-only host event callers against both full providers;
* links the six new selected/projection methods and new private provenance
  helpers against the current provider, then rejects those same objects
  against the real R2 provider for every required method;
* links selected/projection callers built with 0e18690 headers against both
  0e18690 and current archives, plus current selected callers against 0e18690;
* links one caller for all four reversal methods against the current archive,
  then rejects that same object against real 0e18690 with each exact method
  name and `reverse_to_v1::ReverseTo` parameter namespace;
* compiles wrong projection `_v2`/return-namespace and wrong selection-domain
  callers, then requires rejection at those exact symbols. Return namespace
  is not demanded in ordinary projection method mangling;
* rejects synthetic reversal `_v2` methods and `reverse_to_v2::ReverseTo`
  parameters against current, supplementing the real historical rejection;
* links constructor/vtable, host observation, return-only `native_events()`
  and core request callers to their matching v13/v14/v15/v16 archives, then
  rejects every cross-epoch host/order pair at its exact namespaced symbol;
* compiles shape-agnostic current-execution callers from authenticated v14/v15
  headers and the live v16 headers, links each to its matching archive, and
  rejects every mismatched owner epoch;
* links driver callers to their matching archives, rejects v13↔v14 and
  v13↔v15 pairs, and requires positive v14→v15 and v15→v14 links because
  both own `native_driver_v4`. These per-domain outcomes govern over the
  frozen v14 fixture README's historical blanket-rejection wording;
* records in each link the provider engine owner derived from that archive's
  own defined symbols (`providerEngine`), never from which command-line role
  named the path, and verifies that owner against its authenticated headers and
  pinned provider role. Mixed, missing or mislabeled epochs are errors.
  Exact-owner RTTI is tolerated only for a sanitized
  cross-epoch rejection;
* refuses unrelated undefined symbols, retains full compiler/link logs and
  records `executedBinaries: 0`.

The full phase-0 receipt retains `CURRENT_EXECUTION_V15_CALLER` and
`NATIVE_FX_CURVE_CALLER` as pending-surface rows while
`CURRENT_TERMS_SURFACE_READY = False`. Their complete templates require the
phase-1c host members and are not compiled before those members exist. Once
active, both link to v15 and reject v14/v13 at the configure/FX-value symbols.
CTest also writes `native-abi-receipt.json`, with the active
`v14_current_execution_shape_agnostic_compile` using the frozen authenticated
tar closure. Its named good-v15 and missing-Cancelled negative compilation
controls are staged pending until phase 1c; ordinary compiler failures remain
hard failures and do not count as link rejections.

Private/protected member access uses explicit test-only `-fno-access-control`
on the pairing TUs. Product visibility is unchanged. Callers reinterpret an
unused command-line pointer and are link-only; **never execute them**.
The compiler's non-standard-layout `offsetof` extension is used only for
same-toolchain ABI observation, with its specific diagnostic suppressed.

Three development modes return labeled partial receipts:
`--base-only` checks old-provider controls; `--old-rejections-only` checks
new public declarations against the real old provider before the new archive
exists; `--public-only` runs all public pairings before new private provenance
helpers are integrated. These partial modes do not include the reversal matrix
or the v13/v14/frozen-v15/live-v15 domain pairings, and may omit `--prior-receipt`,
`--v13-receipt`, `--v14-receipt`, and `--v15-frozen-receipt`.
None returns `status: passed` or
`newArchivePairingComplete: true`. Do not use these modes in CI acceptance.

The offline Python refusal tests are run by
`python3 scripts/test_settlement_cpp_abi.py -v`; they are tooling checks, not
substitutes for the actual compiler/archive matrix.
