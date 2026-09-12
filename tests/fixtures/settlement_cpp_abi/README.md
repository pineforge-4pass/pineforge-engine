# Settlement C++ ABI proof

`e60e571/manifest.json` pins the complete 52-file public header closure of
R2 commit `e60e57156a54bb1c74c0c0a4f5a338fd924c046d`, tree
`e209c89035ec884774f8f3e68e1dc118f26aaf51`. Each entry records bytes, SHA-256
and Git blob identity. Binary libraries are **not** checked into this fixture.

`0e18690/manifest.json` similarly pins the complete 54-file header closure of
R3 commit `0e18690db3fb6bb4705841c2a86936dfa206380a`, tree
`c29545d262bf6dcc22e46aa3382422f2e9c68378`. This provider has selected settlement
and account projection, and predates the four `ReverseTo` methods. Both real
historical providers remain required; the selected-method rejection uses R2.

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
  --receipt build/settlement-abi-receipt.json
```

Pass `--extra-flag=-fsanitize=address,undefined` for an instrumented archive,
as the existing CMake ABI guards do. The compiler must match the current
build and both prepared providers. A preserved Mac Release archive cannot stand
in for a Linux, Debug or sanitizer provider. Missing artifacts fail with the preparation
command; no skip, stub or implicit network fallback is available.

The checker:

* authenticates both historical header inventories and all three archives, checks old defined symbols,
  and requires current archive freshness against all `src`, `include`,
  `cmake` files and `CMakeLists.txt`;
* freezes execution aggregate field/order/type/status encodings, Action and CloseScope
  alternatives, native header contracts, engine storage declarations and
  virtual methods; compares actual compiler-emitted offsets/alignment/type
  sizes for 251 engine data members plus returned structures; separately freezes
  selected/projection headers and compares their layouts against 0e18690;
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
and may omit `--prior-receipt`. None returns `status: passed` or
`newArchivePairingComplete: true`. Do not use these modes in CI acceptance.

The offline Python refusal tests are run by
`python3 scripts/test_settlement_cpp_abi.py -v`; they are tooling checks, not
substitutes for the actual compiler/archive matrix.
