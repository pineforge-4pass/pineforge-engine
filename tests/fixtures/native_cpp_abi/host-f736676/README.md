# Frozen epoch 14 native provider headers

Exact 55-header closure from f736676ea9a558dc664b18f099a488b3a2c0067f,
tree c69421f0f86d23aa48eeb2c79bf7f475a4db0e83. Every file is authenticated
by size, SHA-256 and Git blob in manifest.json. headers.tar SHA-256:
37e9340e0a985db118006e7e3b265e0191445285ce5e8fd8fc77f1578275e28e.

This is the last engine_script_run_v14 / native_order_v3 / native_driver_v4 /
consumer v5 commit (R4-A, PR #250). The preparation tool builds the real
immutable source with the current profile's compiler/configuration; it never
synthesizes a provider or executes ABI callers. The e60, 0e and c3ed455
providers remain required. Epoch-15 callers must reject against this archive
at the declared symbols; v14-v14 and current-current controls link.
