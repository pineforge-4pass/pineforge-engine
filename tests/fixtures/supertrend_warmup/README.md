# ta.supertrend from the first bar, as TradingView computes it (lane W11-ENG-TIME-COLOR)

TradingView runs Pine v6's reference implementation of `ta.supertrend` from
the run's first bar:

    upperBand/lowerBand = hl2 +/- factor * ta.atr(atrPeriod)   // na while the ATR warms up
    lowerBand := lowerBand > nz(lowerBand[1]) or close[1] < nz(lowerBand[1]) ? lowerBand : nz(lowerBand[1])
    upperBand := upperBand < nz(upperBand[1]) or close[1] > nz(upperBand[1]) ? upperBand : nz(upperBand[1])
    direction = na(atr[1]) ? 1 : superTrend[1] == nz(upperBand[1]) ? (close > upperBand ? -1 : 1)
                                                                   : (close < lowerBand ? 1 : -1)
    superTrend := direction == -1 ? lowerBand : upperBand

A comparison with na is false. With no `close[1]` on bar 0, both bands keep
`nz`'s 0 there, so bar 0's line is 0. On bars 1 .. atrPeriod - 1 the line is
na. The direction is 1 until `atr[1]` is valid. `ta.supertrend(1000, 5)`,
whose lower band is negative, keeps that band at 0.

`w11-st-warmup-eth15` is one `lab tv --no-note` export (channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json` and `meta.json`. It ran on
BINANCE:ETHUSDT.P 15 from 2025-04-01 to 04-03. The probe alternates two
positions and closes one on every chart bar. Each exit comment spells
`line,direction` of `ta.supertrend(3, 10)`, `(2, 1)`, `(1000, 5)`, `(0.5, 3)`
and `(input 3.0, input 7)` on the bar before the fill, the line to two
decimals (`n` for na). ` @...` carries bar 0's reading, kept in a `var`.

`bars.inc` holds the chart's 192 bars. They are the corpus feed's own:
`corpus/data/ohlcv_ETH-USDT-USDT_1m.csv` folded to 15 minutes by the lane's
`gen_st_bars.py`. The reference computed on them reproduces all 192 readings
of the tape.

`tests/test_ta_voltrend_edge.cpp` (`test_supertrend_warmup_tape`) replays the
tape through `ta::Supertrend`, the class generated code builds for a literal
or input factor and period.

| tape | range | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---:|---|---|
| `w11-st-warmup-eth15` | 2025-04-01 .. 2025-04-03 | 192 | `8eb153be1c31e953b0d1709554e1a26260c8650866f582101bec592bbfa24b32` | `b9e825613281` |
