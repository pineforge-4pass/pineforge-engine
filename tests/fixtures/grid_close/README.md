# strategy.close(id) under the FIFO rule books from the close ledger (lane W3B-ENG-GRID)

Under the default FIFO `close_entries_rule`, TradingView sizes a
`strategy.close(id)` without `qty` from a ledger of the units entered under
`id` that no fill has booked yet, not from the lots still carrying `id` (the
FIFO rule has usually closed those). The grid bots of population family F02
(TAIL-TRIAGE; pyramiding up to 200, one `strategy.close` call site in a loop
over the grid levels) read that ledger on every take-profit. TradingView keeps
it by these rules:

- One `strategy.close` call site places one order per bar. A later call of the
  site re-sizes that order for its own id and comment, but the order's fill
  books against the id of the site's first call on the bar.
- A fill books against its id's unbooked units, oldest first; what they cannot
  cover spills over every id's unbooked units in the order the entries filled.
- A call whose id has no unbooked units places nothing and leaves its site's
  order as it was.
- Every other fill that reduces the position books too: `strategy.close(id,
  qty)` and a `strategy.exit` against the entry they name, a fill that names
  none (a `strategy.order` sell, a margin call) against the oldest entries'
  units.

The adapter kept a per-id ledger, but it erased the first id's units whole and
reserved the survivor's fill against the other ids instead of booking it, so
the units the first id could not cover stayed on the books: `close("L38")`
closed 0.19 where TradingView closes 0.18 (`w3f02-x2`), or nothing
(`w3f02-x1`).

Each directory is one `lab tv` export (channel `ws-report-v1`, `rangeProof`
covered, `--no-note`), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-05-01, omits `initial_capital` (v6: 100000) and declares
`default_qty_type = strategy.fixed, default_qty_value = 1`. The `w3f02-*`
probes are lane W3-ENG-EXIT-ALLOC's; the `w3bf02-*` probes are this lane's
(`exec/W3B-ENG-GRID-scratch/tools/closeseq.py`): steps of 15 minutes from
2025-04-08 00:00 UTC, entries `BUY_<id>` through one `strategy.entry` call site
in a loop, loop closes `TP_<id>` through one `strategy.close` call site in a
loop, sole closes `SOLE_<id>` through a second call site, a cleanup
`close_all` at step 20.

`tests/test_grid_close_tapes.cpp` replays every tape through the Pine adapter
under the configuration the generated constructor declares for that probe,
over the corpus 15m bars of `tests/fixtures/exit_queue/bars.inc`, trading from
2025-04-08 00:00 UTC with TradingView's 0.0001 lot as the `qty_step`, and
requires each trade the tape closes inside the replayed bars -- entry and exit
time, side, price in ticks of 0.01, quantity in lots of 0.0001, exit signal --
to be the engine's. It also reads the rules off TradingView's own rows.

## The rule tapes (the engine booked them wrong before the fix)

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3f02-x2-min-5call` | `process_orders_on_close`, pyramiding 200: twelve grid entries (L38 .. L49, 0.19 .. 0.22); at step 3 the loop closes L45 .. L49 and only the last call's order fills, sized for L49 (0.22) but booked against L45 (0.21); the 0.01 L45 cannot cover comes off L38, the oldest entry, and `close("L38")` at step 13 closes 0.18, not 0.19 | `ae92feca54a06eee2189a5de6de74d78600b38517c52088ccebb5c8994c2112c` |
| `w3f02-x1-xlm-sequence` | an xlm grid bot's sequence on the same entries: later loops spill the rest of L38 away, and its close at step 13 is void | `393bd244059616417cfed9474ec7b9eebc7decbaee205ff5e97f0359f820a392` |
| `w3bf02-a2-loop-reversed` | B 0.1, A 0.2, C 0.3, D 0.5; the loop closes D then C: the order is C's 0.3, booked against D, which keeps 0.2 for its sole close; C keeps its 0.3 | `fea32f41ef6596ae6e7e817d23aa26ab23ac9a607b665a68326090a114524fd4` |

## The reductions that name another entry or none (the engine left them unbooked)

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3bf02-f1-order-reduce` | A 0.1, B 0.2, C 0.3, each close its own call site; at step 3 `strategy.order("trim", strategy.short, qty=0.1)`: the sell books A's units, and `close("A")` at step 5 is void (the engine closed 0.1) | `e8f75f9161d03df4b14852e72053a6248c5e9b6b252603906a73e36dbfc30cc3` |
| `w3bf02-f3-close-explicit-qty` | the same entries; at step 3 `close("B", qty=0.1)` takes A's lot but books B's units: `close("B")` then closes 0.1 (the engine closed 0.2), `close("A")` 0.1 | `f77ab88fbe8c3852b43eb1969bce5c9e901895467b03398c755ad269cc999124` |

The host's `strategy_order` takes no comment (nor does the generated call), so
the engine books f1's `TRIM` exit unsigned; the test compares that row without
its signal.

## Without process_orders_on_close (the engine sized the close from the id's lots)

The same ledger sizes the close when orders fill at the next open. The adapter
sized a strategy.close(id) there from the lots still carrying the id and
dropped the call when the FIFO rule had closed them all.

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3f02-g1-close-after-fifo-consumed` | g2 without `process_orders_on_close`: `close("A")` still closes 1 at the next open (the engine dropped it) | `daf5a60a1ae4b48809f96b24fb6a77f49f1f901ca0f68ee020efaecd5c7e4b06` |
| `w3bf02-d1-nopooc-five-call` | x2 without `process_orders_on_close`: the loop's order fills 0.22 at the next open, booked against L45, and `close("L38")` closes 0.18 at step 14 (the engine dropped it) | `0a4dcf48a702c2d4307e9fa2292562ed4f4b90c641b55e0d1c10620af78719e5` |
| `w3bf02-d2-nopooc-spill-oldest-record` | a1 without `process_orders_on_close`: `close("A")` closes the 0.1 the spill left (the engine dropped it) | `b3a9866683f73a51ea063045356948b6e9271f5fccafe18476722d5a0597a5a7` |
| `w3bf02-f2-nopooc-exit-reduce` | A 0.1, B 0.2, C 0.3; at step 3 `strategy.exit("X", from_entry="B")` with a limit 5% under the close fills 0.2 at the next open, A's lot and half of B's, but books B's units: `close("A")` closes 0.1 (the engine dropped it), `close("B")` nothing | `4ae4027fb07609c71b123fe6acc08ace00197432a889cb5761bc8eeec4ba75c5` |

## The retired reservation model's scenarios (the engine held standing claims)

The adapter used to hold a close site's fill as a standing reservation against
the other ids and carried the first call's target between two-call sites; the
rows of `test_integration_l4d` pinned that model's claims and trades. These
probes replay those rows' command sequences (`tools/siteseq.py`: each
`strategy.close` call site a loop of its own, `process_orders_on_close`,
pyramiding 10) and show no standing claim: each site fills its survivor's whole
ledger, and a `strategy.order` sell that used up an id voids that id's closes.
The twin's rows now pin TradingView's outcome (`tests/twin_parity_inventory.json`
records the rewrite).

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3bl-s6-single-site-replacement` | seven entries; one site closes F7 then L7, F15 then L15, then L3 then L4: the last order closes L4's whole 0.4159 (the model capped it at 0.3560) | `6d2410d125e87791d6082a0e6437110fb4c5cad2434ac9e20eb11e2e58e17fde` |
| `w3bl-s7-rejected-replacement` | A 1, B 2, a sell of 1; two sites close A (void: the sell used A up), the second then closes B: B's order takes the book (the model rejected it) | `7b7ffcbd77aad0689fcfd7550a6d2812c6e465f3b8da04b3b3d9bd88b6df73ab` |
| `w3bl-s8-cross-bar-claims` | A, B, D 3, C; two sites first naming A close B and C; a later close of D closes D's 3 (the model capped it at 2) | `a66d0a34e6f52a1924b70e62d47b995695c66989c37672f56ede0d9f86ea3570` |
| `w3bl-s9-same-id-owner-claims` | two sites on A's re-entries, a sell of 1, then a close of D closes D's 4 and flattens the book (the model closed 3) | `9cd0d293caa52bea4f252bf8a4b40874c7b269f69844602b43f1ee8749885b93` |
| `w3bl-s10a-local-alias-small-first` | two sites on B's re-entries, a sell of 2; one site closes B, C then D: B's and C's calls are void, D's order closes D's 4 (the model closed 3) | `31b0e516086782547383b1535fe16cc9dc615b600bc9b501fbe19f171411868a` |
| `w3bl-s10b-local-alias-big-first` | s10a with the two earlier sites swapped: the same rows (the model closed 3.4) | `31b0e516086782547383b1535fe16cc9dc615b600bc9b501fbe19f171411868a` |

## The controls (the engine already booked them)

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3bf02-a1-spill-oldest-record` | a2 with the loop closing C then D: D's 0.5 books against C's 0.3 and spills 0.2 over B (filled first, 0.1) and then A: `close("A")` closes A's 0.1 left, `close("B")` nothing -- the spill follows fill order, not the ids' names | `56f37ead54a5f725bed3a5342f2f4f1ccdb1878cc7b7a90d6968ef2aa447cefa` |
| `w3bf02-b1-reentry-record-age` | A 0.1, B 0.2, A closed, A re-entered 0.1, C 0.1, D 0.3; the loop C, D spills 0.2 over B, not over the re-entered A | `5b022c2687c40b0ab133e15cd4bbbf54f890ee528456c8bb84e7f57c3d39903c` |
| `w3bf02-c1-first-call-never-entered` | the loop first closes an id never entered: that call is no call, the order books against Y | `29543e1e27eee034785e34592bb36bea3f7823c4492580e99b5fa5ffd78ca2fe` |
| `w3bf02-c2-last-call-never-entered` | the loop last closes an id never entered: Y's order fills as it was (c1's rows) | `29543e1e27eee034785e34592bb36bea3f7823c4492580e99b5fa5ffd78ca2fe` |
| `w3bf02-c3-first-call-closed-by-name` | the loop first closes W, which a sole close already closed: no call | `1e558bbfc5ff80c5aaaff6a2c959a6d743759db0d7eecbd5875bd044ba63e861` |
| `w3bf02-e1-pyramiding-refused` | pyramiding 3 refuses the fourth entry D: D has nothing to close, the loop D, C closes C | `51f3fff6341881c773a7f1f4e0216a4a7db3695cec9809d8e69515f155fcf58e` |
| `w3bf02-e2-offgrid-qty` | entries 0.123456 and 0.300049 fill 0.1234 and 0.3000; the ledger holds those | `033362abdf956fb1354cc347017866feb41c41ac06a845716ccd8f480a6a7fe7` |
| `w3f02-g2-pooc-close-after-fifo-consumed` | A 1, B 2 at 00:00 and 01:00 on 04-08, 09 and 10: `close("B")` takes A's lot and one of B's; `close("A")` still closes 1 | `dd4ae9fc6fe3e7099eb6db7dd44e2dbe46d982bb48662c8f82f2a0289f495d37` |
| `w3f02-g3-pooc-close-remaining-fragment` | A 2, B 1: `close("B")` takes one of A's; `close("A")` closes A's 2 | `15406e57ea898ca421a42a556ab782a96960265f200a7913c7636e6d97fef537` |
| `w3f02-g4-pooc-loop-close-consumed-ids` | L1 .. L4; one loop site closes L1 then L2: one order, L2's | `eb528911cf4a87e6e9cfe7263b4f9e5eee718180099639555908b52967b6d5e8` |
| `w3f02-g5-pooc-two-sites-close-consumed-ids` | g4 with two call sites: two orders | `bc2f445c67bb21eb74aeb293d738454750f9841b00164033ee4524e979948131` |
| `w3f02-g6-pooc-close-id-loop-survivor-consumed` | the loop L3, L4 books against L3; `close("L1")` still closes 1 | `452f74130698cef3c7fdbe81b38cc979663db960ece47b7ce7a271a262d7b812` |
| `w3f02-g7-pooc-close-id-sole-close-consumed` | a sole `close("L4")` takes L1's lot; `close("L1")` still closes 1 | `3510913b73961b773819281ba15c203e92a9a3f09d10b2864a0c4bec4ff62bf6` |
| `w3f02-g8-pooc-loop-entries-close-first` | L1 .. L3 through one entry loop: `close("L3")`, then `close("L1")` closes 1 | `4f4ef8d908b1e5eb11beded7f2e53f3530082a1ccc5c1358bac5a8673aad7dfc` |
| `w3f02-g9-pooc-loop-entries-close-second` | g8 closing L2 second | `b33529bb25576f54a9cafa3e0901130b2e6ad885ca987765d396cff5075fe77e` |
