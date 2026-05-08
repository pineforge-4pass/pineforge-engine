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

Quick sanity check: with `magnifier_samples = 4` and
`PF_MAGNIFIER_ENDPOINTS`, expect roughly
`magnifier_sample_ticks_total ≈ 4 * input_bars_processed`.

## How it interacts with `request.security()`

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
