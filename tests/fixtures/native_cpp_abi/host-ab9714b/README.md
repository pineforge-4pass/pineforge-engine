# Frozen epoch 16 native provider headers

Exact 61-header closure from `ab9714beccb62b796c122cf68986ec9e7dbf4a67`,
tree `8c75db9858e63e019a31dd90230eff7f16ce24eb`. Every file is authenticated
by size, SHA-256 and Git blob in `manifest.json`. `headers.tar` SHA-256:
`1a1ab85239ce1bca9022f879ecc0e88c2ee0af719c74cdfe9d8e9d5aaada8d98`.

This is the real immutable `engine_script_run_v16` / `native_order_v4` /
`native_run_spec_v1` / `native_driver_v4` / native-consumer-v6 provider at the
R4-D adapter-lowering base. The preparation tool builds the archived source
with the current profile compiler/configuration; it never synthesizes a
provider or executes ABI callers.

The provider is intentionally same-epoch with the live v16 archive. The ABI
matrices require both same-epoch link directions to succeed, while retaining
the existing v13/v14/v15 rejection controls. It is frozen before L1 advances
any version literal, so a later v17/v5/v2/v5/v7 transition has an authentic
v16 side to reject.
