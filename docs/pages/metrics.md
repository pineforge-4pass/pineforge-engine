# Trading metrics reference {#metrics}

@tableofcontents

Every backtest report (`pf_report_t::metrics`, ABI v2) carries a computed
trading-metrics suite plus the raw per-script-bar equity curve. This page is
the complete metric list with definitions, units, NaN rules, and validation
status. The per-field doxygen in `<pineforge/pineforge.h>` is the canonical
contract; this page is the readable index of it.

@see @ref report_schema for where the blocks sit inside `pf_report_t`.

## Blocks

| Block | Type | Scope |
| --- | --- | --- |
| `metrics.all` | #pf_trade_stats_t | All closed trades |
| `metrics.longs` | #pf_trade_stats_t | Long trades only |
| `metrics.shorts` | #pf_trade_stats_t | Short trades only |
| `metrics.equity` | #pf_equity_stats_t | Equity-curve stats (all-trades only, like TradingView) |
| `equity_curve[]` | #pf_equity_point_t | One point per script bar: `{time_ms, equity, open_profit}` |

## Trade statistics (per block: All / Long / Short)

Conventions: loss-side currency fields are **positive magnitudes**; percent
fields are on the 0–100 scale; per-trade `%` derives from
`pf_trade_t::pnl_pct` = **net return-on-cost** (`net pnl / (entry_price ×
qty × pointvalue) × 100`, TradingView-arbitrated).

| Field | Definition | Undefined → NaN when |
| --- | --- | --- |
| `num_trades` | Closed trades in the block | — |
| `num_wins` / `num_losses` | Trades with `pnl > 0` / `pnl < 0` | — |
| `num_even` | Trades with `pnl == 0.0` exactly; break both streaks, excluded from win/loss averages | — |
| `percent_profitable` | `100 · num_wins / num_trades` | no trades |
| `net_profit` | Σ pnl (account currency, net of commission) | — |
| `net_profit_pct` | `net_profit / initial_capital · 100` | capital ≤ 0 |
| `gross_profit` (+`_pct`) | Σ winning pnl (pct vs initial capital) | capital ≤ 0 (pct) |
| `gross_loss` (+`_pct`) | Σ \|losing pnl\| — positive magnitude | capital ≤ 0 (pct) |
| `profit_factor` | `gross_profit / gross_loss` | zero gross loss |
| `avg_trade` (+`_pct`) | `net_profit / num_trades`; pct = mean `pnl_pct` | no trades |
| `avg_win` (+`_pct`) | `gross_profit / num_wins`; pct = mean win `pnl_pct` | no wins |
| `avg_loss` (+`_pct`) | `gross_loss / num_losses` (positive); pct = mean negated loss `pnl_pct` | no losses |
| `ratio_avg_win_avg_loss` | `avg_win / avg_loss` | either side empty |
| `largest_win` (+`_pct`) | Largest win in **currency**; `_pct` = **independent max** of per-trade % (TV convention — may come from a different trade) | no wins |
| `largest_loss` (+`_pct`) | Same, loss side, positive magnitudes | no losses |
| `commission_paid` | Σ `pf_trade_t::commission` (captured at close from actual deductions) | — |
| `expectancy` | `(num_wins/n)·avg_win − (num_losses/n)·avg_loss`, currency per trade | no trades |
| `max_consecutive_wins` / `_losses` | Longest run; even trades reset both | — |
| `avg_bars_in_trade` / `_wins` / `_losses` | Mean of `exit_bar − entry_bar + 1` (**inclusive** of the entry bar, TV convention), script bars | empty set |

Native settlement records fees in account currency at the event where they are
incurred. Entry fees are snapshotted on each physical lot at its entry fill
and remain unchanged when a later FX point becomes active. A partial FIFO
close allocates the paid entry fee in proportion to closed quantity; the
survivor retains the unconsumed cost. A
single cash-per-order ticket belongs to the native execution as a whole and is
allocated proportionally across its physical close/open effects; it is not
charged once per trade row. The shared native close helper now consumes these
historical paid costs. Existing legacy Pine paths are not all grouped into
native executions, so this rule does not claim that every legacy path has the
same dynamic-FX settlement semantics. Pine sizing, source-specific reporting,
and range-end conventions remain compatibility behavior.

## Equity statistics (`metrics.equity`, all-trades only)

The equity curve samples `initial_capital + net_profit + open_profit` at
every **script-bar close**, timestamped with the script-bar **open** time
(magnifier-invariant).

| Field | Definition | Undefined → NaN when |
| --- | --- | --- |
| `max_equity_drawdown` (+`_pct`) | Peak-to-trough equity drop, positive currency; pct vs the peak in effect | — (0 when flat) |
| `max_equity_runup` (+`_pct`) | Trough-to-peak rise, **trough resets on each new equity peak** (mirrors the engine's intra-run extremes); pct vs that trough | — |
| `buy_hold_return` (+`_pct`) | `initial_capital · (last_close/first_open − 1)` | first open non-finite or ≤ 0 |
| `sharpe_monthly` / `sortino_monthly` | Month-end-resampled equity simple returns (chart tz, open-time bucketing), risk-free 2 %/yr (2/12 per month), annualized ×√12. Sharpe: sample (N−1) stddev. Sortino: population downside deviation vs the monthly risk-free. The pre-1.0 C spelling `sharpe_tv` / `sortino_tv` (the same `double` at the same offset) was removed for 1.0; the **serialized report key stays `sharpe_tv` / `sortino_tv`** (see @ref report_schema) | < 2 monthly returns, or zero deviation |
| `sharpe_bar` / `sortino_bar` | Same construction over per-script-bar returns, annualized by **observed bar density** (`bars/yr = (len−1)/calendar span`) — not a fixed calendar formula | < 2 returns, or zero deviation |
| `cagr` | `100 · ((final_equity/initial_capital)^(1/years) − 1)`, calendar span | span ≤ 0, or either side ≤ 0 |
| `calmar` | `cagr / max_equity_drawdown_pct` (both percent → dimensionless) | zero drawdown |
| `recovery_factor` | `net_profit / max_equity_drawdown` | zero drawdown |
| `time_in_market_pct` | `100 ·` script bars with an open position at close `/ total script bars` | empty curve |
| `open_pl` | Mark-to-market open profit at the final bar | — |

@note Metrics are only meaningful when `strategy_get_last_error()` returns
an empty string: `run()` captures exceptions, so a failed run yields a
truncated curve and metrics over the truncated prefix.

## Validation status

| Surface | Validated against | Result |
| --- | --- | --- |
| Trade statistics (counts, PF, percent bases, averages, largest-%, bars) | Real TradingView Strategy Tester export (`composite-4emarsi-integration-01`, 336 trades, All/Long/Short panels) | Match within TV 2-dp rounding; three TV conventions arbitrated and adopted (net return-on-cost `pnl_pct`, independent largest-%, inclusive bar counts) |
| Commission + slippage economics | Historical TradingView exports: 0.1% commission + 2 ticks via `strategy_set_override` ([archived inputs](https://github.com/pineforge-4pass/pineforge-engine/blob/a03ac6d3fb42df5af1db9e39727daf450b2fb71f/validation-adhoc/4emarsi-commission-slippage-ethusdt/inputs.json)) | Historical fixed-export validation only: 672 fill prices bit-exact and 335/336 commission rows reproduced to the cent after the documented USDT→USD conversion. A separate historical bracket export reproduced 396/396 trades and pinned limit/stop slippage rules. These archived receipts do not claim current dynamic-FX native settlement parity; current native settlement preserves paid entry costs and allocates one execution's costs across physical effects. |
| TV risk panel (Sharpe, Sortino, drawdown/run-up rows, CAGR) | TV xlsx export (Performance + Risk-adjusted performance sheets) | Every panel value reproduced from the engine curve once TV's conventions are applied — see the definition-delta table below |
| Equity statistics (max DD ±%, Sharpe/Sortino both variants, CAGR, Calmar, recovery) | quantstats 0.0.81 + empyrical-reloaded 0.5.12 (`scripts/crossvalidate_metrics.py --all`) | All 246 corpus strategies ran, 0 skipped, 0 mismatches; worst engine-convention \|rel Δ\| = 1.886e-11 (`pyramid-cash-fractional-commission-01`, sharpe/sortino_bar vs empyrical); 3 degenerate NaN fields (`sharpe_monthly`, zero monthly variance) agree on degeneracy across engine/numpy/empyrical/quantstats; known library-convention deltas labelled in single-strategy mode |
| Closed-form unit oracles | `tests/test_metrics.cpp` (e.g. monthly Sharpe 19/20, Sortino 114/61 exact rationals) | Bit-level |

### TV risk-panel definition deltas (arbitrated 2026-06-12, all reproduced)

The engine's fields are NOT wrong where they differ from TV's panel — TV
uses different constructions. Each was reverse-engineered and reproduced
exactly from the engine curve:

| TV panel row | TV's actual construction | Engine field & difference |
| --- | --- | --- |
| Sharpe ratio | Monthly returns of **realized** equity (open profit excluded), account-currency, UTC months, rf 2 %/12, **population** stddev, **not annualized** | `sharpe_monthly` (report key `sharpe_tv`): mark-to-market equity, chart-tz months, sample stddev, ×√12 |
| Sortino ratio | Same series, population downside dev vs rf, not annualized | `sortino_monthly` (report key `sortino_tv`) = TV × √12 (same convention otherwise — matches to 4 decimals after de-annualizing) |
| Max/avg drawdown & run-up "(close-to-close)" | Realized equity sampled at **trade exits only**, split into alternating phases at the global max/min; phase value = endpoint-to-endpoint change (durations in days corroborate) | `max_equity_drawdown/runup`: per-script-bar curve with trough-reset walk |
| Max DD / run-up "(intrabar)" | Settled realized curve (with entry-commission dips) vs per-trade excursion extremes | closest to the engine's per-bar walk; reproduced exactly from trade MFE/MAE |
| CAGR | Net over the **configured backtesting range** day count, 365-day year | `cagr`: traded calendar span, 365.25 |
| All currency rows | Converted to **account currency** at previous-UTC-day close of the quote-currency pair | engine reports symbol currency (USDT here) |

TV-only fields not computed by the engine: outliers, run-up/drawdown
durations, intrabar excursion variants, account-size/margin rows. The retired research scripts and overrides are preserved in
[Git history](https://github.com/pineforge-4pass/pineforge-engine/tree/a03ac6d3fb42df5af1db9e39727daf450b2fb71f/validation-adhoc/4emarsi-commission-slippage-ethusdt).

## Consuming from Python

```python
report.metrics.all.profit_factor        # ctypes mirror, see FFI page
report.metrics.equity.sharpe_monthly    # the C field name; the serialized
                                        # JSON key of this field is sharpe_tv
curve = report.equity_curve[:report.equity_curve_len]
```

Mirror classes and the mandatory `pf_abi_version()` guard: @ref ffi_python.
