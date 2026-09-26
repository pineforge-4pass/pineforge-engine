# process_orders_on_close reversing stop tapes (R5 lane PAR-ORDERS-2, item 3)

TradingView's own trades for corpus probe 96's flip on bar counts, under
`process_orders_on_close`: every six bars a stop entry of the other side is placed
while the book is still open, with a same-bar `strategy.close` of the held id (or
without one: a plain reversal), the stop 5c on the close's marketable side (or 5c
beyond it, the control). TradingView fills the reached entry at the signal bar's
close, its quantity grown by the side it was placed against.
`tests/test_pooc_reversing_stop_tapes.cpp` replays each tape through the Pine
adapter on the 15m chart (`-chart`) or the 1m bars aggregated to 15m under the
magnifier (`-mag`).

Each directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, NYSE:F 15, window 2025-07-01 .. 2025-07-08), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The
scripts count chart bars from 2025-07-02 09:30 ET, the first bar of
`tests/fixtures/session_islastbar/bars.inc`, whose 15m and 1m bars the rows replay.

| probe | variant | trades | tvTradesSha256 | note |
|---|---|---|---|---|
| `pa2-i3-flip-s-chart` | flip with a same-bar strategy.close, first flip short, chart | 22 | `fece5dc70a3bcd9590bb28f768f5bb150c886836e049ec2a2ddd9d742250a4b0` | `tv-tape-pa2-i3-flip-s-chart-fece5dc7` |
| `pa2-i3-flip-s-mag` | the same, bar magnifier on | 22 | `e3ef67816e8b94589b18ec740441d479a10e069527f123914cafc69ba970cf8a` | `tv-tape-pa2-i3-flip-s-mag-e3ef6781` |
| `pa2-i3-flip-l-chart` | flip with a same-bar strategy.close, first flip long, chart | 22 | `213292ff0d373a4dffcc4936c2da142ee6df7508179d5fe21542ea2881f22690` | `tv-tape-pa2-i3-flip-l-chart-213292ff` |
| `pa2-i3-flip-l-mag` | the same, bar magnifier on | 22 | `33665e31b1e690ae885c2c51e5e19805659fac961fcf9f932d85623b83155c40` | `tv-tape-pa2-i3-flip-l-mag-33665e31` |
| `pa2-i3-rev-s-chart` | plain reversal, no same-bar close, chart | 22 | `e4d9c73535ca8fb6fb98bf23951822c3fcbe9a5742575182a1aa1894715669e5` | `tv-tape-pa2-i3-rev-s-chart-e4d9c735` |
| `pa2-i3-rev-s-mag` | the same, bar magnifier on | 22 | `fc405dd76617426a32815a8eade4ea07df3603ad2c5dab7572c1f9e0af36945c` | `tv-tape-pa2-i3-rev-s-mag-fc405dd7` |
| `pa2-i3-flip-unreached-s-chart` | flip, the stops 5c beyond the close (control), chart | 11 | `b2e8779bcf6bc769756b0b00cedcb182230c814985b6131b7f390b7ca70eb39e` | `tv-tape-pa2-i3-flip-unreached-s-chart-b2e8779b` |
| `pa2-i3-flip-unreached-s-mag` | the same, bar magnifier on | 11 | `b19172f54665a75a1d9e13c603759de43cb0b6cfa4624afaeb33c54cfa792cd3` | `tv-tape-pa2-i3-flip-unreached-s-mag-b19172f5` |

The last row of each flip and reversal tape is TradingView's trade still open at
the end of the data (its exit signal empty); the row compares it with the book the
replay ends on.
