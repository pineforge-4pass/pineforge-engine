# The children of a global exit that one path point triggers (the N5 pin)

TradingView keeps one child of a `strategy.exit` that names no entry for every open entry. When one
path point triggers several children, it fills them one at a time. The children of the entry id that
comes first in TradingView's entry-id table go first, and an older entry's child goes before a newer
one's of the same id.

The table iterates as a `java.util.HashMap<String, ?>`:
- `String.hashCode` over the id's UTF-16 code units, spread as `h ^ (h >>> 16)`;
- the buckets in ascending order, the newest key first inside a bucket;
- its keys are every entry id from its first fill on (never removed), plus one transient key per
  `strategy.close` / `close_all` order between its call and its fill; an entry order still waiting
  is no key;
- 16 buckets, doubling once the keys exceed 13/16 of them, never shrinking.

Each child is a transaction of its entry's quantity. It settles against its own entry under
`close_entries_rule = "ANY"` and against the oldest lots first otherwise, and every settled slice is
a row of its own. A slice's percent fee is its share of its lot's entry fee plus its share of its
transaction's exit fee, each on TradingView's ten-significant-digit money. For example, an add's
child that fires before the older entry's child closes the older lot and part of the add
(`apex-third`).

A named stop whose limit entry fills at a point where the stop is already through fills at once at
the entry's fill, with the stop's slippage. It never fills at the level the bar crossed before the
entry filled (`eth-member`, `eth-fresh`).

## Layout

These are all 127 tapes of the pin's own synthetic scripts. Each directory holds:
- `strategy.pine` and `tv_trades.csv` (times at UTC+8), byte for byte;
- `metrics.json` from the first of two `lab tv --no-note` exports (maintainers' private
  pineforge-workflow, channel `ws-report-v1`), whose `tv_trades.csv` are byte-identical;
- `provenance.json`, with:
  - the pin's tape name and the schedule (`parameters`) the script renders;
  - the shared bars file and its sha256;
  - the tape's status;
  - for a known divergence, the rule it needs and its first departure.

`bars/` holds the four feed windows the tapes share, each the chart feed's bars for the window.

`schedules.inc` is the schedule each `strategy.pine` renders, as C++ data. It is parsed from the
script itself (the pin's restricted grammar), and each `strategy.close` call-site token is the one
codegen d9b92c7 emits for that call. `known_divergences.inc` is generated with it. Never edit either
by hand. `tests/test_global_exit_children_tapes.cpp` replays every tape through the Pine adapter.

**Asserted tapes.** Each row must be the engine's: ids, times, side, prices in ticks, quantity, net
profit at the report's precision, and commission at ten significant digits.
- 91 of these reproduce all ten of the pin validator's exact fields.
- `eth-member` and `eth-fresh` differ only in the commission's eleventh significant digit, a
  separate rounding this pin does not cover (rule 8).

**Known divergences.** These run and report where the engine still departs from TradingView. They
assert nothing.

## Pinned shapes

The same test holds scripts the pin does not reach at the rows the engine booked on 700c5d24, bit
for bit (`pinned_rows.inc`, harvested with `-DPINEFORGE_GLOBAL_EXIT_CHILDREN_HARVEST`):
- a global exit of an explicit quantity;
- a named and a global child at one point;
- 27 ids (a table past 32 buckets);
- 14 ids with two of them in one bucket of the grown table;
- a waiting `strategy.order` market order at 13 keys;
- the newborn stop under `calc_on_order_fills`.

## Asserted tapes (93)

| fixture | pin tape | trades | bars |
|---|---|---:|---|
| `apex-third` | `n5b-13-apex-third` | 5 | `eurusd-2025-07-30-2025-08-04` |
| `eth-fresh` | `n5b-17-eth-fresh` | 1 | `ethusdtp-2025-05-21-2025-05-23` |
| `eth-member` | `n5b-16-eth-member` | 2 | `ethusdtp-2025-05-21-2025-05-23` |
| `global-13` | `n5-13-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-14` | `n5-14-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-15` | `n5-15-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-16` | `n5-16-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-17` | `n5-17-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-18` | `n5-18-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-19` | `n5-19-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-20` | `n5-20-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-35` | `n5-35-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-36` | `n5-36-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-37` | `n5-37-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-38` | `n5-38-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-39` | `n5-39-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-40` | `n5-40-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-41` | `n5-41-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-42` | `n5-42-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-43` | `n5-43-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-44` | `n5-44-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-45` | `n5-45-global` | 5 | `eurusd-2025-07-30-2025-08-04` |
| `global-46` | `n5-46-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-47` | `n5-47-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-48` | `n5-48-global` | 5 | `eurusd-2025-07-30-2025-08-04` |
| `global-57` | `n5-57-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-58` | `n5-58-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-59` | `n5-59-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-60` | `n5-60-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-61` | `n5-61-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-62` | `n5-62-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-63` | `n5-63-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-64` | `n5-64-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-65` | `n5-65-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-66` | `n5-66-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-67` | `n5-67-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-68` | `n5-68-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-69` | `n5-69-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-70` | `n5-70-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-71` | `n5-71-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-72` | `n5-72-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-73` | `n5-73-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-74` | `n5-74-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-75` | `n5-75-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-76` | `n5-76-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-77` | `n5-77-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-78` | `n5-78-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-79` | `n5-79-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-80` | `n5-80-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-81` | `n5-81-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-82` | `n5-82-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-83` | `n5-83-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-84` | `n5-84-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-85` | `n5-85-global` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `global-86` | `n5-86-global` | 5 | `eurusd-2025-07-30-2025-08-04` |
| `global-87` | `n5-87-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `global-88` | `n5-88-global` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `grown-bx-g` | `n5b-25-grown-bx-g` | 15 | `eurusd-2025-07-30-2025-08-04` |
| `grown-q-xa` | `n5b-26-grown-q-xa` | 15 | `eurusd-2025-07-30-2025-08-04` |
| `hist13` | `n5b-05-hist13` | 15 | `eurusd-2025-07-30-2025-08-04` |
| `hist13-closeall` | `n5b-24-hist13-closeall` | 15 | `eurusd-2025-07-30-2025-08-04` |
| `hist4` | `n5b-06-hist4` | 6 | `eurusd-2025-07-30-2025-08-04` |
| `hist6` | `n5b-20-hist6` | 8 | `eurusd-2025-07-30-2025-08-04` |
| `hist7` | `n5b-21-hist7` | 9 | `eurusd-2025-07-30-2025-08-04` |
| `hist8` | `n5b-22-hist8` | 10 | `eurusd-2025-07-30-2025-08-04` |
| `live12` | `n5b-03-live12` | 12 | `eurusd-2025-07-30-2025-08-04` |
| `live12-pend3` | `n5b-29-live12-pend3` | 12 | `eurusd-2025-07-30-2025-08-04` |
| `live13` | `n5b-04-live13` | 13 | `eurusd-2025-07-30-2025-08-04` |
| `live13-close1` | `n5b-30-live13-close1` | 13 | `eurusd-2025-07-30-2025-08-04` |
| `live13-close3` | `n5b-31-live13-close3` | 13 | `eurusd-2025-07-30-2025-08-04` |
| `live14` | `n5b-27-live14` | 14 | `eurusd-2025-07-30-2025-08-04` |
| `live15` | `n5b-28-live15` | 15 | `eurusd-2025-07-30-2025-08-04` |
| `live16` | `n5b-18-live16` | 16 | `eurusd-2025-07-30-2025-08-04` |
| `live17` | `n5b-19-live17` | 17 | `eurusd-2025-07-30-2025-08-04` |
| `named-ba` | `n5b-12-named-ba` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `named-xz` | `n5b-11-named-xz` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `named-zx` | `n5b-10-named-zx` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `pending-21` | `n5-21-pending` | 1 | `ethusdtp-2025-05-21-2025-05-23` |
| `pending-23` | `n5-23-pending` | 2 | `ethusdtp-2025-05-21-2025-05-23` |
| `reversal-29` | `n5-29-reversal` | 2 | `nifty-2025-07-04-2025-07-09` |
| `reversal-53` | `n5-53-reversal` | 2 | `nifty-2025-07-04-2025-07-09` |
| `shrink13` | `n5b-07-shrink13` | 13 | `eurusd-2025-07-30-2025-08-04` |
| `siblings-01` | `n5-01-siblings` | 2 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-04` | `n5-04-siblings` | 4 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-50` | `n5-50-siblings` | 2 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-91` | `n5-91-siblings` | 4 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-92` | `n5-92-siblings` | 4 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-93` | `n5-93-siblings` | 4 | `xauusd-2025-05-20-2025-05-23` |
| `siblings-94` | `n5-94-siblings` | 4 | `xauusd-2025-05-20-2025-05-23` |
| `three-any` | `n5b-09-three-any` | 3 | `eurusd-2025-07-30-2025-08-04` |
| `three-fifo` | `n5b-08-three-fifo` | 5 | `eurusd-2025-07-30-2025-08-04` |
| `tie-aq` | `n5b-01-tie-aq` | 2 | `eurusd-2025-07-30-2025-08-04` |
| `tie-qa` | `n5b-02-tie-qa` | 2 | `eurusd-2025-07-30-2025-08-04` |

## Known divergences (34)

Recorded, not asserted. `known_divergences.inc` gives each one's row counts and first departing
field at the change's head; the test reports whether each still departs.

| fixture | pin tape | trades | the rule it needs |
|---|---|---:|---|
| `hist13-same` | `n5b-23-hist13-same` | 15 | rule 7: a same-bar close(id) and entry(id) run the entry first |
| `pending-22` | `n5-22-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-24` | `n5-24-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-25` | `n5-25-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-26` | `n5-26-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-27` | `n5-27-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-28` | `n5-28-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-54` | `n5-54-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-55` | `n5-55-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-56` | `n5-56-pending` | 2 | pending entry: a named exit of a waiting entry survives a close of the position |
| `pending-89` | `n5-89-pending` | 1 | pending entry: a global exit placed for a waiting entry survives to its fill |
| `reversal-30` | `n5-30-reversal` | 4 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-31` | `n5-31-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-32` | `n5-32-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-33` | `n5-33-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-34` | `n5-34-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-51` | `n5-51-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-52` | `n5-52-reversal` | 5 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-90` | `n5-90-reversal` | 4 | reversal: a close before a reversal leaves the requested opening leg |
| `reversal-95` | `n5-95-reversal` | 1 | rule 6: a range-end row reports the entry commission only |
| `reversal-96` | `n5-96-reversal` | 3 | reversal: a close before a reversal leaves the requested opening leg |
| `siblings-02` | `n5-02-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-03` | `n5-03-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-05` | `n5-05-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-06` | `n5-06-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-07` | `n5-07-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-08` | `n5-08-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-09` | `n5-09-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-10` | `n5-10-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-11` | `n5-11-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-12` | `n5-12-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `siblings-49` | `n5-49-siblings` | 4 | rule 1: child identity per (exit id, entry incarnation) |
| `terminal-part` | `n5b-15-terminal-part` | 3 | rule 6: a range-end row reports the entry commission only |
| `terminal-pct` | `n5b-14-terminal-pct` | 2 | rule 6: a range-end row reports the entry commission only |
