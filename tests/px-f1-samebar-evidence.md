# PX-F1 same-bar entry tapes

`px-f1-samebar-entries` embeds 90 independently exported synthetic cases:
30 with explicit quantity and pyramiding 7, 30 with explicit quantity and
pyramiding 1, and 30 with default fixed quantity and pyramiding 7.
The initial position is flat, short 1/2, or long 1/2. Each position is tested
with the source sequences LS, SL, LSL, SLS, LLS, and SSL. All quantities are 1
except the explicitly sized seed.

The wholly synthetic sources and full exports are retained under the
campaign scratch directory `px-f1-scratch`. They were exported with
`lab tv --no-note`, BINANCE:ETHUSDT.P, 15 minutes, April 1-4, 2025.
Every export reports `rangeProof: covered`; trade timestamps in the CSV are
UTC+8 and the fixture subtracts eight hours. No population source was sent
to TradingView.

| Source | Source SHA-256 | Trade CSV SHA-256 |
| --- | --- | --- |
| px-f1-p7-explicit-entry.pine | 4a1d51caef63cd705ed2838ff6f5b8116353520a675d7bb3d3cf950f77b3ac39 | bea6dd9796fef1220c589e197049d12155279202dddf580208cfcf5fcc3271ac |
| px-f1-p1-explicit-entry.pine | 0e573397ad4c06f25e0c9e7b5b0ec68df7a94a479ddcdaa89eef4eb55508a776 | cfdf25a693c663d42526cf999fa2153fcce004f58740f45553738af75fdb9bfb |
| px-f1-p7-default-entry.pine | 9bb353158a35cf3a4613569cef44c0376b464983cfbff4b249906da68e421ad5 | bea6dd9796fef1220c589e197049d12155279202dddf580208cfcf5fcc3271ac |

## Observed rule

Entry admission and transaction sizing use the position projected through
earlier accepted source calls. A same-direction add transacts its own
quantity. A reversal transacts its own quantity plus the projected opposite
position and resets the projected pyramiding count. The accepted market
transactions then fill buys before sells, preserving placement order inside
each side. They are not resized into new reversals at fill time.

For example, case C07 starts short 1 and places S then L at pyramiding 7:
S transacts 1; L transacts 3. L fills first, closes the seed and opens long 2.
S then closes 1 of that long without opening a short. The tape splits the
long into a quantity-1 zero-length trade and a quantity-1 surviving trade,
both carrying L's entry identity. At pyramiding 1 the initial S add is
rejected, so L transacts 2 and opens only long 1.

The tests reproduce trade direction, quantity, entry/exit identities and
timestamps using constant-price bars. All prices are 100 and PnL is zero;
the fixture does not require a corpus checkout or a TradingView connection.

Additional synthetic exports cover raw `strategy.order`, mixed
`strategy.close`/entry sequences, and `process_orders_on_close`. They are
boundary controls, not silently accepted entry fixtures. Raw orders do not
auto-reverse; close calls can consume the projected position and have their
own entry identity when an over-sized close crosses zero. The patch does not
change these routes or the existing commissioned, margined, priced,
fill-recalculation, risk-gated or process-on-close behavior.

The adapter change is limited to ordinary batch-mode fixed-quantity market
entries with positive pyramiding, zero margins/costs, no OCA or live risk
gate, and no active intrabar sampler. Existing frozen transaction fields,
source ordering and native `Transact` requests carry the policy; no durable
state or kernel change is introduced.
