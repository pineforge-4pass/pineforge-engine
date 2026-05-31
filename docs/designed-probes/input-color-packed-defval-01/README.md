# input-color-packed-defval-01

## Feature under test
Codegen #9 — **input.color defval lowering** to a packed ARGB int64 literal
(`0xAARRGGBB`) and routing through `get_input_int64`.

## Why this is NOT a TV-parity probe
Color inputs are cosmetic — they never alter the trade list, so there is no
`tv_trades.csv` to compare. This is a **compile / generated-code golden**.

## Validation
- Transpile and inspect `generated.cpp`:
  - `color.red`        → `get_input_int64("Const color", pine_color::red)`
  - `#00e676`          → `get_input_int64("Hex #RRGGBB", 0xff00e676LL)`
  - `#00ff0080`        → `get_input_int64("Hex #RRGGBBAA", 0x8000ff00LL)`
  - `color.new(...)`   → `get_input_int64("Builder", pine_color::new_color(...))`
- No occurrence of `get_input_int("…")` with a color title, and **no** color
  defval lowered to `0`.
- The codegen unit tests already pin these:
  `tests/test_codegen_input_getters.py::test_input_color_*`,
  `tests/test_input_time_int64.py::test_input_color_routes_to_int64`.

## Promotion
If a runtime/UI color-value harness is added later, this can become a
behavior golden. As a trade-list corpus probe it adds nothing — keep it as a
compile golden (do **not** move into `corpus/validation/` expecting parity).
