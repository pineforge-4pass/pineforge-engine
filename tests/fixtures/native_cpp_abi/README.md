# Frozen native C++ pairing fixtures

Each subdirectory is a byte-for-byte header (and, where needed, source)
closure used by `scripts/check_native_cpp_abi.py`. `manifest.json` records
SHA-256, Git blob IDs and byte lengths, plus the archive digest for gzip fixtures.
`headers.json.gz` is UTF-8 JSON mapping include- or `src/`-relative paths to
exact file contents, gzip-compressed with `mtime=0`. The archive is
independent of Git history, shallow checkouts, network access, and later
current-header changes.

The `host-c3ed455`, `host-f736676`, `host-e7cdf05`, and `host-ab9714b` closures use `headers.tar`, authenticated
against their exact per-file manifests. They also supply the real historical
libraries prepared by `scripts/prepare_settlement_cpp_abi_base.py`; the native
checker imports that module's tar extraction/authentication helpers directly.

| Fixture | Identity | What it proves |
|---|---|---|
| `order-1acaf33` | commit `1acaf33e0340f6ae09d9529a3b6a0c12742dcb0d`, tree `ced31532e86bd41bc8753330ea0abcf379fdc05f` | Unversioned `pineforge::native_order`. `CommandEvent` has 7 alternatives. `src/native_order.cpp` is an authentic compile-only old object. |
| `calendar-draft-a8e34c` | content SHA `a8e34c127b3d8d95f5556cf2e99fbc3d93c68c85f607a0e5e3e044b904193291` | Draft unversioned calendar. `NativeInterval` is 32 bytes; `period_key` returns `int64_t`. Authentic calendar/timezone objects are compile/link controls. |
| `calendar-262a280` | commit `262a28013ed277990349e845ba0554a63f6fbc79`, tree `fb00ed9ba02cede2fe08ee8113ae32f71fd26b00`, header SHA `c47b3ee15d69587879cca8f491e73880cddae1ce3e7dfd0d1f4db16dfb8fcf4b` | Pre-wrapper unversioned `timezone_identity_descriptor`. Byte-identical to independently reviewed `f041756`. |
| `driver-08b5c88` | commit `08b5c8868eadd3022a62b32e7e2cabe0640e75f4`, tree `3482d155df5fe91c5dfd4a05fdfb506641d0ee19` | Unversioned driver values. `NativeCoordinate` is 72 bytes. `src/market_driver.cpp` is the authentic production `native_bar_structurally_valid` definition. |
| `run-spec-262a280` | same 262a280 commit/tree, `native_run_spec.hpp` SHA `b263a8bf3a68f58201c968ffa9ec8b54633fe3c752332df164ea11cf4bce3c7c` | Pre-wrapper unversioned `NativeRunSpec`. Matching old/old uses a labeled minimal symbol control, not a historical validator runtime. |
| `host-e7d023d` | commit `e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb`, tree `0201bf052429490fb453bbfd6037e5afd1669626`, `native_host.hpp` SHA `871865715f084a0054c9d8e220cb9b957318bfdc0a0e100765d1cda85d7944b3` | `engine_script_run_v12` host/event return layout. Return-only `native_events()` pairing; not a historical runtime. |
| `order-e7d023d` | same e7d023d commit/tree, `native_order.hpp` SHA `b13006e99554ba9caa3e5b444cca3e4f2b5ebd5e372d3dcbe44bb6a677192a3e` | `native_order_v1`. `CommandEvent` has 10 alternatives. `src/native_order.cpp` is an authentic compile-only old object. |
| `host-c3ed455` | commit `c3ed45516721d3185fcd2f50bb293793304bc6e6`, tree `bb80c4767dddc0e5c9ae172672edd955ad344890` | Engine/host epoch 13, order epoch 2, driver epoch 3. Full historical host/order/driver library pairing; no current-execution declarations. |
| `host-f736676` | commit `f736676ea9a558dc664b18f099a488b3a2c0067f`, tree `c69421f0f86d23aa48eeb2c79bf7f475a4db0e83`, tar SHA `37e9340e0a985db118006e7e3b265e0191445285ce5e8fd8fc77f1578275e28e` | Frozen 55-header engine/host epoch 14, order epoch 3, driver epoch 4 closure. Historical current-execution controls remain authenticated against the current v16 matrix. |
| `host-e7cdf05` | commit `e7cdf052fa44d4c98035804db7b8399d3a5a37b2`, tree `dea028ca5664f78c055b1588820a4f7cce5b137f`, tar SHA `189a0e99ff60f7c9284243117fe501ebf9a9fb6269c787dad35957d0ca7a6ed3` | Frozen 56-header v15 source-layer-base closure. It is the immutable old provider for required v15↔v16 rejection pairs. |
| `host-ab9714b` | commit `ab9714beccb62b796c122cf68986ec9e7dbf4a67`, tree `8c75db9858e63e019a31dd90230eff7f16ce24eb`, tar SHA `1a1ab85239ce1bca9022f879ecc0e88c2ee0af719c74cdfe9d8e9d5aaada8d98` | Frozen 61-header v16 adapter-lowering-base closure: engine/host v16, native order v4, run spec v1, driver v4, consumer v6. L1's authenticated v16→v17 relocation manifest makes it the rejection-pair provider for live host v17, native order v5, run spec v3, driver v5 and consumer v7. |

Sources were taken from the pairing-audit capture
`tasks/native-abi-audit/snapshot-20260912T064119Z` and, where that capture
omitted a file, from the same Git commit the capture names. The three later
tar closures come from the exact commits listed above. Do not execute
mismatched binaries. Layout sizes in each manifest were recorded by the
pairing audit's LLVM `sizeof`/`offsetof` witness and are re-checked here
with `static_assert` against the frozen headers.

The full settlement matrix comprises e60, 0e, v13, v14, frozen v15, frozen
v16 and live v17 archives. Host and order cross-epoch pairs reject, including
the mandatory v16↔v17 pairs; same-epoch v17 callers/providers link in both
directions. Driver v4 retains its historical same-owner positive controls.
`native-abi-receipt.json` records the authenticated v14, frozen-v15, and
frozen-v16 compile controls. `CURRENT_TERMS_SURFACE_READY = True`:
the complete v17 current-execution, FX and missing-Cancelled controls
are active.
Existing order-v1 rejection pairs remain required.
