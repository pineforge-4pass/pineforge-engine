# Multi-Timeframe (MTF) {#mtf}

@tableofcontents

PineForge exposes two distinct multi-timeframe surfaces. They look
similar from Pine but resolve through completely different runtime
paths, and they have different rules:

| Surface | Pine call | Direction | Bars source |
| --- | --- | --- | --- |
| **Upward (HTF)** | `request.security(sym, "60", expr)` | target TF **coarser** than input | aggregated from input bars |
| **Downward (LTF)** | `request.security_lower_tf(sym, "1", expr)` | target TF **finer** than input | **synthesized** from each input bar's OHLC path |
| **Another symbol** | `request.security("TVC:DXY", "15", expr)` | any TF | **the other symbol's own bars**, installed before the run |

The downward path is where PineForge diverges from TradingView. TV
downloads a separate finer-resolution feed when you switch to a lower
timeframe; PF does **not**. The input feed's resolution is the upper
bound on what `request.security_lower_tf` can target — beyond that, no
data exists. Sub-bars are synthesized on demand by walking the input
bar's OHLC path. See @ref magnifier for the same idea applied to fill
resolution.

For the upward path and the chart-aggregation rules, see also
@ref timeframes.

## Another symbol's bars

A `request.security` of **another** symbol reads that symbol's own bars; no
aggregation of the chart can produce them. The host installs them before the
run, keyed by the exact symbol string the script passes and by the timeframe,
each bar with its own close:

```c
strategy_set_symbol_feed(s, "TVC:DXY", "15", bars, close_ms, n);   /* PINEFORGE_HAS_SYMBOL_FEED_V1 */
strategy_set_symbol_facts(s, "TVC:DXY", "type", "index");           /* PINEFORGE_HAS_SYMBOL_FACTS_V1 */
```

On each chart bar the site sees the **last bar of the other symbol that has
closed by the chart bar's close** (`lookahead_off`) or **that opened by the
chart bar's open** (`lookahead_on`); no calendar is guessed for the other
symbol, so a daily bar that closes after the chart's session close is seen one
chart day late, as TradingView sees it. The site's expression runs on **every**
bar of the other symbol, in order, so its history offsets and indicators run
over that symbol's own bars; inside it `bar_index`, `time_close` and
`syminfo.*` are the other symbol's. `gaps_on` reads na on a chart bar that
received no new bar. A symbol with no installed feed fails the run closed,
naming the symbol and the timeframe; an invalid one reads na under
`ignore_invalid_symbol`. Historical runs only: a stream refuses an installed
symbol feed. The merge itself is the kernel's (`NativeRunSpec::instrument_feeds`,
@ref native_engine) and holds for a native host too.

`scripts/run_strategy.py` installs a probe's pinned feeds, facts and recorded
request values (`strategy_set_recorded_series`, TradingView's per-chart-bar
fundamentals) from `PINEFORGE_REQUESTS_ROOT/<probe>/requests.json` when that
variable is set. Codegen's lowering of a foreign `request.security` onto this
surface is a separate lane; until it lands, codegen still refuses a symbol that
is not the chart's.

## The script_tf / input_tf model

Every backtest run takes two timeframe strings on
`run_backtest_full(...)`:

```c
run_backtest_full(s, bars, n,
                  /* input_tf  */ "",   // empty → auto-detect from bar timestamps
                  /* script_tf */ "",   // empty → defaults to input_tf
                  /* magnifier */ 0, 4, PF_MAGNIFIER_ENDPOINTS,
                  &report);
```

The runtime resolves them in order:

1. `input_tf` — what the bar feed actually is. If empty, the runtime
   computes the median delta between consecutive `pf_bar_t::timestamp`
   values and snaps to a canonical Pine TF.
2. `script_tf` — what the strategy script believes it's running on.
   If empty, defaults to the resolved `input_tf`.
3. If `script_tf > input_tf`, the runtime aggregates input bars up to
   script-TF parents before each `on_bar` dispatch. The two timeframes
   are concatenated by an aggregator (ratio or calendar —
   `TimeframeAggregator`, `src/timeframe.cpp`).

The report exposes the resolved values and the ratio:

| Report field | Meaning |
| --- | --- |
| `input_tf_seconds` | Resolved input TF (after auto-detect). |
| `script_tf_seconds` | Resolved script TF (after defaulting). |
| `script_tf_ratio` | `script_tf_seconds / input_tf_seconds`. |
| `needs_aggregation` | `1` when ratio > 1, `0` otherwise. |

## Switching timeframes — C

```c
pf_report_t r = {0};

// 1. Both empty → auto-detect input, default script to input.
run_backtest_full(s, bars, n, "", "", 0, 4, PF_MAGNIFIER_ENDPOINTS, &r);
//    r.input_tf_seconds  == 900   (15m, auto-detected)
//    r.script_tf_seconds == 900   (defaulted)
//    r.script_tf_ratio   == 1
//    r.needs_aggregation == 0

// 2. Explicit input + explicit higher script → 4:1 aggregation.
run_backtest_full(s, bars, n, "15", "60", 0, 4, PF_MAGNIFIER_ENDPOINTS, &r);
//    r.input_tf_seconds  == 900
//    r.script_tf_seconds == 3600
//    r.script_tf_ratio   == 4
//    r.needs_aggregation == 1

// 3. Explicit input, script defaults to input.
run_backtest_full(s, bars, n, "15", "", 0, 4, PF_MAGNIFIER_ENDPOINTS, &r);
//    r.input_tf_seconds  == 900
//    r.script_tf_seconds == 900   (defaulted, NOT inferred separately)
```

## Switching timeframes — Python (ctypes)

```python
# Same three calls, byte-string TFs.
lib.run_backtest_full(s, bars, n, b"",   b"",   0, 4, 3, byref(r))
lib.run_backtest_full(s, bars, n, b"15", b"60", 0, 4, 3, byref(r))
lib.run_backtest_full(s, bars, n, b"15", b"",   0, 4, 3, byref(r))
```

The full runnable harness — three tables walking the script_tf sweep,
the input_tf/script_tf pair matrix, and the lower-TF synthesis ratio
— is in `tutorial/run_mtf.py`. See the **Worked example** section
below.

## request.security_lower_tf — the codegen contract

The lower-TF builtin returns an array of one value per synthesized
sub-bar. The codegen pattern in `generated.cpp` is:

```cpp
class GeneratedStrategy : public pineforge::source::PineStrategyHost {
public:
    // Accumulator vector for the sub-bar values.
    std::vector<double> _req_sec_lower_tf_0{};

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        // sec_id 0, target "1" minute, input_tf passed through from the
        // run-time resolved value.
        register_security_lower_tf_eval(0, std::string("1"), input_tf_);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        // Sub-bar 0 marks the start of a new chart bar — clear before pushing.
        if (security_lower_tf_sub_bar_index(0) == 0) {
            _req_sec_lower_tf_0.clear();
        }
        _req_sec_lower_tf_0.push_back(bar.close);
    }

    void on_source_bar(const Bar& bar) override {
        // _req_sec_lower_tf_0 is the Pine array returned by
        // request.security_lower_tf(sym, "1", close).
        // ... compute on it ...
    }
};
```

`register_security_eval`, `register_security_lower_tf_eval`,
`security_lower_tf_sub_bar_index` and `security_series_slot_is_new` are the
Pine source host's (`source::PineStrategyHost`,
`src/source/pine_security_eval.cpp`). The kernel underneath registers a plain
aggregating evaluator and keeps the feed store; every `request.security`
rule on this page — lookahead and gaps publication, Heikin-Ashi, the
lower-timeframe emulation, the diagnostics below — is source-host state kept
per `sec_id` beside it.

### Validation rules

When the run begins, the source host validates each registered lower-TF
site against the run's evaluator input timeframe
(`validate_security_timeframes`):

- The target TF must be **strictly finer** than the resolved input TF.
- The input TF (in seconds) must be an **integer multiple** of the
  target TF — non-clean ratios are rejected.
- `lookahead` and `gaps` must be off (TV does not expose them on this
  builtin and PF refuses to fake them).

Violations raise at run-time with a precise diagnostic, e.g.:

```
request.security_lower_tf requires a timeframe finer than the
chart's input timeframe: requested 30 from input timeframe 15
```

### Sub-bar synthesis

For each input bar, the engine generates `input_tf / target_tf`
synthetic sub-bars by walking the input bar's OHLC endpoints (the
same path-sampling primitive the bar magnifier uses, see
`src/engine_lower_tf.cpp`). Each synthetic sub-bar gets `1/ratio` of
the input bar's volume.

Per-run sub-bar counts surface in the report's `security_feeds_total`
field. With a 15m chart and a `"1"` lower-TF target, expect:

```
security_feeds_total == 15 * input_bars_processed
```

## Worked example

`tutorial/mtf/` ships two strategies side-by-side:

| File | Demonstrates |
| --- | --- |
| `tutorial/mtf/strategy_htf.pine` + `generated_htf.cpp` | `request.security` (HTF SMA filter on a 15m chart). HTF is an input — sweepable without rebuild. |
| `tutorial/mtf/strategy_ltf.pine` + `generated_ltf.cpp` | `request.security_lower_tf("1", close)` — intra-bar 1m range as an entry signal. |
| `tutorial/run_mtf.py` | Three tables: script_tf sweep, input_tf/script_tf pair matrix, lower-TF synthesis ratio. |

Build:

```bash
cmake --build build --target strategy_tutorial_mtf_htf strategy_tutorial_mtf_ltf -j
python3 tutorial/run_mtf.py
```

Expected (excerpt):

```
Table A — script_tf sweep, fixed input (HTF .so)
 script_tf  in_s  sc_s ratio agg?  in_bars  sc_bars  trades    net_pnl
        ""   900   900     1    0      672      672       1    -653.46
      "15"   900   900     1    0      672      672       1    -653.46
      "60"   900  3600     4    1      672      168       6    -962.57
     "240"   900 14400    16    1      672       42       0      +0.00
```

Numbers depend on the OHLCV snapshot.

## See also

- @ref timeframes — TF strings, auto-detection, calendar vs ratio aggregation.
- @ref magnifier — sub-bar synthesis for fill resolution (same primitive).
- @ref tutorial_macd — single-cadence baseline this tutorial extends.
