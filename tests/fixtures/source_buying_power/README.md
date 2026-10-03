# Source-time buying power tapes

These independently authored strategies were exported from TradingView's
WebSocket strategy report on 2026-10-03. Each directory records the exact source,
symbol, interval, UTC date range, report hashes, and export count in
`provenance.json`. CSV trade timestamps are UTC+8. `bars.csv` contains the matching
chart bars in UTC milliseconds; symbol tick and lot grids are set explicitly by
`test_source_buying_power_tapes`.

| Fixture | Contract |
| --- | --- |
| `flat-under`, `flat-equal` | A whole-lot all-in flat entry with an attached bracket refuses unrounded equity just below rounded notional, but admits equality. |
| `pooc-under`, `pooc-equal` | The same money boundary applies to a true-flat on-close entry with pyramiding 2. |
| `whole-under`, `whole-equal` | Held whole-lot reversal controls retain the existing closing-only versus reversal behavior. |
| `fractional-fill-band`, `band-flat-residue` | Decimal band equality at 3386.68 refuses its tick-index product, for a reversal and a true-flat entry, even if raw notional fits. |
| `band-reversal-equal`, `band-flat-equal`, `band-reversal-increase` | Decimal band equality at the exact-double fill 3865.5 admits; a strict decimal or strict integer-tick rule is contradicted. |
| `band-reversal-above`, `band-flat-above` | A band just above 3865.5 admits; the affordable band is not rounded to whole ticks. |
| `fractional-affordable` | An affordable upward-gap reversal admits and retains its entry-bar margin slice. |
| `forex-under`, `forex-equal` | An opposite entry compares exact source equity to money-grid notional without an additive epsilon; equality admits. No currency conversion is used. |
| `fx-1-{under,equal,above}`, `fx-omitted-{under,equal,above}` | Explicit and default pyramiding 1 use the same exact source-money comparison as pyramiding 0; a one-ULP deficit refuses the opening leg, equality and one-ULP surplus admit. |

The fixtures preserve the exported Pine bytes, including hash-pinned scratch
names in older strategy titles; those titles are export identities, not a
repository naming convention. The committed trade CSVs normalize
line endings and add a final newline; `exportTapeSha256` identifies the original
export and `fixtureTapeSha256` identifies the committed copy. Corresponding
fixture source and bar hashes identify the bytes used by the C++ replay.

The original ten row-set comparisons fail four cases against the unchanged
previous adapter. Parsing assertions are additional integrity checks, not
independent behavioral pins. The six review controls pin both inclusive
exact-double equality and residue-class refusal. The fill operand is rebuilt
from its tick index, not accepted with an incidental incoming-price residue.
This is TradingView's representation-qualified inclusive comparison, not a
strict decimal inequality; money-notional equality separately admits.

The epsilon removal is restricted to the ordinary, no-cost low-value opposite
market shape proved by `forex-under/equal` and the explicit/default pyramiding-1
controls; other fractional/POOC/cost shapes
retain their previous guard. Flat whole-lot admission retains TAIL-D's settled
agreement or no-gap qualification. No realized-cash rounding rule is prescribed.

The six pyramiding-1 controls fail the two under-capital row-set comparisons
against wave head `a747ad5b`. Their sources, trade tapes, and provenance were
exported independently, not derived from population strategy sources.
