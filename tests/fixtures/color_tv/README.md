# Colors as TradingView reads them back (lane W11-ENG-TIME-COLOR)

TradingView stores a color's transparency `t` (0 opaque, 100 invisible) as
the alpha byte nearest `255 * (100 - t) / 100`. `color.t` reads a byte `a`
back as `100 - a * 100 / 255`, rounded to the nearest whole number. A whole
`t` therefore reads back as itself (`color.new(c, 90)` is 90), and a
fractional one goes through the byte (`color.new(c, 10.5)` reads 11 and
`color.new(c, 20.5)` 20). `color.new` replaces the transparency of the color
it is given; it does not compound it. The named constants are Pine v6's; v5
differs in `color.red` (#FF5252), `color.teal` (#00897B) and `color.yellow`
(#FFEB3B) only.

| name | v6 | v5 |
|---|---|---|
| aqua | #00BCD4 | = |
| black | #363A45 | = |
| blue | #2962FF | = |
| fuchsia | #E040FB | = |
| gray | #787B86 | = |
| green | #4CAF50 | = |
| lime | #00E676 | = |
| maroon | #880E4F | = |
| navy | #311B92 | = |
| olive | #808000 | = |
| orange | #FF9800 | = |
| purple | #9C27B0 | = |
| red | #F23645 | #FF5252 |
| silver | #B2B5BE | = |
| teal | #089981 | #00897B |
| white | #FFFFFF | = |
| yellow | #FDD835 | #FFEB3B |

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json` and `meta.json`, on BINANCE:ETHUSDT.P 15. Each
probe opens positions on even bars and closes them on odd bars. Each exit's
comment spells what the script read on the bar before its fill
(`tests/exit_comment_tape.hpp`):

- `w11-color-v6-eth15` and its `//@version=5` twin `w11-color-v5-eth15`:
  - `n0` / `n1` / `n2`: `r,g,b,t` of the seventeen named constants, in the
    order above;
  - `t<k>:`: `color.t` of `color.new(color.red, k)`, `color.new(color.red,
    k + 0.5)`, `color.rgb(10, 20, 30, k)`, `color.new(#123456, k)` and
    `color.new(color.new(color.blue, 50), k)`, for `k = 0 .. 100`.
- `w11-color-alpha-eth15`: `a<j>:` holds `color.t` of the literals
  `#FF0000xx` for the alpha bytes `j .. j + 3`, all 256 of them.

`tests/test_color_tapes.cpp` replays every reading through
`include/pineforge/color.hpp` as generated code calls it.

| tape | range | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---:|---|---|
| `w11-color-v6-eth15` | 2025-04-01 .. 2025-04-04 | 576 | `a15d2aae0d9c4a4a234e9dd545fcee3b571076626515adfefd01d990096132a6` | `50c3b73997cf` |
| `w11-color-v5-eth15` | 2025-04-01 .. 2025-04-04 | 576 | `6173b30fbb1a0f21ed49b98b1678b51ff8badf500cc171ec0ae3eadc13e5688c` | `bf2fda0670eb` |
| `w11-color-alpha-eth15` | 2025-04-01 .. 2025-04-03 | 96 | `069af3719ba6d43bebcd7f5fb522ff1e7528e31beeb81a696bf481409b5636cf` | `e8f7f7810e99` |
