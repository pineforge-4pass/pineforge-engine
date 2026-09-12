# Frozen native C++ pairing fixtures

Each subdirectory is a byte-for-byte header (and, where needed, source)
closure used by `scripts/check_native_cpp_abi.py`. `manifest.json` records
SHA-256, Git blob IDs, byte lengths, and the gzip archive digest.
`headers.json.gz` is UTF-8 JSON mapping include- or `src/`-relative paths to
exact file contents, gzip-compressed with `mtime=0`. The archive is
independent of Git history, shallow checkouts, network access, and later
current-header changes.

| Fixture | Identity | What it proves |
|---|---|---|
| `order-1acaf33` | commit `1acaf33e0340f6ae09d9529a3b6a0c12742dcb0d`, tree `ced31532e86bd41bc8753330ea0abcf379fdc05f` | Unversioned `pineforge::native_order`. `CommandEvent` has 7 alternatives. `src/native_order.cpp` is an authentic compile-only old object. |
| `calendar-draft-a8e34c` | content SHA `a8e34c127b3d8d95f5556cf2e99fbc3d93c68c85f607a0e5e3e044b904193291` | Draft unversioned calendar. `NativeInterval` is 32 bytes; `period_key` returns `int64_t`. Authentic calendar/timezone objects are compile/link controls. |
| `calendar-262a280` | commit `262a28013ed277990349e845ba0554a63f6fbc79`, tree `fb00ed9ba02cede2fe08ee8113ae32f71fd26b00`, header SHA `c47b3ee15d69587879cca8f491e73880cddae1ce3e7dfd0d1f4db16dfb8fcf4b` | Pre-wrapper unversioned `timezone_identity_descriptor`. Byte-identical to independently reviewed `f041756`. |
| `driver-08b5c88` | commit `08b5c8868eadd3022a62b32e7e2cabe0640e75f4`, tree `3482d155df5fe91c5dfd4a05fdfb506641d0ee19` | Unversioned driver values. `NativeCoordinate` is 72 bytes. `src/market_driver.cpp` is the authentic production `native_bar_structurally_valid` definition. |
| `run-spec-262a280` | same 262a280 commit/tree, `native_run_spec.hpp` SHA `b263a8bf3a68f58201c968ffa9ec8b54633fe3c752332df164ea11cf4bce3c7c` | Pre-wrapper unversioned `NativeRunSpec`. Matching old/old uses a labeled minimal symbol control, not a historical validator runtime. |
| `host-e7d023d` | commit `e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb`, tree `0201bf052429490fb453bbfd6037e5afd1669626`, `native_host.hpp` SHA `871865715f084a0054c9d8e220cb9b957318bfdc0a0e100765d1cda85d7944b3` | `engine_script_run_v12` host/event return layout. Return-only `native_events()` pairing; not a historical runtime. |
| `order-e7d023d` | same e7d023d commit/tree, `native_order.hpp` SHA `b13006e99554ba9caa3e5b444cca3e4f2b5ebd5e372d3dcbe44bb6a677192a3e` | `native_order_v1`. `CommandEvent` has 10 alternatives. `src/native_order.cpp` is an authentic compile-only old object. |

Sources were taken from the pairing-audit capture
`tasks/native-abi-audit/snapshot-20260912T064119Z` and, where that capture
omitted a file, from the same Git commit the capture names. Do not execute
mismatched binaries. Layout sizes in each manifest were recorded by the
pairing audit's LLVM `sizeof`/`offsetof` witness and are re-checked here
with `static_assert` against the frozen headers.
