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
request, driver and current-execution pairings.

All three real historical providers remain required in the full matrix; the
selected-method rejection uses R2.

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
  --receipt build/settlement-abi-receipt.json
```

`--v13-receipt` is mandatory in the full matrix; only the partial development
modes below may omit it. Pass `--extra-flag=-fsanitize=address,undefined` for
an instrumented archive, as the existing CMake ABI guards do. The compiler must
match the current build and all three prepared providers. A preserved Mac Release archive cannot stand
in for a Linux, Debug or sanitizer provider. Missing artifacts fail with the preparation
command; no skip, stub or implicit network fallback is available.

The checker:

* authenticates all three historical header inventories and all four archives, checks old defined symbols,
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
* compares every frozen native header's text. Across the reviewed
  `engine_script_run_v13` → `engine_script_run_v14` transition — and only that
  transition, enumerated in one module constant in the checker — exactly four
  headers may differ: `native_order.hpp`, `native_host.hpp`,
  `market_driver.hpp` and `execution_consumer.hpp`, which advance with
  `native_order_v3`, host v14, `native_driver_v4` and consumer v5. Each actual
  difference is recorded in `frozenShape.exemptedHeaders` with both digests and
  its transition; an exempted header that did not change records nothing, and
  any other differing header raises. `native_order_identity.hpp`,
  `native_run_spec.hpp` and `native_calendar.hpp` stay frozen throughout. A
  future v14→v15 transition must be added to that constant explicitly, and a
  frozen v14 provider added after merge restores the header-text fence for all
  seven;
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
* links epoch-13 constructor/vtable, host observation, return-only
  `native_events()`, core request, driver and current-execution callers against
  the real c3ed455 archive, the matching v14 callers against the current
  archive, and rejects each across the two epochs at its exact namespaced
  symbol;
* records in each link the provider engine owner derived from that archive's
  own defined symbols (`providerEngine`), never from which command-line role
  named the path; exact-owner RTTI is tolerated only for a sanitized
  cross-epoch rejection;
* refuses unrelated undefined symbols, retains full compiler/link logs and
  records `executedBinaries: 0`.

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
or the c3ed455 pairings, and may omit `--prior-receipt` and `--v13-receipt`.
None returns `status: passed` or
`newArchivePairingComplete: true`. Do not use these modes in CI acceptance.

The offline Python refusal tests are run by
`python3 scripts/test_settlement_cpp_abi.py -v`; they are tooling checks, not
substitutes for the actual compiler/archive matrix.
