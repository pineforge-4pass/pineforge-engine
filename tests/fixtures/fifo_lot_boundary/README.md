# FIFO lot-boundary TradingView evidence

These are standalone synthetic strategies, exported with `lab tv --no-note`
on 2026-10-03 using `ws-report-v1`. Each directory contains the exact compiled
`strategy.pine`, TradingView's `tv_trades.csv`, export `meta.json`, and raw-report
provenance in `metrics.json`. Chart, window, source digest and tape digest are
recorded per case; all exports have a covered range. Tapes contain one entry and
one exit row per closed trade. No population source was submitted.

`test_adapter_fifo_lot_boundary.cpp` pins quantities, FIFO ordering, distinct
exit IDs and entry incarnations from these tapes. Its bars are synthetic, so
prices and timestamps in that unit test are not claims about these chart feeds.
The add-on fixture has two cycles; the unit test pins its first two-trade cycle.
The fifty-close fixture deliberately uses fifty static close statements, not
one dynamic-ID loop callsite (which TradingView replaces).

Both adapter boundary paths use `internal::kQtyEpsilon` (`1e-10`). POOC binds
an ambiguous interior prefix as a selected `Flatten`; ordinary deferred FIFO
closes use `HostSized` to resolve the exact physical prefix sum for a `Reduce`.
Non-ambiguous and ANY closes keep their prior request shape. The unrelated-suffix
case proves a suffix alone is not a reachable FIFO boundary. The unit-only
near-above/near-below cases exercise misses of `5e-11`; an outside-bound `5e-9`
partial and genuine `1e-8` closes remain distinct, rather than being swallowed.
Cancelled/replaced predecessors and a partially drained first lot are adapter
request-shape controls, not additional TradingView export claims.

Same-bar market transactions, `immediately` closes and the ShortSeed placeholder
are outside the ordinary normalization. No kernel quantity tolerance is declared.

| Slug | Closed trades | Pine SHA-256 | Tape SHA-256 |
| --- | ---: | --- | --- |
| `addon-lot-identities` | 4 | `b0edd4dc98199fdf6607ff56ba32526eaae8ff7b9939288aceb88ad1654326fe` | `ca04884f0c50cfd786287e576e7bbb3dcf2ece1ec90c1d4d610b66b663e8399c` |
| `any-named-boundary` | 4 | `e8066b434cb5713c1fbfcedf40e7bc1f6c6fcfbe3a19d2e2fc7623f62bddb1bd` | `4ed52e8fd4f08c8128dc198cff436e1a7e70fa564462c203decf16bcb435c87f` |
| `cash-sizing-floor` | 1 | `6de0757689fe97470a0acfd1300bbce043411ca366edab0e492257c27e07e379` | `4f46ff7c192d191d38c871a009fb7d7625bdafe393d826eef305456e5dd7e3dd` |
| `chained-fifo-boundary` | 5 | `873b40007c5205cab0cedc2fb4a9b97ed928b0e6e32dd72294220bcd15f98ab7` | `b56e0675d078046ed195ed7482d1125648363de31f21f08abb8ccf28359f1c89` |
| `fifo-named-boundary` | 4 | `439249be66e69c1115a9aa136a7dffedbacc6d1f1e02735a86fb9faaa2c1f2d0` | `d2c641650269f5dca1455473ca41e523811d94dd7c343bbf529bb56f0087760c` |
| `fifty-static-closes` | 102 | `f53e880a97a24a58be4a661e16e3ae044ad14bf81e949121d9b08cbf35ec6758` | `21fe943546b5f8e9df10b621da16b3b95c9eff305f4343b2dd220864d8c11e37` |
| `fractional-cost-chain` | 7 | `1e51497047624a08c7614cde6acd4f851cc4223a7c26a0a502d230c60e77fd1f` | `e7c2bfb094916e9b2002795370bbd1a4765b2d24130ee5e6e9f0dd259fecd639` |
| `gap-sibling-exits` | 2 | `a40492bed97d0db7f8f890c32b568945ee7e54541a2182d4a45404a8689be729` | `1b1408c6759615be94446621c2ced4f439037bccf01332f307a6fc3657f7997f` |
| `percent-sizing-floor` | 1 | `6e606b1390c84ceec9c5f1f3577364dae53580bcefc598b7f31825e28db9913c` | `24561b06b0a55f15eece1b43449dab96149f80d136959b477c7f08ec0f2b0011` |
| `separate-exit-identities` | 2 | `0da19915ed5f054db097a96a707bb53bb408b590179c664057ecc9ed41d4c098` | `d8702847a1b5e4109b0080c8ff9e45107119a19d762399558113486a2ea28d99` |
| `short-fifo-boundary` | 4 | `6a4446466727caf47b4c286821fe1feba50dc2376a6452547a723cd1618a3f9f` | `a739fd1b85798dfab19d8323656021f25f69acca2f045460ae29bc2bbbdc519a` |
| `unrelated-suffix` | 6 | `4a1a20611128d46b20dce160e6fbb7f7a2668e21896d42bca566465ecfd095cb` | `c002e29b6254fd2a8b7655f5d41ef7e6bf0493037e1b5896e029dd5b656de719` |
