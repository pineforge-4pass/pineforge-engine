# Frozen native C API fixture — v1

`native_c_api.h` is a byte-for-byte copy of
`include/pineforge/native_c_api.h` as the L13 lane published it
(`PF_NATIVE_API_VERSION == 1`). `manifest.json` records its SHA-256, Git blob
id and byte length.

`tests/test_native_c_api_frozen_header.cpp` compiles a C fixture against this
copy instead of the live header — the copy defines `PINEFORGE_NATIVE_C_API_H`
before including `<pineforge/pineforge.h>`, so the current header is skipped
and the whole translation unit sees the frozen declarations — and then links
it against the current runtime.

That makes the pairing executable rather than aspirational. Today the two
files are identical and the test proves the round trip. The moment a field is
appended or a struct is reordered in the live header, the frozen caller keeps
sending the old `struct_size`, the current runtime answers
`PF_NATIVE_E_STRUCT`, and the test fails — which is the refusal the header's
hardening rules promise. Never regenerate this copy to make that failure go
away: add `v2/` beside it and keep v1 as the old provider.
