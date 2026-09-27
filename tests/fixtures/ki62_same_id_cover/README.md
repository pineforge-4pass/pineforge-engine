# The KI-62 cover takes only a same-id pyramid add (lane W8E-EXITS)

KI-62 (ab9714be `pine_fills.cpp:7026-7033`): when a priced `from_entry` exit
fills on the bar a MARKET pyramid add of its own entry id opened, the exit
first closes the id's older lot (FIFO) and then covers the whole same-bar add
at its own fill price, a second fill of the same order. The adapter
(`pine_adapter.cpp`, the cover after a bracket leg's fill) used to cover the
add after any such leg fill, even when the leg had reduced nothing but the add
itself or (FIFO) another id's older lot. TradingView covers only behind the
leg's reduction of an older lot of the id. Each rule is pinned by synthetic
probes written for this lane; no closed or scraped strategy is involved.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-04-03, `pyramiding = 2`, a fixed default size of 1 and an
initial capital of 1000000, with fixed `timestamp("UTC", 2025, 4, 1, ...)`
cells and a `strategy.close_all()` at 13:30.

`tests/test_ki62_same_id_cover.cpp` replays every tape through the Pine host
under the configuration the generated constructor declares, over the corpus
15m bars embedded in `bars.inc`, trading from 2025-04-01 11:30 UTC, and
requires each trade the tape closes -- entry and exit time, side, price in
ticks of 0.01, quantity in lots of 0.0001 -- to be the engine's.

| tape | trades | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|
| `t1-two-lots-exit-qty1` | 4 | A (2 units, 11:45 open) with qty=1 exits AT1 (limit 1861) and AT2 (limit 1869.55); at 12:00 B (2 units, another id) with qty=1 exits BT1 (limit 1869.75) and BT2 (limit 1950). On the 12:15 bar B opens at 1868.83, AT2 closes A's last unit at 1869.55 and BT1 closes ONE unit of B at 1869.75; B's second unit rides to the 13:30 close_all | `be4a9a4a5ca7e2defab27248015c6c84e4a02e4dfdcdeea6da070342b1965159` |
| `t3-same-id-add-cover` | 2 | A (1 unit, 11:45 open) with a qty=1 exit AX (limit 1869.55); at 12:00 a second A (same id, 1 unit). On the 12:15 bar the add opens at 1868.83 and AX closes the older A lot and covers the add, both at 1869.55: two units for a one-unit exit (the control, which the engine already booked) | `55e26cccf4be556d44c70909fb293a1f96b3c9d2cafc9592b8f6def5c77fabc5` |
| `t4-fifo-other-id-drain` | 2 | A (1 unit, 11:45 open); at 12:00 B (2 units, another id) with qty=1 exits BT1 (limit 1869.75) and BT2 (limit 1950). On the 12:15 bar B opens at 1868.83 and BT1 fills FIFO against A's older lot, closing it at 1869.75; both units of B ride to the 13:30 close_all | `17a9d88eafc91b5dba23bd86ff1d3a8471f5d92da749d37ecd126871bf11f462` |

On `t1` B's lot is a market pyramid add (it opened beside A's) and BT1's fill
reduced only that lot; the engine still covered B's remaining unit at BT1's
price as a second BT1 fill. The population shape is thulashimohanr's "Short" pair
(ShortT1/ShortT2, qty=1 each) opened beside a reversal lot's last unit
(NSE:NIFTY 15, 2025-05-15 04:15 and 2025-05-16 04:15).

On `t4` BT1's fill closed only A's lot, an older lot of another id; the
engine covered both units of B at BT1's price as a second BT1 fill.
