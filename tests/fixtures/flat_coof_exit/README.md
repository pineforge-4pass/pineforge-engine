# Flat recalculation exit tapes (R5 lane PAR-ORDERS-2, item 1)

TradingView's own trades for a `strategy.exit` placed in a `calc_on_order_fills`
recalculation on a FLAT book: market entry E fills at bar j's open and its exit TX
sits inside that bar's first leg, so TX flattens the book mid-leg (cycles j = 2, 30,
41), and the recalculation places an entry and an exit for it, or an exit for an id
with no order yet. `tests/test_flat_coof_exit_tapes.cpp` replays each tape through
the Pine adapter on the 15m chart.

Each directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, NYSE:F 15, window 2025-07-01 .. 2025-07-08), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The
scripts count chart bars from 2025-07-02 09:30 ET, the first bar of
`tests/fixtures/session_islastbar/bars.inc`, whose 15m and 1m bars the rows replay.

| probe | variant | trades | tvTradesSha256 | note |
|---|---|---|---|---|
| `pa2-i1-pend-xf-pooc` | pending buy limit entry L, bracket XF 50c either side (POOC) | 6 | `b563b368e50a261d89437b2bc649f3e0b0cfc1f00f7aaa932a62cb6dcd7fc26a` | `tv-tape-pa2-i1-pend-xf-pooc-b563b368` |
| `pa2-i1-pend-xs-crossed-pooc` | pending sell limit entry S, XS stop 50c below (crossed) (POOC) | 6 | `cdb5ca043f4870e951c60e3aee246d74fc1d47bebe2cb6f2c5c7adc989df2952` | `tv-tape-pa2-i1-pend-xs-crossed-pooc-cdb5ca04` |
| `pa2-i1-xf-qty-pooc` | XF of a long id with no order, qty 100 (POOC) | 9 | `c78b18abe1bd4af3d01c40b7c1705d3d7a20505796e449b699515a590c1c5fce` | `tv-tape-pa2-i1-xf-qty-pooc-c78b18ab` |
| `pa2-i1-xf-qty-pyr2` | the same without POOC, pyramiding 2 | 9 | `d127e5c29b28963fd10614a25dcc8f9e646269b76a5e49a36b08ae22b7bd60bd` | `tv-tape-pa2-i1-xf-qty-pyr2-d127e5c2` |
| `pa2-i1-xf-dyn-pooc` | XF of a long id with no order, whole position (POOC) | 9 | `c78b18abe1bd4af3d01c40b7c1705d3d7a20505796e449b699515a590c1c5fce` | `tv-tape-pa2-i1-xf-dyn-pooc-c78b18ab` |
| `pa2-i1-xs-dyn-pooc` | XS of a short id with no order, stop 50c below (POOC) | 9 | `54720b55cf52d84d1455db4abc4ef12c119dfae53013ddfdff6d2df6e5332013` | `tv-tape-pa2-i1-xs-dyn-pooc-54720b55` |
| `pa2-i1-xs-dyn` | the same without POOC | 9 | `626653faf60e832fc66d8cde05c62315193ca08faa8332ff36bfe7d9326f34ff` | `tv-tape-pa2-i1-xs-dyn-626653fa` |
| `pa2-i1-xs-qty-pooc` | XS with qty 100 (POOC) | 9 | `54720b55cf52d84d1455db4abc4ef12c119dfae53013ddfdff6d2df6e5332013` | `tv-tape-pa2-i1-xs-qty-pooc-54720b55` |
| `pa2-i1-xs-qty` | the same without POOC | 9 | `626653faf60e832fc66d8cde05c62315193ca08faa8332ff36bfe7d9326f34ff` | `tv-tape-pa2-i1-xs-qty-626653fa` |
| `pa2-i1-xf-tight-qty-pooc` | XF of a long id with no order, qty 100, bracket 3c either side (POOC) | 9 | `c78b18abe1bd4af3d01c40b7c1705d3d7a20505796e449b699515a590c1c5fce` | `tv-tape-pa2-i1-xf-tight-qty-pooc-c78b18ab` |
| `pa2-i1-xf-tight-qty-nocoof-pooc` | the same placed at a bar close, calc_on_order_fills off (POOC) | 6 | `3999af4912a43d9cc4e5f13697b1016a09bb7f5fcda33c656c8321b7c8ea330a` | `tv-tape-pa2-i1-xf-tight-qty-nocoof-pooc-3999af49` |

What they show: a pending parent's untouched protective bracket rests (3 of 3); a
pending parent's crossed stop exits at the parent's own fill (3 of 3); an exit placed
while its id has no order at all is never active -- the id's trade runs to the
`close_all` even with a 3c bracket, which is why five of the tapes carry the same
trade list (`c78b18ab...`, `626653fa...`, `54720b55...`).
