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

That makes the pairing executable rather than aspirational. The two files are
no longer identical: the live header has since grown deliberately additive
tails — on `pf_native_request_v1`, `pf_native_run_spec_ext_v1`,
`pf_native_callbacks_v1` and the `pf_native_working_v1` readout — still at
`PF_NATIVE_API_VERSION == 1`. Each such tail publishes the earlier layout's
length as a `*_SIZE` constant that the runtime keeps accepting (and a readout
is written only as far as that length), so the frozen caller, which keeps
sending the v1 `struct_size` it was compiled with, keeps working — and the
test proves exactly that, row by row. A change that is not such a tail — a
field inserted or reordered, or a published length the runtime stops
accepting — makes the current runtime answer the frozen caller
`PF_NATIVE_E_STRUCT`, and the test fails: the refusal the header's hardening
rules promise. Never regenerate this copy to make that failure go away: add
`v2/` beside it and keep v1 as the old provider.
