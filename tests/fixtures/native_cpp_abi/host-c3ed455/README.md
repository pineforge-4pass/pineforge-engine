# Frozen epoch 13 native provider headers

Exact 55-header closure from c3ed45516721d3185fcd2f50bb293793304bc6e6,
tree bb80c4767dddc0e5c9ae172672edd955ad344890. Every file is authenticated
by size, SHA-256 and Git blob in manifest.json. headers.tar SHA-256:
8dce8eab8e189dad5caabdb4c4b8db29cd3c68af80d10097e46650f51076085b.

The preparation tool builds the real immutable source with the current
profile's compiler/configuration; it never synthesizes a provider or executes
ABI callers. Historical e60/0e providers remain required. Host/order/driver
cross-epoch links reject explicitly; old-old and new-new controls link.
The original root-frozen Release provider remains separate and untouched.
