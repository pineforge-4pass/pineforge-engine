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

## Equity statistics (`metrics.equity`, all-trades only)

The equity curve samples `initial_capital + net_profit + open_profit` at
every **script-bar close**, timestamped with the script-bar **open** time
(magnifier-invariant).

| Field | Definition | Undefined → NaN when |
| --- | --- | --- |
| `max_equity_drawdown` (+`_pct`) | Peak-to-trough equity drop, positive currency; pct vs the peak in effect | — (0 when flat) |
| `max_equity_runup` (+`_pct`) | Trough-to-peak rise, **trough resets on each new equity peak** (mirrors the engine's intra-run extremes); pct vs that trough | — |
| `buy_hold_return` (+`_pct`) | `initial_capital · (last_close/first_open − 1)` | first open non-finite or ≤ 0 |
| `sharpe_tv` / `sortino_tv` | Month-end-resampled equity simple returns (chart tz, open-time bucketing), risk-free 2 %/yr (2/12 per month), annualized ×√12. Sharpe: sample (N−1) stddev. Sortino: population downside deviation vs the monthly risk-free | < 2 monthly returns, or zero deviation |
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
| Equity statistics (max DD ±%, Sharpe/Sortino both variants, CAGR, Calmar, recovery) | quantstats 0.0.81 + empyrical-reloaded 0.5.12 (`scripts/crossvalidate_metrics.py --all`) | All 246 corpus strategies ran, 0 skipped, 0 mismatches; worst engine-convention \|rel Δ\| = 1.886e-11 (`pyramid-cash-fractional-commission-01`, sharpe/sortino_bar vs empyrical); 3 degenerate NaN fields (sharpe_tv, zero monthly variance) agree on degeneracy across engine/numpy/empyrical/quantstats; known library-convention deltas labelled in single-strategy mode |
| Closed-form unit oracles | `tests/test_metrics.cpp` (e.g. monthly Sharpe 19/20, Sortino 114/61 exact rationals) | Bit-level |

Known open deltas: TradingView's "Max run-up (close-to-close)" uses a
different run-up definition than the engine's trough-reset semantics; TV's
own Sharpe/Sortino/max-DD panel values have not yet been exported for
comparison (engine values are library-validated meanwhile). TV-only fields
not computed: outliers, average run-up/duration, intrabar excursion
variants.

## Consuming from Python

```python
report.metrics.all.profit_factor        # ctypes mirror, see FFI page
report.metrics.equity.sharpe_tv
curve = report.equity_curve[:report.equity_curve_len]
```

Mirror classes and the mandatory `pf_abi_version()` guard: @ref ffi_python.
