# Bar magnifier {#magnifier}

@tableofcontents

The bar magnifier sub-samples each parent bar into synthetic
intra-bar slices, allowing intra-bar order fills to resolve at finer
granularity than the source feed. It's the runtime's mechanism for
matching TradingView's "Bar Magnifier" feature.

## When to enable it

Enable the magnifier when:

- Your strategy uses **intra-bar stop / limit / take-profit** orders
  whose fills depend on the OHLC path inside the parent bar.
- You're parity-checking against a TV backtest that has Bar Magnifier
  on.

Skip it when:

- Orders are **bar-close-only** (`process_orders_on_close = true`).
- You're running purely on a 1m or sub-minute feed where no extra
  resolution exists to recover.

## Enabling

```c
run_backtest_full(s, bars, n,
                  /*input_tf */ "5",
                  /*script_tf*/ "60",
                  /*bar_magnifier  */ 1,
                  /*magnifier_samples*/ 4,
                  PF_MAGNIFIER_ENDPOINTS,
                  &report);
```

| Arg | Effect |
| --- | --- |
| `bar_magnifier` | Boolean toggle. |
| `magnifier_samples` | Sub-bar samples per parent bar. Typical: 4. Higher = finer fills, more CPU. |
| `magnifier_dist` | Sampling density profile — see below. |

Add volume-weighted sample density as a sticky toggle on the handle:

```c
strategy_set_magnifier_volume_weighted(s, 1);
```

## The intrabars a Pine strategy walks {#magnifier_intrabars}

When the input feed is finer than the script timeframe, a compiled Pine
strategy's magnifier does not walk the input bars: it walks the intrabars
TradingView's bar magnifier walks, which the Pine host builds from the input
(`tradingview_magnifier_bars` magnifier_intrabars.hpp:44). TradingView's help
centre ("What is bar magnifier backtesting mode") fixes the intrabar timeframe
per chart timeframe, each row covering the charts from it up to the next row
(`tradingview_intrabar_timeframe` magnifier_intrabars.hpp:19):

| Chart | Intrabar | Buildable from 1-minute bars |
| --- | --- | --- |
| 1 minute | 10 seconds | no — needs a 10-second feed |
| 5 minutes | 30 seconds | no — needs a 30-second feed |
| 10 minutes | 1 minute | yes (the feed itself) |
| 15 minutes | 2 minutes | yes |
| 30 minutes | 5 minutes | yes |
| 1 hour | 10 minutes | yes |
| 4 hours | 30 minutes | yes |
| 1 day | 1 hour | yes |
| 3 days | 4 hours | yes |
| 1 week and up | 1 day | yes |

Each intrabar is the symbol's regular bar of that timeframe — anchored at the
session day's open (00:00 UTC on a 24x7 symbol, 09:15 on NSE, 09:30 on NYSE)
and cut at the session's close — and belongs to the chart bar that holds its
**last** minute. On a 15-minute chart the 2-minute intrabar from 10:14 to 10:16
belongs to the 10:15 bar, so a chart bar's path is its own open, the intrabars
it owns (each an open, its nearer extreme, its other extreme, its close), then
its own close. The host marks the chart bar's open and close as one-price bars
that traded nothing where an intrabar straddles its first minute or leaves its
last one to the next bar, and the kernel walks such a bar as one point. The
rows from 1 minute to 1 day were measured against TradingView's own
`request.security_lower_tf` bars and magnified exports (R5 lane MAG-INTRABAR:
`tests/test_adapter_magnifier_intrabar_tapes.cpp`,
`tests/fixtures/magnifier_intrabars`).

The host walks the input bars themselves when it cannot build TradingView's:
an intrabar finer than the input or not a multiple of it (a 1- or 5-minute
chart on 1-minute bars), or a session it cannot resolve. A daily chart of an
exchange-listed stock is built from the minutes, but TradingView prices that
chart bar's open and close from its daily feed (the official open and close),
which a 1-minute feed does not carry.

## Distribution modes

@see #pf_magnifier_distribution_t for the enum.

| Mode | Density profile | Use case |
| --- | --- | --- |
| `PF_MAGNIFIER_UNIFORM` | Even spacing across the bar. | Symmetric noise; no prior on where fills cluster. |
| `PF_MAGNIFIER_COSINE` | Tapered ends, dense middle. | Smooth volatility profile. |
| `PF_MAGNIFIER_TRIANGLE` | Linear taper from a peak. | Single-peaked intraday activity. |
| `PF_MAGNIFIER_ENDPOINTS` *(default)* | Exact O,H,L,C points + uniform fill between. | TV-parity default. Best for stop/limit fill realism. |
| `PF_MAGNIFIER_FRONT_LOADED` | Density biased toward bar open. | Open-driven assets (futures session opens). |
| `PF_MAGNIFIER_BACK_LOADED` | Density biased toward bar close. | Close-driven assets (equities rebalance). |

## Diagnostics

Every report carries two magnifier counters:

| Field | Meaning |
| --- | --- |
| `magnifier_sub_bars_total` | Synthetic intra-bar slices generated. |
| `magnifier_sample_ticks_total` | Sample ticks visited. |

The kernel's intrabar driver counts both for every run that walks an intrabar
path, so a bare C++ or C native host reports them as a compiled strategy does;
a bar that was assigned no lower-feed bars counts nothing. The Pine host
mirrors the same counts at its bar callbacks, and no state hash folds them.

Quick sanity check: with `magnifier_samples = 4` and
`PF_MAGNIFIER_ENDPOINTS`, expect four sample ticks per sub-bar and one per
one-price bar that traded nothing. A Pine strategy walks TradingView's
intrabars, not the input bars (above): a 15-minute chart on 1-minute bars
visits about `4 * 7.5 + 1` ticks per chart bar, not `4 * 15`.

## How it interacts with request.security()

A higher-timeframe feed brought in via `request.security()` follows the
**parent timeframe** of the security site, not the script TF. The
magnifier still runs on the script-TF parent bar; the security feed
delivers complete-bar values when the parent bar closes, partial-bar
values otherwise. See `pf_report_t::security_partial_total` to count
partial-bar evaluations.

## Reproducibility

The magnifier is **deterministic** for a given (`bars`,
`magnifier_samples`, `magnifier_dist`, `volume_weighted`) tuple. Two
runs with identical inputs produce bit-identical trade lists. There is
no internal RNG seeded from time or address.
