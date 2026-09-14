# Frozen epoch 15 native provider headers

Exact 56-header closure from e7cdf052fa44d4c98035804db7b8399d3a5a37b2,
tree dea028ca5664f78c055b1588820a4f7cce5b137f. Every file is authenticated
by size, SHA-256 and Git blob in manifest.json. headers.tar SHA-256:
189a0e99ff60f7c9284243117fe501ebf9a9fb6269c787dad35957d0ca7a6ed3.

This is the frozen engine_script_run_v15 / native_order_v4 / native_driver_v4 /
consumer v6 provider at the R4-C source-layer base. The preparation tool builds
the real immutable source with the current profile's compiler/configuration; it
never synthesizes a provider or executes ABI callers. The e60, 0e, c3ed455 and
f736676 providers remain required. This fixture is the frozen v15 provider;
the live v16 archive rejects it and its v15 callers reject the live archive at
the declared engine/source-domain symbols.
