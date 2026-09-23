# Frozen epoch 18 native provider headers

Exact 65-header closure from `fc7aad6219e4ed752e290abb553b1c597acec2c3`,
tree `41c5a16ce254f0a25064ddfc07e7a64f02a9bae1`. Every file is authenticated
by size, SHA-256 and Git blob in `manifest.json`. `headers.tar` SHA-256:
`91f93ac68ff072b2e1977fe748a29f2ae47901fb76e13461d90b91b66229de1b`.

This is the real immutable `engine_script_run_v18` / `native_order_v6` /
`native_run_spec_v3` / `native_driver_v5` / native-consumer-v8 provider at
the last `main` commit of epoch 18 (the base of the v19 value epoch). The
preparation tool builds the archived source with the current profile
compiler/configuration; it never synthesizes a provider or executes ABI
callers.

The provider is intentionally the authentic predecessor of the live v19
archive. The ABI matrices require v18↔v19 rejection in both directions while
retaining the historical v13/v14/v15/v16 controls. Its sibling
`relocation-manifest-v18-v19.json` pins what the epoch moved at the public
surface: the host capability macro, no host virtual added or removed, the
request-value epoch `native_order_v6` -> `native_order_v7` that V19-B moves
inside the epoch, and the storage later v19 lanes relocate inside it.
