# Percentage exit reservation

These independent synthetic strategies were exported through `lab tv --no-note`
on BINANCE:ETHUSDT.P, 15-minute bars, April 1–7, 2025. Each directory contains
the exact strategy and TradingView export provenance. CSV line endings are
normalized to LF with a final newline; `meta.json` retains the upstream export
hash and separately records the normalized fixture hash. Tape timestamps are UTC+8;
the embedded OHLCV bars and test timestamps are UTC.

These tapes prove named, absolute, limit-only `qty_percent` targets after a
partial runner completes, including entry ancestry reduced by margin calls.
The adapter sizes that target from the filled quantity of the entry incarnations
that remain open, including their completed slices, while reserving only the
currently unreserved live units. A live percentage reservation retains its
existing quantity when reissued. Two-sided brackets, relative levels, unnamed
exits and trailing calls retain their previous sizing and pending-close basis;
these tapes do not establish a new rule for those shapes.

- `reset-fixed`: a runner closes half of a short entry, both exit calls have
  absent operands across midnight, and the percentage target is recreated.
  The target owns all 28 remaining units; the completed runner cannot steal 14.
- `reset-margin`: the same lifecycle with an opening margin slice. The target
  keeps 28.1187 units, while the runner closes 26.8711.
- `new-percent`: a different 50% exit ID created after a 28-unit partial close
  owns the remaining 28 units of the original 56-unit entry, not 14.
- `rearm-default`: continuously repeating an implicit-full runner does not
  steal the reservation of its standing percentage sibling.
- `nan-standing`: an all-absent standalone target is withdrawn; the cleanup
  order closes all ten units. Absent operands are not globally inert.

`test_percent_entry_reservation_tapes.cpp` replays all five scripts through the
adapter and compares every closed trade's side, entry/exit time, tick prices and
quantity against these tapes. No generated code or native trailing change is
needed for the reservation rule.
