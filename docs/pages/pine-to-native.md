# PineScript to native C++ {#pine_to_native}

@tableofcontents

This page is for someone who already writes PineScript strategies and wants to
write the same strategy directly against the kernel — in C++ through
`pineforge::NativeStrategyHost`, or in C through the `strategy_native_*`
surface. No PineScript source, no codegen, no `src/source`, no
`src/compat/pine`.

Read **[Native engine](@ref native_engine)** first: it is the reference for the
lifecycle, the run spec, the request vocabulary and the C ABI contract. This
page is the translation layer on top of it — what each Pine strategy concept
becomes in C++ **and** in C, which runnable host demonstrates it, and where a
concept deliberately has no native counterpart.

## Coverage {#pine_to_native_coverage}

The 19 `strategy()` declaration parameters listed below, every `strategy.*` builtin, every
`request.*` form and every `barmerge.*` constant has a row below. Other Pine declaration
arguments are source or display policy; the inventory crosswalk below records their
absence instead of claiming a native parameter. The offered
set is not this page's opinion: the `strategy.*`, `request.*` and `barmerge.*`
rows come from the repository's own Pine v6 inventory,
`docs/pine_v6_coverage_detail.md`, and the
declaration parameters are the named arguments of the `strategy()` call, which
that inventory collapses into a single row.

| group | offered | covered | of which native | of which ruled *none* |
| --- | ---: | ---: | ---: | ---: |
| `strategy()` declaration parameters | 19 | 19 | 14 | 5 |
| `strategy()` and the sizing constants | 7 | 7 | 6 | 1 |
| order commands and their constants | 16 | 16 | 16 | 0 |
| `strategy.risk.*` | 6 | 6 | 6 | 0 |
| account and report variables | 33 | 33 | 31 | 2 |
| `strategy.opentrades.*` | 15 | 15 | 15 | 0 |
| `strategy.closedtrades.*` | 20 | 20 | 19 | 1 |
| `request.*` and `barmerge.*` | 15 | 15 | 6 | 9 |
| **total** | **131** | **131** | **113** | **18** |

`offered == covered`: a *ruled none* row is covered, not missing. It says the
kernel has no counterpart, names the reason, and names where the behaviour
lives instead — in the Pine adapter, in codegen, or in the host's own code.

Recount it yourself. The guard derives the offered set from the inventory and
the covered set from this page's own first table column, and fails when they
disagree:

```bash
python3 scripts/check_pine_to_native_coverage.py          # exit 0, prints the table above
python3 scripts/check_pine_to_native_coverage.py --list    # every token, with its verdict
```

## Why native {#pine_to_native_why}

A Pine strategy runs through two layers: the **adapter** (`src/source`,
`src/compat/pine`), which reproduces TradingView semantics down to its
rounding quirks, and the **kernel**, which matches triggers, prices fills,
books lots and settles. A native host talks to the kernel directly. What you
gain:

- **Your own decision model.** `on_native_bar` is an ordinary C++ member
  function (native_host.hpp:874). Any data structure, any library, any
  precomputation — no Pine type system, no series-of-everything.
- **Explicit setup.** One `NativeRunSpec` (native_run_spec.hpp:552) names the
  clock, instrument, account, fees and caps. Nothing is inferred from the bars
  and nothing comes from a chart.
- **Typed orders and typed outcomes.** A request is a value
  (`Request` native_order.hpp:483); its fate is a typed event — `AcceptedEvent`
  (native_order.hpp:981), `ExecutionAppliedEvent` (native_order.hpp:1120),
  `MatchRejectedEvent` (native_order.hpp:1082), `CancelledEvent`
  (native_order.hpp:1023) — readable in order from `native_events`
  (native_host.hpp:1294).
- **Any language.** The same kernel drives from C through a callback table
  (`pf_native_callbacks_v1` native_c_api.h:2371), so a host in Rust, Go, Python or Zig needs no C++.
- **One binary, no transpiler.** Link `libpineforge`, ship an executable or a
  loadable module.

What you give up: TradingView parity. The adapter exists because TV's
behaviour is not always the generic behaviour — money rounding, half-tick
trigger thresholds, same-bar command batching, margin-call sizing. Those rows
stay in the adapter by design; §1 of `native-feature-parity.md:42` marks every
one of them, and a native host reproduces the *generic* rule, not TV's.

## How to read a row {#pine_to_native_how_to_read}

Each map below has five columns.

| column | what it holds |
| --- | --- |
| Pine | the spelling TradingView offers, exactly as the inventory spells it |
| C++ | the symbol a `NativeStrategyHost` uses, with its `file:line` |
| C | the same thing through `<pineforge/native_c_api.h>` or `<pineforge/pineforge.h>`, or `—` when the C surface has none |
| Runs in | a host under `examples/native/` that exercises it, or the test that pins it |
| Notes | the behaviour difference a Pine author would otherwise guess wrong |

A `none` in the C++ column is a **ruling**, never a gap: it names why the
kernel does not have the concept and where the behaviour lives. The two
normative ruling tables are ADR-0001's "Kernel capabilities the Pine adapter
does not declare" and the design record's §3.6
(`native-feature-parity.md:411`); both are held against the tree by
`scripts/check_native_feature_rulings.py`.

Every example named here is a CTest row: `ctest --test-dir build -R example_`
runs all of them, and each fails on a nonzero exit, a signal, a timeout or a
missing summary line (examples/native/run_example.cmake:1).

@anchor pine_to_native_map
## The `strategy()` declaration {#pine_to_native_map_declaration}

A Pine script declares its broker once, in the `strategy()` call, and inherits
the rest from the chart. A native host declares *everything* in one
`NativeRunSpec` (native_run_spec.hpp:552) applied by `configure_native`
(native_host.hpp:1161); a C host sends `pf_native_run_spec_v1`
(pineforge.h:524) plus `pf_native_run_spec_ext_v1` (`sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2207)
through `strategy_configure_native_ext_v1` (`sha256:6cf2f65aea67c6054b256e3cf27126d706f60ed1d8b561396aa3f65c3a0389b4` native_c_api.h:3010). Setup is
atomic: `Ready` → begin → `Completed`, and an incomplete value is refused
rather than defaulted.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy()` | `NativeRunSpec` native_run_spec.hpp:552 | `pf_native_run_spec_v1` pineforge.h:524 | `hello_kernel.cpp` / `hello_kernel_c.c` | One atomic setup value. `validate_native_run_spec` native_run_spec.hpp:797 names the offending field before a run begins. |
| `initial_capital` | `initial_capital` native_run_spec.hpp:578 | `initial_capital` pineforge.h:515 | `hello_kernel.cpp` | Required, positive. |
| `currency` | `currency` native_run_spec.hpp:569 | `currency` pineforge.h:513 | `hello_kernel.cpp` | With `ticker` native_run_spec.hpp:566, `tickerid` native_run_spec.hpp:567, `type` native_run_spec.hpp:568, `basecurrency` native_run_spec.hpp:570, `description` native_run_spec.hpp:571 and `volumetype` native_run_spec.hpp:572 this is Pine's whole `syminfo` block, named rather than inherited from a chart. |
| `commission_type` | `NativeFeeKind` native_run_spec.hpp:19 | `fee_kind` pineforge.h:516 | `native_open_lots_strategy.cpp` | `Percent`, `CashPerUnit`, `CashPerExecution` — the same three TradingView offers. |
| `commission_value` | `fee_value` native_run_spec.hpp:588 | `fee_value` pineforge.h:516 | `native_open_lots_strategy.cpp` | Quoted per execution by `quote_execution_commissions` engine_execution.cpp:1431. A cash-per-execution ticket is split by units over every slice of that execution. |
| `slippage` | `slippage_ticks` native_run_spec.hpp:582 | `slippage_ticks` pineforge.h:516 | `native_price_grid_strategy.cpp` | Raw `ticks * price_tick`, applied once native_execution_consumer.cpp:3894-3895. TradingView's round-then-slip order is adapter-only. |
| `process_orders_on_close` | `NativeCloseExecution` native_run_spec.hpp:33 | `close_execution` pineforge.h:517 | `native_open_lots_strategy.cpp` | `AfterCalculation` emits the post-calculation close point native_execution_consumer.cpp:7328-7329, so a request submitted in bar *k*'s calculation fills at bar *k*'s own close. |
| `pyramiding` | `max_open_lots` native_run_spec.hpp:594 | `max_open_lots` pineforge.h:519 | `native_open_lots_strategy.cpp` | Caps surviving+new physical lots `sha256:87bd0ff848901478840870d95eb356b35d7d368555ce8eadee8571b8e17f86ec` native_execution_consumer.cpp:2090-2093. Pine's per-cycle *entry count* is a TradingView rule, ruled **adapter-policy** in ADR-0001's table row `max_open_lots`. |
| `default_qty_type` | `Sized` native_order.hpp:164 with `CashValue` native_order.hpp:91 or `EquityFraction` native_order.hpp:97 | `PF_NATIVE_INTENT_SIZED` native_c_api.h:336 with `size_basis` `sha256:1ce14256cf90fd6381830a90e277237826c4c5b242b2cedb5096ad3a1341997d` native_c_api.h:2045 | `native_sized_report_strategy.cpp` | The size is a property of the *request*, not of the run: a native host may size one order by cash and the next by equity fraction. `Transact` native_order.hpp:41 is the fixed-units form. |
| `default_qty_value` | the basis's own scalar: `CashValue::cash` native_order.hpp:92, `EquityFraction::fraction` native_order.hpp:98 | `intent_value` `sha256:9480c2941ab8b1c7dacb27fe8c2dbb39b543039f653dd4418651cb80ae05f274` native_c_api.h:2049 | `native_sized_report_strategy.cpp` | `SizeTime` native_order.hpp:106 chooses *when* the basis resolves and `SizePrice` native_order.hpp:146 *which* price it converts at; both compose. |
| `calc_on_order_fills` | `NativeCalculationTrigger` native_run_spec.hpp:93 | `calculation` native_c_api.h:2211 | `native_calc_on_fills_strategy.cpp` | `BarCloseAndFills` recalculates once at the cursor of each applied execution, bounded by `max_recalculations_per_point` native_run_spec.hpp:625. The generic cadence only: TradingView's waypoint-only refill, its two-fills-at-open rule and its script-state rollback stay in the adapter. |
| `calc_on_every_tick` | `NativeCalculationTrigger` native_run_spec.hpp:93 | `calculation` native_c_api.h:2211 | `native_calc_on_fills_strategy.cpp` | `EveryModeledPoint` additionally recalculates at every modeled point of the delivered path and at every observed print. In a stream, `on_native_tick` native_host.hpp:859 is one call per accepted realtime print whatever the trigger is. |
| `margin_long` | `NativeMarginModel::initial_long` native_run_spec.hpp:240 | `margin_initial_long` native_c_api.h:2221 | `native_margin_strategy.cpp` | A fraction, not a percent. `0.0` is the maintenance-only spelling for that side: the kernel enforces no opening requirement there and the host owns opening admission. |
| `margin_short` | `NativeMarginModel::initial_short` native_run_spec.hpp:241 | `margin_initial_short` `sha256:0d4ddc0ac52c46f3d37b64f3e2fc1b2a428c32e8b5c21ddccced80fdf2c75346` native_c_api.h:2232 | `native_margin_strategy.cpp` | Per side, unlike the one-scalar `initial_margin_fraction` native_run_spec.hpp:596 the model replaces. `maintenance_long` native_run_spec.hpp:242 / `maintenance_short` native_run_spec.hpp:243 add the liquidation half Pine has no word for. |
| `close_entries_rule` | none — a native close names the lots it closes | — | `native_selected_strategy.cpp` | Pine picks FIFO or ANY for the whole script. A native reduce is explicit instead: `BindOpening` native_order.hpp:445 closes one opening's lots, `BindOpenings` native_order.hpp:454 a chosen set, a bare `Reduce` native_order.hpp:220 the book in FIFO order. The adapter keeps the run-level rule (`close_entries_rule` pine_adapter.hpp:88). |
| `backtest_fill_limits_assumption` | none — a generic limit fills when the level is reached | — | `tests/test_native_order_core.cpp` | TradingView's assumption is "the price must exceed the limit by *n* ticks". The generic knob beside it is `Limit::fill_through` native_order.hpp:239, which makes a limit market-if-touched; the *n*-tick assumption itself is a TradingView rule and stays in the adapter. |
| `use_bar_magnifier` | `IntrabarPath` native_run_spec.hpp:377 | `intrabar_kind` `sha256:2d7419a40ec33d3ae069d80c3e6ba5843c85b3671e0ff35eceb5dd1c14eb25cf` native_c_api.h:2271 | `tests/test_native_calc_timing.cpp` | The host owns the lower bars, as `lower_tf` native_run_spec.hpp:392 or `synthesized` native_run_spec.hpp:406. The C magnifier arguments of `run_backtest_full` pineforge.h:600 configure a *compiled Pine strategy*, never a native host. |
| `fill_orders_on_standard_ohlc` | none — the kernel matches the bars you feed it | — | — | TradingView's switch exists because a Heikin-Ashi chart's candles are not the traded prices. A native host feeds the standard OHLC it wants matched, and derives any transformed series itself. |
| `max_bars_back` | none — a native host owns its own history | — | `hello_kernel.cpp` | Pine infers a buffer length for every series. `pineforge::Series<T>` series.hpp:94 is a fixed-capacity ring you size yourself, so nothing is inferred and nothing silently truncates. |
| `risk_free_rate` | none — the ratio is a report field with TradingView's fixed rate | `sharpe_monthly` pineforge.h:303 | `native_sized_report_strategy.cpp` | `pf_metrics_t::equity.sharpe_monthly` and `pf_metrics_t::equity.sortino_monthly` `sha256:4936196cce01e508e5a42f2a582bb8bf14b5a4d059dcc89a24a5418ac52978d9` pineforge.h:343 use a fixed 2 %/yr, the TradingView default; nothing in the kernel takes a declared rate. `sharpe_tv` / `sortino_tv` are the deprecated spellings of the same two doubles (one anonymous union each, same offset), removed at the next `PF_ABI_VERSION`; the serialized report key stays `sharpe_tv`. A host that needs another rate computes it from `pf_report_t::equity_curve`. |

### The feed-shape and presentation policies Pine has no word for

These have no Pine spelling at all, so they carry no row above; they are here
because a host that ports a Pine strategy will meet them.
`NativeSlotLabelPolicy` native_run_spec.hpp:323 and `NativeFeedTolerance`
native_run_spec.hpp:344 admit a provider feed whose bar labels or values are
not canonical; `NativePathOrder` native_run_spec.hpp:334 forces the intrabar
O→H/L→C leg order for path-dependent probing; `NativeAbortReporting`
native_run_spec.hpp:41 makes a cooperative abort a quiet status instead of an
error. All four reach C through `PF_NATIVE_SPEC_EXT_FEED_POLICY`
native_c_api.h:733.


## Design §1 inventory crosswalk {#pine_to_native_inventory}

Every design inventory ID has one row here. Existing mappings point to the tables above;
source-only features state why a native counterpart does not exist.

| ID | mapping or truthful disposition |
| --- | --- |
| OL1 | Mapped by the migration table above. |
| OL2 | Mapped by the migration table above. |
| OL3 | Native `replace` and `cancel` are the generic live-order amendment and withdrawal operations described above. |
| OL4 | Mapped by the migration table above. |
| OL5 | Native `Capacity`, `PointBudget` and `Remaining` provide the generic partial-fill and residual-quantity contract. |
| OL6 | Mapped by the migration table above. |
| OL7 | Mapped by the migration table above. |
| OL8 | Mapped by the migration table above. |
| OL9 | Mapped by the migration table above. |
| OL10 | Mapped by the migration table above. |
| OL11 | Mapped by the migration table above. |
| OL12 | Mapped by the migration table above. |
| OL13 | No native counterpart: same-bar batching, deferred queues and priority are Pine adapter policy. |
| OL14 | No native counterpart: Pine exit activation and historical birth reach are source-language barriers. |
| TR1 | Mapped by the migration table above. |
| TR2 | Mapped by the migration table above. |
| TR3 | Mapped by the migration table above. |
| TR4 | Native `replace(..., ReplaceOptions{retain_trigger_state=true})` retains a trail extreme; Pine re-issue policy remains adapter-owned. |
| TR5 | Mapped by the migration table above. |
| SZ1 | Mapped by the migration table above. |
| SZ2 | Native counterpart: `resolve_execution_terms` / `HostSized` lets a host override resolved terms; Pine has no declaration parameter for this hook. |
| SZ3 | Mapped by the migration table above. |
| SZ4 | Mapped by the migration table above. |
| SZ5 | Mapped by the migration table above. |
| SZ6 | Mapped by the migration table above. |
| SZ7 | Native counterpart: `quantity_grid` is a generic run-spec quantity step; Pine exposes no equivalent declaration parameter. |
| SZ8 | Mapped by the migration table above. |
| SZ9 | Mapped by the migration table above. |
| SZ10 | No separate native counterpart: Pine money rounding is an adapter policy over the native sizing basis. |
| SZ11 | No separate native counterpart: Pine quantity floors are adapter policy; native `quantity_grid` is the generic knob. |
| SZ12 | No separate native counterpart: Pine reservation ledger details stay in the adapter over native scope claims. |
| MG1 | Mapped by the migration table above. |
| MG2 | Mapped by the migration table above. |
| MG3 | Mapped by the migration table above. |
| MG4 | Mapped by the migration table above. |
| MG5 | Mapped by the migration table above. |
| MG6 | Mapped by the migration table above. |
| MG7 | Mapped by the migration table above. |
| MG8 | Mapped by the migration table above. |
| MG9 | Mapped by the migration table above. |
| MG10 | Mapped by the migration table above. |
| MG11 | Mapped by the migration table above. |
| MG12 | Mapped by the migration table above. |
| MG13 | Mapped by the migration table above. |
| MG14 | Mapped by the migration table above. |
| MG15 | No native counterpart: TradingView margin-call money rounding and slice chronology are adapter policy. |
| MG16 | No native counterpart: the market-admission journal records source-layer review history, not generic execution state. |
| CT1 | Mapped by the migration table above. |
| CT2 | Mapped by the migration table above. |
| CT3 | Mapped by the migration table above. |
| CT4 | Mapped by the migration table above. |
| CT5 | Mapped by the migration table above. |
| CT6 | Mapped by the migration table above. |
| CT7 | Mapped by the migration table above. |
| CT8 | Mapped by the migration table above. |
| CT9 | Mapped by the migration table above. |
| CT10 | Mapped by the migration table above. |
| CT11 | Mapped by the migration table above. |
| HT1 | Mapped by the migration table above. |
| HT2 | Mapped by the migration table above. |
| HT3 | Mapped by the migration table above. |
| HT4 | Mapped by the migration table above. |
| HT5 | Mapped by the migration table above. |
| HT6 | No separate native counterpart: a host can use `TimeframeAggregator` as the documented self-aggregation recipe. |
| FP1 | Mapped by the migration table above. |
| FP2 | Mapped by the migration table above. |
| FP3 | Mapped by the migration table above. |
| FP4 | Mapped by the native price-grid examples and the `price_grid` discussion above. |
| FP5 | Mapped by the native price-grid examples and the `price_grid` discussion above. |
| PG | No native counterpart for the Pine per-order-kind grid waiver; the generic `NativePriceGrid` row is above. |
| FP6 | Mapped by the migration table above. |
| FP7 | Mapped by the migration table above. |
| RP1 | Mapped by the migration table above. |
| RP2 | Mapped by the migration table above. |
| RP3 | Mapped by `NativeReportPolicy::KernelRecorded` and `KernelRecordedAtHostMarks` in the report section. |
| RP4 | Mapped by the migration table above. |
| RP5 | Mapped by `report_open_position_at_end` and the shared range-end row producer above. |
| RP6 | Mapped by the migration table above. |
| RP7 | Native live order-action events exist in the stream API; there is no Pine builtin or separate migration target. |
| RP8 | Mapped by the continuation-hash and event-retention discussion in the native-engine reference. |
| RP9 | Native counterpart: broker-state hash recording is an engine/C ABI extension; Pine has no equivalent builtin. |
| RP10 | Native counterpart: `hash_host_extension` is the generic host-state hash seam; Pine has no equivalent builtin. |
| OT1 | No native counterpart: native hosts pass constructor/run parameters; Pine `input.*` is codegen/source state. |
| OT2 | Mapped by the migration table above. |
| OT3 | Mapped by `pineforge::ta`, `Series` and the indicator/history section above. |
| OT4 | Mapped by `request_abort` and `NativeAbortReporting` in the lifecycle section above. |
| OT5 | Mapped by the C configuration, stream and FX sections above. |
| OT6 | Mapped by the migration table above. |
| OT7 | Mapped by the session-day rows (`session.ismarket`, `session.isfirstbar`, `session.islastbar`) above. |
| OT8 | No native counterpart: live-tail and probe-suppress flags are Pine harness controls behind the source host. |

## Sizing constants and the default quantity {#pine_to_native_map_sizing}

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.fixed` | `Transact` native_order.hpp:41 | `PF_NATIVE_INTENT_TRANSACT` native_c_api.h:327 | `hello_kernel.cpp` | Signed units, the default native sizing: `> 0` long, `< 0` short. |
| `strategy.cash` | `CashValue` native_order.hpp:91 | `PF_NATIVE_SIZE_BASIS_CASH` native_c_api.h:360 | `native_sized_report_strategy.cpp` / `native_fee_reserve_strategy.cpp` | `units = cash / (price * point_value * fx)`. `Sized::reserve_percent_fee` native_order.hpp:179 divides the cash by `(1 + fee)` under `NativeFeeKind::Percent`, which is the fee reserve Pine has no spelling for. The fee-reserve example sizes 46 shares with it where 47 fit without, and asserts the fee each leg pays. |
| `strategy.percent_of_equity` | `EquityFraction` native_order.hpp:97 | `PF_NATIVE_SIZE_BASIS_EQUITY_FRACTION` native_c_api.h:361 | `native_sized_report_strategy.cpp` | A fraction of the marked equity at the sizing point: `0.10` is ten percent, never `10`. |
| `strategy.long` | `Side::Long` native_order.hpp:65 | `PF_NATIVE_SIDE_LONG` native_c_api.h:354 | `native_selected_strategy.cpp` | For `Transact` it is the sign of `signed_units` order_action.hpp:10; `Sized::side` native_order.hpp:165 and `HostSized::side` native_order.hpp:75 name it explicitly. |
| `strategy.short` | `Side::Short` native_order.hpp:65 | `PF_NATIVE_SIDE_SHORT` native_c_api.h:355 | `native_selected_strategy.cpp` | Same. |
| `strategy.default_entry_qty()` | none — the kernel has no declaration-level default quantity | — | `native_sized_report_strategy.cpp` | Every native request carries its own size, so there is no run-level default to read back. The adapter keeps Pine's (`default_qty_value` pine_adapter.hpp:67), and `strategy_set_override` pineforge.h:642 overrides it for a compiled Pine strategy. |

`HostSized` native_order.hpp:73 is the sixth intent and the escape hatch: the
host answers the units itself from `resolve_execution_terms`
(native_host.hpp:927), which sees the kernel-resolved quantity as the facts'
`RemainingUnits` native_order.hpp:550 and may return its own. A C host answers
the same question through `on_close_units` `sha256:02b5c7afed8b96339a37052ef77921891dcefca560807c1099a527f049f68b54` native_c_api.h:2470 for a close.

## Orders {#pine_to_native_map_orders}

Everything here is one call: `submit` native_host.hpp:1178, or `submit_market`
native_host.hpp:1192 for the market-only shorthand, which *refuses* non-market
extras rather than dropping them. `submit` returns a `RequestHandle`
native_order_identity.hpp:135; keep it, it is the order id. In C the same call
is `strategy_native_submit_v1` `sha256:9b2a62efb044f5b285a824d5fb15be418b459ffd83c5dd4e1013cf798c8a9a32` native_c_api.h:2590 over a
`pf_native_request_v1` native_c_api.h:2030.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.entry()` | `Request` native_order.hpp:483 carrying `Transact` native_order.hpp:41 | `strategy_native_submit_v1` `sha256:9b2a62efb044f5b285a824d5fb15be418b459ffd83c5dd4e1013cf798c8a9a32` native_c_api.h:2590 | `native_market_strategy.cpp` | Pine's entry *reverses* an opposite position; `Transact` does not. Spell the reversal explicitly with the row below. |
| `strategy.order()` | the same `Request` native_order.hpp:483 | `strategy_native_submit_v1` `sha256:9b2a62efb044f5b285a824d5fb15be418b459ffd83c5dd4e1013cf798c8a9a32` native_c_api.h:2590 | `native_market_strategy.cpp` | `strategy.order` is the non-reversing entry, so it maps 1:1 with no caveat. |
| `strategy.exit()` | `submit_bracket` native_toolkit.hpp:160 over a `BracketSpec` native_toolkit.hpp:37 | `strategy_native_submit_v1` `sha256:9b2a62efb044f5b285a824d5fb15be418b459ffd83c5dd4e1013cf798c8a9a32` native_c_api.h:2590 per leg | `native_bracket_strategy.cpp` | The full bracket is described below in [Brackets](@ref pine_to_native_brackets). |
| `strategy.close()` | `Reduce` native_order.hpp:220 with `OwnerOpenedUnits` native_order.hpp:191 and `BindOpening` native_order.hpp:445 | `PF_NATIVE_REDUCE_OWNER_OPENED` native_c_api.h:342 with `PF_NATIVE_OWNER_BIND_OPENING` native_c_api.h:447 | `native_selected_strategy.cpp` | Closes the lots one opening produced. The cycle comes from `ExecutionAppliedEvent::cycle_after` native_order.hpp:1139. A fractional close is `ScopeFraction` native_order.hpp:207 — `qty_percent = 50` is `fraction = 0.5`, with `ScopeClaim` native_order.hpp:195 and `ScopeBasis` native_order.hpp:202 saying whether siblings are netted and when the scope is measured. |
| `strategy.close_all()` | `Flatten` native_order.hpp:36 | `PF_NATIVE_INTENT_FLATTEN` native_c_api.h:325 | `hello_kernel.cpp` / `hello_kernel_c.c` | The whole book, in one settlement cycle. |
| `strategy.cancel()` | `cancel_where` native_host.hpp:1243 with `NativeRequestField::Label` native_host.hpp:679 | `strategy_native_cancel_where_v1` `sha256:64e752669b24739d5a68c49d86c86ee8371eacbb9e61a46741f62f2f9d4ab4f4` native_c_api.h:2677 | `native_trail_risk_strategy.cpp` | Withdraws exactly the live requests whose `Request::label` native_order.hpp:485 is that id, and answers how many. `cancel` native_host.hpp:1214 is the single-request form for a host that kept the handle. |
| `strategy.cancel_all()` | `cancel_all` native_host.hpp:1232 | `strategy_native_cancel_all_v1` `sha256:6e455462adb1d127c090d34a80127552fd0df9b84d00bfcf222a570241bbf64e` native_c_api.h:2656 | `native_trail_risk_strategy.cpp` | Neither field is indexed: every form walks the live book once. A `PendingUntilArmed` native_order.hpp:394 child is absent from `native_working_requests` native_host.hpp:1228 before its arm, and all three cancels still address it. |
| `strategy.oca.cancel` | `GroupEffect::Cancel` native_order.hpp:466 | `PF_NATIVE_GROUP_CANCEL` native_c_api.h:466 | `native_bracket_strategy.cpp` | Membership is `Member` native_order.hpp:468 inside `Group` native_order.hpp:473. |
| `strategy.oca.reduce` | `GroupEffect::Reduce` native_order.hpp:466 | `PF_NATIVE_GROUP_REDUCE` native_c_api.h:467 | `tests/test_native_resting_group_contract.cpp` | The sibling's quantity is reduced instead of cancelled. |
| `strategy.oca.none` | `NoGroup` native_order.hpp:467 | `PF_NATIVE_GROUP_NONE` native_c_api.h:460 | `hello_kernel.cpp` | The default: no group, no sibling effect. |
| `strategy.direction.all` | `NativeOpenDirections::Both` native_run_spec.hpp:75 | `allowed_open_directions` pineforge.h:517 | `native_risk_limits_strategy.cpp` | The default. |
| `strategy.direction.long` | `NativeOpenDirections::Long` native_run_spec.hpp:73 | `allowed_open_directions` pineforge.h:517 | `tests/test_native_run_spec.cpp` | An opening the direction forbids is refused with `OpeningDirection` native_execution_consumer.cpp:2892. |
| `strategy.direction.short` | `NativeOpenDirections::Short` native_run_spec.hpp:74 | `allowed_open_directions` pineforge.h:517 | `tests/test_native_run_spec.cpp` | Same, the other way. |
| `strategy.commission.percent` | `NativeFeeKind::Percent` native_run_spec.hpp:20 | `fee_kind` pineforge.h:516 | `native_sized_report_strategy.cpp` | `fee_value` is percent ÷ 100 of the absolute account notional. |
| `strategy.commission.cash_per_contract` | `NativeFeeKind::CashPerUnit` native_run_spec.hpp:21 | `fee_kind` pineforge.h:516 | `tests/test_native_selected_requests.cpp` | Account currency per unit. |
| `strategy.commission.cash_per_order` | `NativeFeeKind::CashPerExecution` native_run_spec.hpp:22 | `fee_kind` pineforge.h:516 | `native_open_lots_strategy.cpp` | One ticket per execution, split by units over every slice that execution touches — the closed rows and, for a reversal, the lot it opens. |

Two more order concepts Pine has no word for, both generic:
`ReverseTo` native_order.hpp:60 is one exact signed target reached in one
ticket and one settlement cycle — the explicit form of Pine's implicit
reversal; and `PointBudget` native_order.hpp:379 caps how much of a request may
fill at a single matching point, where Pine only ever uses the whole remaining
quantity. Amending a live order is `replace` native_host.hpp:1185, which emits
`ReplacedEvent` native_order.hpp:997 and can carry the predecessor's live
trigger state forward through `ReplaceOptions` native_order.hpp:367 -- or keep
its handle, or carry a close's book binding, from C++ only. Its C
spelling is `strategy_native_replace_v1`, or `strategy_native_replace_ext_v1`
when a rejection's `RequestRejectReason` native_order.hpp:821 matters.

### Triggers {#pine_to_native_triggers}

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `limit=` | `Limit` native_order.hpp:237 | `PF_NATIVE_TRIGGER_LIMIT` native_c_api.h:379 | `native_calc_on_fills_strategy.cpp` | `fill_through` native_order.hpp:239 makes it market-if-touched: the level gates when the request becomes executable, and the fill is no longer bounded by it. |
| `stop=` | `Stop` native_order.hpp:246 | `PF_NATIVE_TRIGGER_STOP` native_c_api.h:380 | `native_price_grid_strategy.cpp` | Stop-limit is `StopLimit` native_order.hpp:252. |
| `trail_points` | `FromOwnerFill{+ticks, true}` native_order.hpp:331 | `arm_scope` / `arm_first_match` `sha256:4c4077f5233d7b7b67042c0b124b5174397b7908b627b6a8b746ae7f59da5ab7` native_c_api.h:2120 | `native_bracket_strategy.cpp` | Pine points are the arm distance from the entry, in ticks. The adapter lowers that anchor distance separately from the ride offset; it is not `TrailTicks`. |
| `trail_offset` | `TrailTicks` native_order.hpp:293 (resolved into `Trail::offset` native_order.hpp:291) | `p1` `sha256:8303493f9f1c487209078c3c6cefea0a45ffb77487203cd5e827f3f6b0215024` native_c_api.h:2054 | `native_trail_risk_strategy.cpp` | Pine offset is the ride distance behind the running best, also expressed in ticks. Zero is legal and means "ride the best"; the first adverse move past it exits. |
| `trail_price` | `Trail::arm_price` native_order.hpp:292 | `trail_has_arm_price` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2059 | `native_trail_risk_strategy.cpp` | The absolute level the trail arms at. Read the live projection with `trail_state` native_host.hpp:1065 (C: `strategy_native_trail_state_v1` `sha256:5ad829eb68a6536b5a0abe1ce981b6a59811951a4090768f24babadd74646985` native_c_api.h:2898). The kernel compares `arm_price` with the **raw** path. TradingView tests the activation on the **tick-quantized** bar, with or without an offset and the placement close included (R5 lanes E5 and E9's tapes), so the Pine adapter arms at the activation's half-tick threshold — a placement close whose tick reaches the activation arms there — and books from a running best that starts at the activation; a one-shot (`trail_offset` 0) books the tick its resting threshold stands for, rounded away from the position. Since R5 lane E14 that running best is the **kernel's** own: the trailing leg names the activation — or the favourable one of the activation and the placement print when that print armed it — in the generic `Trail::best_seed` native_order.hpp:294, so the ride no longer starts half a tick short of the activation. |

`limit=` and `stop=` are listed once here rather than once per command,
because a native trigger is a property of the `Request` native_order.hpp:483
and applies to every intent.

### Brackets {#pine_to_native_brackets}

`strategy.exit(from_entry=…, profit=…, loss=…, trail_points=…)` is the one
Pine call with no single native counterpart, because it is four decisions at
once. Natively they are separate and each is nameable:

- **Placed before the entry has a price.** Each leg is a child with
  `WaitForApplied` native_order.hpp:436 naming the parent, and a
  `FromOwnerFill` native_order.hpp:331 anchor instead of an absolute level. The
  trigger's own level field is then left **unwritten**: the `0.0` a
  `Limit::price` / `Stop::price` already defaults to, and an absent
  `Trail::arm_price` — or that same `0.0` — for a trail. Writing any other
  level is refused, because the arm would overwrite it. At
  the parent's fill the kernel materializes `fill + offset` — in ticks when
  `FromOwnerFill::ticks` native_order.hpp:333 is set — snaps it per
  `NativeAnchorRounding` native_order.hpp:309, offers it once to
  `resolve_anchored_level` native_host.hpp:992, and carries the installed level
  in `ArmedEvent` native_order.hpp:1200.
- **Invisible until armed.** `NativeArmVisibility::PendingUntilArmed`
  native_order.hpp:394 keeps the legs out of `native_working_requests`
  native_host.hpp:1228 until the fill, like Pine's pending exit and like a
  broker that shows no working child before the parent trades.
- **First match.** `NativeArmFirstMatch::AfterArmPrint` native_order.hpp:412
  gives the leg the birth rule of a request submitted from the fill callback:
  a level the fill print already satisfies does not match on that print.
- **What it closes.** `NativeArmScope::Book` native_order.hpp:427 binds the leg
  to the whole position the fill left, later same-id adds included, instead of
  only the lot its owner opened; `qty_percent=100` is
  `Reduce{OwnerOpenedUnits{}}` native_order.hpp:220, bound at the arm.
- **One-cancels-all.** `Member` native_order.hpp:468 with `GroupEffect::Cancel`
  native_order.hpp:466 makes the take-profit and the stop-loss exclusive.

`submit_bracket` native_toolkit.hpp:160 places the three legs of a
`BracketSpec` native_toolkit.hpp:37 in one call and hands back a
`BracketReceipt`, which reports **every** leg: beside the handle it rests
under, a `BracketLegOutcome` saying whether the leg was never requested, never
submitted (no allocated parent), accepted, or refused with the kernel's own
`RequestRejectReason`. Pine drops a leg it cannot place without telling the
script; a native host reads `every_requested_leg_accepted()`.
`native_bracket_strategy.cpp` is the runnable version.

`native_toolkit::OrderBook<Key>` native_toolkit.hpp:309 is the same idea for a
host that keeps its own key → handle book (replace, re-price, forget) instead
of a predicate, and it reports the kernel verbatim too:
`submit_or_replace_outcome` answers an `OrderBookAction` — `Replaced`,
`ReplaceRejected`, `SubmitAccepted`, `SubmitRejected` — beside the
`ReplaceResult` and `SubmitResult` of every command that actually reached the
kernel, and `cancel_outcome` says whether a cancel was commanded at all. Pine's
`strategy.cancel` tells a script neither. In C
each leg is its own `strategy_native_submit_v1` `sha256:9b2a62efb044f5b285a824d5fb15be418b459ffd83c5dd4e1013cf798c8a9a32` native_c_api.h:2590 with
`PF_NATIVE_OWNER_WAIT_FOR_APPLIED` native_c_api.h:446, and its `first_match`
and `scope` are the request's `arm_first_match` / `arm_scope`
`sha256:b187f061543dbbc3f07d32b8acb39854827d36762f863c3a552ef6e6efdf71e8` native_c_api.h:2120-2121, so a C bracket child arms after its owner's print or over
the whole book exactly as a C++ one does.

The Pine adapter lowers its own queued relative legs exactly this way — a
host-sized `Book` / `AfterArmPrint` child, with `resolve_anchored_level`
carrying only TradingView's half-tick threshold — and adopts the armed child at
the parent's fill. An explicit `qty=`, `close_entries_rule="ANY"` and stream
runs keep the adapter's fill-point submission.

## Risk limits {#pine_to_native_map_risk}

Pine's `strategy.risk.*` are per-bar statements. Natively they are one opt-in
block, `NativeRiskLimits` native_run_spec.hpp:309, declared before the run and
measured at three points of every script bar: its open, its own close
calculation, and after each applied drain. A breach does what
`NativeRiskAction` native_run_spec.hpp:277 says — refuse openings, or first
flatten the book with one kernel-originated request and then refuse. In C the
block is the `PF_NATIVE_SPEC_EXT_RISK` native_c_api.h:727 tail.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.risk.allow_entry_in()` | `allowed_open_directions` native_run_spec.hpp:595 | `allowed_open_directions` pineforge.h:517 | `native_risk_limits_strategy.cpp` | The one `strategy.risk.*` rule that is already the kernel's, per ADR-0001's `risk` ruling row. Rejects with `OpeningDirection` native_execution_consumer.cpp:2892. |
| `strategy.risk.max_position_size()` | `max_abs_units` native_run_spec.hpp:593 | `max_abs_units` pineforge.h:518 | `tests/test_native_run_spec.cpp` | Tests the *resulting* book. TradingView gates the *live* book before the fill, so an entry is refused only once the book already holds the limit — ruled **adapter-policy** in ADR-0001's `max_abs_units` row, with lane N12's measurement (the adapter ends at 4 where the kernel cap ends at 2). |
| `strategy.risk.max_drawdown()` | `NativeRiskLimits::max_drawdown` native_run_spec.hpp:310 | `risk_max_drawdown` native_c_api.h:2247 | `native_risk_limits_strategy.cpp` | `NativeLossLimit` native_run_spec.hpp:258 is account currency, or a percent of the running peak when `percent` native_run_spec.hpp:260 is set. Blocks openings to the end of the run. |
| `strategy.risk.max_intraday_loss()` | `NativeRiskLimits::max_intraday_loss` native_run_spec.hpp:311 | `risk_max_intraday_loss` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2257 | `native_risk_limits_strategy.cpp` | Percent is of the day's opening equity. Blocks to the end of its own day. |
| `strategy.risk.max_cons_loss_days()` | `NativeRiskLimits::max_consecutive_loss_days` native_run_spec.hpp:312 | `risk_max_consecutive_loss_days` `sha256:6519ab1f9bc3be346a80266278fec3f3178b11b2dc499fcb16424ae5b25cb9cf` native_c_api.h:2259 | `native_risk_limits_strategy.cpp` | Counts *days* that close with a realized loss; TradingView's streak counts trades. `NativeRiskDay` native_run_spec.hpp:269 chooses the session day or the civil date in the spec timezone. |
| `strategy.risk.max_intraday_filled_orders()` | `NativeRiskLimits::max_fills_per_day` native_run_spec.hpp:313 | `risk_max_fills_per_day` `sha256:2c520ff10e5b3b36096aaa97b31c113325a5d9df6aa32216d16ead9989ec3e07` native_c_api.h:2261 | `native_trail_risk_strategy.cpp` | Applied fills counted as they settle, so the fills of one matching point all settle and the limit blocks the next admission. Read the ledger back with `native_risk_state` native_host.hpp:1288. |

The whole block is **native-only by ruling**: no adapter run declares it, and
lane N12 measured why — TradingView's drawdown latch samples at the close and
still admits a reversal, its loss-day streak counts trades, its intraday loss
closes at the path's adverse extreme and withdraws the book, and its fill cap
charges slots and transfers quota on a chart-day key. The ruling of record is
ADR-0001's `risk` row and `native-feature-parity.md:473`; the permanent witness
is `tests/test_adapter_risk_relower.cpp`.

## Margin and liquidation {#pine_to_native_map_margin}

Pine has `margin_long` / `margin_short` and a margin call it does not let you
describe. `NativeMarginModel` native_run_spec.hpp:239 is the broker model:
per-side opening requirements, per-side maintenance requirements, a solved
liquidation level, and the ticket the forced close is booked under.

| concept | C++ | C | Runs in |
| --- | --- | --- | --- |
| opening requirement per side | `initial_long` native_run_spec.hpp:240 / `initial_short` native_run_spec.hpp:241 | `margin_initial_long` native_c_api.h:2221 | `native_margin_strategy.cpp` |
| maintenance requirement per side | `maintenance_long` native_run_spec.hpp:242 / `maintenance_short` native_run_spec.hpp:243 | `margin_maintenance_long` native_c_api.h:2226 | `native_margin_strategy.cpp` |
| how much a liquidation closes | `NativeLiquidationSizing` native_run_spec.hpp:139 | `margin_sizing` `sha256:9cd8b5e7d4e5c5f861ec2800841ea937f1a0ed59bf12f2b1f8b993e8620b1f56` native_c_api.h:2223 | `native_margin_strategy.cpp` |
| when the requirement is tested | `NativeLiquidationCheck` native_run_spec.hpp:157 | `margin_check` `sha256:2e5a16ba84a181f720d3b078b964b728beec93768d20f2c738048a95627740a3` native_c_api.h:2224 | `native_margin_strategy.cpp` |
| which equity it is tested against | `NativeMarginEquityBasis` native_run_spec.hpp:171 | `margin_equity_basis` `sha256:39869ac67fbd619690c162d5ae82593fe4b9303388fa4c6cfcc4c0b68585c9dc` native_c_api.h:2291 | `tests/test_native_margin_model.cpp` |
| which base the level is solved from | `NativeLiquidationLevelBase` native_run_spec.hpp:181 | `margin_level_base` `sha256:91f5ebcbe056cb4f40aed25c2d7c89f80853330a84895b89bb59fc57633f1983` native_c_api.h:2292 | `tests/test_native_margin_model.cpp` |
| the forced close's ticket | `liquidation_label` native_run_spec.hpp:250 | `margin_liquidation_label` `sha256:42eac5df59f5a43669e208c03d30b61200ecb02a5742872ae7d30a901996a4c1` native_c_api.h:2293 | `native_margin_strategy.cpp` |
| the host's own admission verdict | `validate_execution_precommit` native_host.hpp:940 | — C hosts gate through `on_precommit` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2495 | `native_margin_strategy.cpp` |
| the host's own requirement | `resolve_margin_requirement` native_host.hpp:956 | `on_margin_requirement` `sha256:40a67575247002c7b93ea668f8a18a8af53a5e45efa82106c53b6098b38f43dd` native_c_api.h:2432 | `native_margin_strategy.cpp` |
| which check points run at all | `margin_check_allowed` native_host.hpp:968 | `on_margin_check` native_c_api.h:2434 | `native_margin_strategy.cpp` |
| a step of the account FX curve, checked though no price moved | `NativeMarginCheckKind::FxRoll` native_host.hpp:542 | `PF_NATIVE_MARGIN_CHECK_FX_ROLL` native_c_api.h:918 | `native_fx_roll_strategy.cpp` |
| the host's own liquidation size | `resolve_margin_call_units` native_host.hpp:975 | `on_margin_call_units` `sha256:eab736154d55dd3a09b18b797fbd4e1a4f8aa34cc8fbf3e4c6b8bd82a875a4eb` native_c_api.h:2448 | `native_margin_strategy.cpp` |
| the liquidation event | `MarginCallEvent` native_order.hpp:1216 delivered to `on_native_margin_call` native_host.hpp:981 | `on_margin_call` `sha256:437d35429846384844f43de626612c37a729413d25240816db794e08c72a7d81` native_c_api.h:2396 | `native_margin_strategy.cpp` |

The one-scalar `initial_margin_fraction` native_run_spec.hpp:596 predates the
model and remains the simple spelling; the two are mutually exclusive. It is
ruled **adapter-policy** (ADR-0001's `initial_margin_fraction` row): the
adapter answers TradingView's ten-significant-digit money admission itself with
`NativePrecommitVerdict::AdmitWithHostMargin` native_host.hpp:509, and declares
a maintenance-only model, because a positive initial requirement would decline
openings TradingView takes.

## Calculation context {#pine_to_native_map_context}

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| script body at bar close | `on_native_bar` native_host.hpp:874 | `on_bar` `sha256:27f360147b3e709e5bf37319d51dc610b10a6b5b7481ed6b814fa925eb3a1259` native_c_api.h:2386 | `hello_kernel.cpp` / `hello_kernel_c.c` | The one pure virtual. Called once per closed script bar — and, with the default cadence, at every calculation there is: the kernel routes all of them through `on_native_recalculate` native_host.hpp:893, whose default forwards here. |
| every calculation, tagged with its reason | `on_native_recalculate` native_host.hpp:893 with `NativeCalculationReason` native_host.hpp:806 | `on_recalculate` `sha256:1a3c1bb1563bd37536fa83388c44f40122d704f44ae6d20c8690d436d8583852` native_c_api.h:2414 | `native_calc_on_fills_strategy.cpp` | **Installing it REPLACES `on_native_bar` for every calculation** — in C++ you override the forwarding away, and in C a non-NULL `on_recalculate` is what the kernel calls instead of `on_bar`. A host that wants both calls the other itself from here; a C host that installs only `on_recalculate` and still expects `on_bar` silently trades nothing. |
| `barstate.isconfirmed` | `NativeCalculationReason::BarClose` native_host.hpp:807 | `on_recalculate` `sha256:1a3c1bb1563bd37536fa83388c44f40122d704f44ae6d20c8690d436d8583852` native_c_api.h:2414 | `native_calc_on_fills_strategy.cpp` | The script bar's own calculation is delivered for every run, whatever the cadence is. |
| `barstate.isrealtime` | `NativeStateView::phase` native_host.hpp:285 with `NativeRunPhase` native_host.hpp:41 | `pf_native_state_v1` `sha256:a5d9e67a74cd49c9699c5410eb803d678427d972ff6d9815897d997532b6e3d0` native_c_api.h:1709 | `native_market_strategy.cpp` | `Batch`, `Warmup`, `Realtime`. |
| bar-open pre-pass | `on_native_bar_open` native_host.hpp:870 | `on_bar_open` `sha256:223ccf4f05ca7d350cbc1a87ff905b855cfdae414599cc6cb0db4cc122e75734` native_c_api.h:2385 | `native_calc_on_fills_strategy.cpp` | Receives the whole script bar by default; `NativeOpenBarView::OpenOnly` native_run_spec.hpp:108 masks that lookahead (H = L = C = open, volume 0) for a host that must decide at the open with open-only information. |
| the bar so far, mid-bar | `current_partial_bar` native_host.hpp:1046 | `strategy_native_partial_bar_v1` `sha256:8e9ca16ca280ba871f4c6629be3b2ae25fe55ca67b6ba9fcb6e06988f36b66e4` native_c_api.h:2883 | `native_calc_on_fills_strategy.cpp` | Never runs ahead of the cursor: at a fill inside a leg it holds the points already reached and never the later high. |
| a fill, as it happens | `on_native_applied` native_host.hpp:918 | `on_applied` native_c_api.h:2381 | `native_calc_on_fills_strategy.cpp` | Fires mid-path after each fill; a request born there is eligible on the unconsumed rest of the bar native_execution_consumer.cpp:5196. |
| raw input before aggregation | `on_native_input` native_host.hpp:856 | `on_input` `sha256:844c5da9dddc393a554c84b7c6e6a2771432f07be0a04e0030d75c91ea1036b9` native_c_api.h:2383 | `native_auxiliary_feed_strategy.cpp` | Every accepted confirmed input bar, before aggregation or matching. |
| a lower-timeframe sub-bar | `on_native_sub_bar` native_host.hpp:908 | `on_sub_bar` `sha256:3f4a0cf1f3a0541114cb50047d353fc25114e67480bc05b65c05a37a78e7c48a` native_c_api.h:2424 | `tests/test_native_calc_timing.cpp` | `NativeCalculationReason::SubBar` is reserved and never delivered to the recalculate hook — a sub-bar has its own callback. |
| `time`, `bar_index`, the bar's calendar interval, session facts | `NativeDecisionContext` market_driver.hpp:114 with `NativeCoordinate` market_driver.hpp:71 | `pf_native_decision_v1` native_c_api.h:1419 | `hello_kernel.cpp` | Copied onto the callback stack; mutating it changes nothing. In C++ `script_interval` market_driver.hpp:118 and `input_interval` market_driver.hpp:117 add the calendar facts (eligible open, last traded close, next period and next input opens); the C decision carries the script interval's five instants and the session days of its open and of the next input in its tail, presented to a callback table of the current length, and no input interval. The session-day facts are the three rows below. |
| `session.ismarket` | `in_session` market_driver.hpp:154 | `in_session` native_c_api.h:1437 | `tests/test_native_session_day_facts.cpp` | The script bar's label is in session on the run's own calendar (`NativeRunSpec::session` / `timezone`); a D/W/M bar holds whole session days, so it is always true there. The calendar applies a session's day mask to the session day's trading date: Sunday 17:00 CT of a CME-style `1700-1600:23456` is Monday's session, in session. Pine's own `session.ismarket` is still generated as the time-of-day predicate `pine_session_ismarket` session_time.hpp:247, whose mask reads each instant's weekday, so on such a masked overnight session the two differ. In C, read the three session bytes only when `session_facts` `sha256:6d543afb9d8a044da8b41d82f29c8ed530e841b3c3aeb66ab6cf83f573b5d289` native_c_api.h:1442 is 1. |
| `session.isfirstbar` | `opens_session_day` market_driver.hpp:155 | `opens_session_day` native_c_api.h:1438 | `tests/test_native_session_day_facts.cpp` | In session, and the bar before it is not, or is on another session day. The session day rolls at the session's first window start and is keyed to its trading date, so an overnight session is one day across local midnight (Tokyo `2230-0500`: its days open at 22:30). "The bar before" is the one the run holds — the batch input, the stream warmup — else the calendar's slot one script width back; a run's first bar opens its day. |
| `session.islastbar` | `closes_session_day` market_driver.hpp:156, `closes_session_day_open_ended` market_driver.hpp:157 | `closes_session_day` native_c_api.h:1439 | `tests/test_native_session_day_facts.cpp` | In session, and the bar after it is not, or is on another session day, "the bar after" read the same way. A batch's final bar closes its day (a batch is complete input); `closes_session_day_open_ended` reads the calendar there instead, for a host recomputing a batch whose last input is still forming (C++ only: a C host's live edge is a stream). A stream's bars read the calendar, so a stream cannot see an early close its session string does not declare: the NYSE half day's 12:45 closes the day in a batch (the next bar held is the next day's) but not in a stream (ADR-0001, ruling "Session-day facts at a bar with nothing held after it"). |
| the price at the current point | `current_execution_point` native_host.hpp:1060 | `price` `sha256:03952f4aa9a127e1e2b8ddb82864e94fdaca748143c8c6008c1dda1d7890f73e` native_c_api.h:1488 | `native_selected_strategy.cpp` | The readiness preview `inspect_current_execution` native_host.hpp:1072 is an observation, never an apply token; `execute_current` native_host.hpp:1077 is the apply. |

## Position, account and report {#pine_to_native_map_report}

Three surfaces answer this namespace, and which one you use is the whole
migration decision:

1. **Live, inside a callback** — `physical_position` native_host.hpp:1257,
   `native_marked_equity` native_host.hpp:1266, `native_open_lots`
   native_host.hpp:1262, `native_liquidation_price` native_host.hpp:1285.
2. **The protected accessors your host inherits.** `NativeStrategyHost` derives
   from `BacktestEngine`, so the statistics Pine exposes as `strategy.*` are
   ordinary protected member functions of your own class: `net_profit`
   engine.hpp:938, `gross_profit` engine.hpp:939, `gross_loss`
   engine.hpp:940, `current_equity` engine.hpp:941 and the rest. They are
   reachable from inside your host exactly as they are from inside a generated
   Pine strategy, and `native_open_lots_strategy.cpp` reads every one of them.
3. **The report, after the run** — `fill_report` engine.hpp:1809 into a
   `pf_report_t` pineforge.h:455, which is what a C host reads.

One rule governs the whole table, in two scopes. The equity **series** is
recorded at the kernel's own report point, which exists **only** under
`NativeReportPolicy::KernelRecorded` native_run_spec.hpp:62 (or
`KernelRecordedAtHostMarks`); the default `HostRecorded` leaves that series,
and every `pf_equity_stats_t` figure walked out of it, to the host. The equity
**extremes** and the **position-size peaks** are not scoped that way: they are
a property of the run, folded under every policy (lane E2) — at every script
calculation under `HostRecorded` and `KernelRecorded`, and at the host's own
marks under `KernelRecordedAtHostMarks`, which handed that cadence away. So a
host on the default reads them truthfully without recording a curve.
`native_open_lots_strategy.cpp` runs the same tape under both policies and
prints the difference; recording books no cash and places no order, so the two
runs close the same trades for the same money.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.account_currency` | `currency` native_run_spec.hpp:569 | `currency` pineforge.h:513 | `hello_kernel.cpp` / `native_fx_roll_strategy.cpp` | Declared, never inferred. `account_fx` native_run_spec.hpp:580 is the one positive scalar that converts quote to account; a timestamped curve is `configure_native_fx_curve` native_host.hpp:1167. The FX-roll example runs a JPY account on a USD-quoted stock through such a curve, and a step of it is a margin check point of its own. |
| `strategy.initial_capital` | `initial_capital` native_run_spec.hpp:578 | `initial_capital` pineforge.h:515 | `hello_kernel.cpp` | The value you declared, unchanged by the run. |
| `strategy.equity` | `native_marked_equity` native_host.hpp:1266 | `strategy_native_marked_equity_v1` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2914 | `native_open_lots_strategy.cpp` | Pine marks at the current `close`; the native call takes the mark explicitly, so the caller says what "now" means. It equals the balance plus the open lots' `unrealized_pnl` native_host.hpp:341, which the example asserts at every calculation. |
| `strategy.netprofit` | `net_profit` engine.hpp:938 | `net_profit` pineforge.h:398 | `native_open_lots_strategy.cpp` | Realized only. |
| `strategy.netprofit_percent` | derive: `net_profit` engine.hpp:938 over `initial_capital` native_run_spec.hpp:578 | `pf_metrics_t` pineforge.h:344 | `native_sized_report_strategy.cpp` | The kernel keeps the facts, not the ratio. |
| `strategy.grossprofit` | `gross_profit` engine.hpp:939 | `pf_trade_stats_t` pineforge.h:277 | `native_open_lots_strategy.cpp` | |
| `strategy.grossprofit_percent` | `grossprofit_percent` engine.hpp:946 | `pf_metrics_t` pineforge.h:344 | `native_sized_report_strategy.cpp` | Of initial capital. |
| `strategy.grossloss` | `gross_loss` engine.hpp:940 | `pf_trade_stats_t::gross_loss` pineforge.h:232 | `native_open_lots_strategy.cpp` | The protected C++ accessor is signed (negative for losses), while the C report stores `gross_loss` as a positive magnitude. Convert C to the protected sign with `-gross_loss`; then `netprofit == grossprofit - C.gross_loss`. |
| `strategy.grossloss_percent` | `grossloss_percent` engine.hpp:949 | `pf_metrics_t` pineforge.h:344 | `native_sized_report_strategy.cpp` | |
| `strategy.openprofit` | `open_profit` `sha256:0490e46ca24b81c82c9549ce38af81be1ea888a60277263108cd26bf3f01e0c7` engine.hpp:995 | `strategy_native_marked_equity_v1` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2914 less the balance | `native_open_lots_strategy.cpp` | Takes the price to mark at, like every other native mark. |
| `strategy.openprofit_percent` | derive from `open_profit` `sha256:0490e46ca24b81c82c9549ce38af81be1ea888a60277263108cd26bf3f01e0c7` engine.hpp:995 | — | `native_open_lots_strategy.cpp` | Pine's denominator is the realized equity. |
| `strategy.max_drawdown` | `max_drawdown_` engine.hpp:577 | `max_equity_drawdown` pineforge.h:282 | `native_open_lots_strategy.cpp` | Folded under **every** report policy — see the rule above. The scalar is the run's own; the `pf_equity_stats_t` figure derived from the recorded curve still needs `KernelRecorded` native_run_spec.hpp:62. |
| `strategy.max_drawdown_percent` | `max_drawdown_percent` engine.hpp:1473 | `max_equity_drawdown_pct` pineforge.h:282 | `native_sized_report_strategy.cpp` | Same rule. |
| `strategy.max_runup` | `max_runup_` engine.hpp:578 | `max_equity_runup` pineforge.h:286 | `native_open_lots_strategy.cpp` | Same rule. |
| `strategy.max_runup_percent` | `max_runup_percent` engine.hpp:943 | `max_equity_runup_pct` pineforge.h:286 | `native_sized_report_strategy.cpp` | Same rule. |
| `strategy.avg_trade` | `avg_trade` engine.hpp:952 | `pf_trade_stats_t` pineforge.h:277 | `native_open_lots_strategy.cpp` | |
| `strategy.avg_trade_percent` | `avg_trade_percent` engine.hpp:956 | `pf_trade_stats_t` pineforge.h:277 | `native_sized_report_strategy.cpp` | |
| `strategy.avg_winning_trade` | `avg_winning_trade` engine.hpp:963 | `pf_trade_stats_t` pineforge.h:277 | `native_sized_report_strategy.cpp` | |
| `strategy.avg_winning_trade_percent` | `avg_winning_trade_percent` engine.hpp:969 | `pf_trade_stats_t` pineforge.h:277 | `native_sized_report_strategy.cpp` | |
| `strategy.avg_losing_trade` | `avg_losing_trade` engine.hpp:966 | `pf_trade_stats_t` pineforge.h:277 | `native_sized_report_strategy.cpp` | |
| `strategy.avg_losing_trade_percent` | `avg_losing_trade_percent` engine.hpp:978 | `pf_trade_stats_t` pineforge.h:277 | `native_sized_report_strategy.cpp` | |
| `strategy.wintrades` | `count_wintrades` engine.hpp:1008 | `pf_trade_stats_t` pineforge.h:277 | `native_open_lots_strategy.cpp` | |
| `strategy.losstrades` | `count_losstrades` engine.hpp:1009 | `pf_trade_stats_t` pineforge.h:277 | `native_open_lots_strategy.cpp` | |
| `strategy.eventrades` | `eventrades` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` engine.hpp:1808 | `pf_trade_stats_t` pineforge.h:277 | `native_open_lots_strategy.cpp` | Public, not protected: a zero-profit closed row. Win + loss + even is the closed-row count, which the example asserts. |
| `strategy.max_contracts_held_all` | `max_contracts_held_all` engine.hpp:1802 | — | `native_open_lots_strategy.cpp` | `pf_trade_stats_t` has no max-contracts-held field. A C host must derive the peak from position/open-lot observations; there is no direct C report counterpart. |
| `strategy.max_contracts_held_long` | `max_contracts_held_long` engine.hpp:1803 | — | `tests/test_native_report_truth.cpp` | No direct C report counterpart; derive the long-side peak from position/open-lot observations. |
| `strategy.max_contracts_held_short` | `max_contracts_held_short` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` engine.hpp:1805 | — | `tests/test_native_report_truth.cpp` | No direct C report counterpart; derive the short-side peak from position/open-lot observations. |
| `strategy.position_size` | `NativePhysicalPosition::signed_units` native_host.hpp:297 | `strategy_native_position_v1` native_c_api.h:2694 | `hello_kernel.cpp` | Signed: no separate direction field. |
| `strategy.position_avg_price` | `NativePhysicalPosition::average_price` native_host.hpp:298 | `strategy_native_position_v1` native_c_api.h:2694 | `native_selected_strategy.cpp` | Volume-weighted over the open lots. |
| `strategy.position_entry_name` | `native_open_lots` native_host.hpp:1262 then the last row's `entry_label` native_host.hpp:333 | `strategy_native_open_lot_get_v1` `sha256:a16c50d8dc5bee2411db506d58146fa361983776af076bfbb302acae91437d1a` native_c_api.h:2744 | `native_open_lots_strategy.cpp` | Pine reports the newest entry's id; the native book lets you read any lot's. |
| `strategy.margin_liquidation_price` | `native_liquidation_price` native_host.hpp:1285 | `strategy_native_liquidation_price_v1` `sha256:5481f26d587a09d36b675cf3a398552d2ca840b96064a93cdf359194784b865d` native_c_api.h:2922 | `native_margin_strategy.cpp` | Solved from the declared maintenance requirement, so it answers `nullopt` when the model declares none or the slope is degenerate (a long at full maintenance has no finite level). |
| `strategy.convert_to_account()` | none — the kernel converts with the FX you declare | `strategy_configure_native_fx_curve_v1` `sha256:64e752669b24739d5a68c49d86c86ee8371eacbb9e61a46741f62f2f9d4ab4f4` pineforge.h:1331 | `native_fx_roll_strategy.cpp` / `tests/test_native_fx_curve.cpp` | Pine's converter needs TradingView's own FX series. Natively, `account_fx` native_run_spec.hpp:580 is one scalar and `configure_native_fx_curve` native_host.hpp:1167 stages an immutable timestamped curve; codegen emits the Pine call as the identity, because no fixed table reproduces TradingView's series. |
| `strategy.convert_to_symbol()` | none — same reason, the other direction | `strategy_configure_native_fx_curve_v1` `sha256:64e752669b24739d5a68c49d86c86ee8371eacbb9e61a46741f62f2f9d4ab4f4` pineforge.h:1331 | `tests/test_native_fx_curve.cpp` | Same ruling. |

@anchor pine_to_native_map_trades
## Open trades {#pine_to_native_map_open_trades}

`native_open_lots(mark)` native_host.hpp:1262 answers one `NativeOpenLot`
native_host.hpp:328 per open physical lot, in book order (oldest first,
`NativePhysicalPosition::lot_count` native_host.hpp:299 rows). The C twin is
`strategy_native_open_lot_count_v1` native_c_api.h:2727 then
`strategy_native_open_lot_get_v1` `sha256:a16c50d8dc5bee2411db506d58146fa361983776af076bfbb302acae91437d1a` native_c_api.h:2744 into a
`pf_native_open_lot_v1` native_c_api.h:1679, whose two strings borrow the
snapshot until the next count call. `mark` is the price the three marked fields
are computed at; Pine's builtins mark at the current `close`. Reading it moves
no fill, no hash and no row. `native_open_lots_strategy.cpp` is the runnable
version of this whole table.

Each row also carries the identity Pine has no word for: `entry_incarnation`
native_host.hpp:330 is the never-reused request record whose fill opened the
lot, and `cycle` native_host.hpp:331 the position cycle a later `BindOpening`
native_order.hpp:445 names.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.opentrades` | `native_open_lots` native_host.hpp:1262 | `strategy_native_open_lot_count_v1` native_c_api.h:2727 | `native_open_lots_strategy.cpp` | The row count, which equals `lot_count` native_host.hpp:299. |
| `strategy.opentrades.entry_id()` | `entry_label` native_host.hpp:333 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | The opening request's `label` native_order.hpp:485. |
| `strategy.opentrades.entry_comment()` | `entry_comment` native_host.hpp:334 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | The opening request's `comment` native_order.hpp:486. |
| `strategy.opentrades.entry_bar_index()` | `entry_bar_index` native_host.hpp:336 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | Script-bar index of the opening fill. |
| `strategy.opentrades.entry_time()` | `entry_time_ms` native_host.hpp:335 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | Effective time of the opening fill, Unix ms. |
| `strategy.opentrades.entry_price()` | `entry_price` native_host.hpp:337 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | The booked price: slippage and the price grid are already applied. |
| `strategy.opentrades.size()` | `signed_units` native_host.hpp:338 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | Pine's is unsigned with a separate direction; here `> 0` is long, `< 0` short, and `side` native_host.hpp:332 says the same. |
| `strategy.opentrades.commission()` | `entry_commission` native_host.hpp:339 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | The entry fee still on the lot, account currency. A partial close takes its share onto the closed row: the example's FIFO reduce leaves 1.5 units carrying 1.5 of a 2.00 ticket. |
| `strategy.opentrades.profit()` | `unrealized_pnl` native_host.hpp:341 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | The move from `entry_price` to `mark` in account currency, **net of `entry_commission`** — the lot's own term of the marked equity. Gross is `unrealized_pnl + entry_commission`. |
| `strategy.opentrades.profit_percent()` | derive: `unrealized_pnl` native_host.hpp:341 over entry cost | — | `native_open_lots_strategy.cpp` | Entry cost is `entry_price * abs(signed_units) * point_value * account_fx`. |
| `strategy.opentrades.max_runup()` | `favorable_excursion` native_host.hpp:342 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | Largest move for the lot the kernel has sampled along the delivered path, account currency, gross of fees, with `mark` folded in. |
| `strategy.opentrades.max_runup_percent()` | derive from `favorable_excursion` native_host.hpp:342 | — | `native_open_lots_strategy.cpp` | The snapshot carries the facts, not the ratio. |
| `strategy.opentrades.max_drawdown()` | `adverse_excursion` native_host.hpp:343 | `pf_native_open_lot_v1` native_c_api.h:1679 | `native_open_lots_strategy.cpp` | Largest move against the lot, likewise; both excursions are magnitudes `>= 0`. |
| `strategy.opentrades.max_drawdown_percent()` | derive from `adverse_excursion` native_host.hpp:343 | — | `native_open_lots_strategy.cpp` | |
| `strategy.opentrades.capital_held` | `open_trades_capital_held` engine.hpp:987 | — | `native_open_lots_strategy.cpp` | `pf_trade_stats_t` carries no capital-held field. Derive it from the open lots and declared price/value/FX terms, or read the protected C++ accessor. |

A NaN `mark` keeps every booking fact, leaves `unrealized_pnl`
native_host.hpp:341 NaN and folds nothing into the excursions — the example
asserts this at every calculation. A host that owns lot excursions
(`owns_lot_excursions` native_host.hpp:1003) keeps its own sampler, so for that
run the two excursion fields fold `mark` alone.

## Closed trades {#pine_to_native_map_closed_trades}

`closed_trade_count` engine.hpp:1773 and `closed_trade` `sha256:42e5897ef91d28ce48f144d9790367aa27579560cdaa58a2375ae5294891a70f` engine.hpp:1775 answer
the `Trade` engine.hpp:168 rows this run booked, in booking order;
`report_trade_count` engine.hpp:1781 and `get_report_trade` engine.hpp:1784
span the same rows followed by the range-end rows
`report_open_position_at_end` native_run_spec.hpp:613 adds. In C the report's
`pf_report_t::trades` pineforge.h:395 carries the numeric fields, and the
strings and the cause come from `strategy_closed_trade_entry_id`
`sha256:9f8afac22456179e84414258b98b326d017f8e8e6b1ebf4af0ac4e5b98a7178b` pineforge.h:1141, `strategy_closed_trade_exit_id` `sha256:e23d0edddb621265a9848f1f9eb567e690e7f7421c2b235b5730fc2683e0653c` pineforge.h:1154,
`strategy_closed_trade_exit_comment` pineforge.h:1158 and
`strategy_closed_trade_close_cause` `sha256:be247fcc9dd52b9a9faf460176dd5576d839e616f6acc024d3389d04b2d523b3` pineforge.h:1223, which index exactly the
rows of that array.

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `strategy.closedtrades` | `closed_trade_count` engine.hpp:1773 | `total_trades` pineforge.h:392 | `native_open_lots_strategy.cpp` | `closed_trade_count` counts booked closed rows. The C report's `total_trades` also includes range-end rows when `report_open_position_at_end` is enabled; filter `pf_trade_t::open_at_end == 0` to match Pine `strategy.closedtrades`. |
| `strategy.closedtrades.first_index` | none — the kernel keeps every row | — | `native_open_lots_strategy.cpp` | TradingView drops old rows past a 9000-trade cap and advances `first_index` when it does. The kernel caps nothing, so the first index is always 0 and codegen emits the literal. |
| `strategy.closedtrades.entry_id()` | `entry_id` engine.hpp:179 | `strategy_closed_trade_entry_id` `sha256:9f8afac22456179e84414258b98b326d017f8e8e6b1ebf4af0ac4e5b98a7178b` pineforge.h:1141 | `native_open_lots_strategy.cpp` | The same string the lot carried as `entry_label` native_host.hpp:333. |
| `strategy.closedtrades.entry_comment()` | `entry_comment` engine.hpp:180 | — | `native_open_lots_strategy.cpp` | No C accessor: `strategy_closed_trade_entry_id` `sha256:9f8afac22456179e84414258b98b326d017f8e8e6b1ebf4af0ac4e5b98a7178b` pineforge.h:1141 answers the id, not the comment, and `pf_trade_t` carries no strings. A C host that needs it reads the open lot's `entry_comment` (`pf_native_open_lot_v1` native_c_api.h:1679) before the close. |
| `strategy.closedtrades.entry_bar_index()` | `entry_bar_index` engine.hpp:177 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Copied from the lot. |
| `strategy.closedtrades.entry_time()` | `entry_time` engine.hpp:169 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Unix ms. |
| `strategy.closedtrades.entry_price()` | `entry_price` engine.hpp:171 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | |
| `strategy.closedtrades.exit_id()` | `exit_id` engine.hpp:182 | `strategy_closed_trade_exit_id` `sha256:e23d0edddb621265a9848f1f9eb567e690e7f7421c2b235b5730fc2683e0653c` pineforge.h:1154 | `native_margin_strategy.cpp` | For a kernel liquidation this is the model's own `liquidation_label` native_run_spec.hpp:250, which is how a reporting layer classifies the row. |
| `strategy.closedtrades.exit_comment()` | `exit_comment` engine.hpp:181 | `strategy_closed_trade_exit_comment` pineforge.h:1158 | `native_margin_strategy.cpp` | |
| `strategy.closedtrades.exit_bar_index()` | `exit_bar_index` engine.hpp:178 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | |
| `strategy.closedtrades.exit_time()` | `exit_time` engine.hpp:170 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | |
| `strategy.closedtrades.exit_price()` | `exit_price` engine.hpp:172 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | |
| `strategy.closedtrades.size()` | `qty` engine.hpp:173 with `is_long` engine.hpp:176 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Unsigned, with the direction beside it. A partial close books a row for the closed slice only. |
| `strategy.closedtrades.profit()` | `pnl` engine.hpp:174 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Net of `commission` engine.hpp:197. `pnl + commission` is the gross move, which equals the lot's `unrealized_pnl + entry_commission` at a mark equal to the exit price, scaled to the closed slice. |
| `strategy.closedtrades.profit_percent()` | `pnl_pct` engine.hpp:175 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Net return on entry cost. |
| `strategy.closedtrades.commission()` | `commission` engine.hpp:197 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | Entry share plus exit share. One cash-per-execution ticket is split by units over every slice of that execution. |
| `strategy.closedtrades.max_runup()` | `max_runup` engine.hpp:195 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | TradingView's net-open-profit basis: the lot's gross favorable excursion less the entry share, floored at zero. The open-lot field is the gross value it is derived from. |
| `strategy.closedtrades.max_runup_percent()` | derive from `max_runup` engine.hpp:195 | — | `native_open_lots_strategy.cpp` | Over `entry_price * qty * point_value`. |
| `strategy.closedtrades.max_drawdown()` | `max_drawdown` engine.hpp:196 | `pf_trade_t` pineforge.h:211 | `native_open_lots_strategy.cpp` | The adverse excursion plus the entry share. |
| `strategy.closedtrades.max_drawdown_percent()` | derive from `max_drawdown` engine.hpp:196 | — | `native_open_lots_strategy.cpp` | |

Pine has no word for *why* a row closed. The native row does:
`closed_trade_close_cause` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` engine.hpp:1800 (C:
`strategy_closed_trade_close_cause` `sha256:be247fcc9dd52b9a9faf460176dd5576d839e616f6acc024d3389d04b2d523b3` pineforge.h:1223) distinguishes a script
close, a liquidation, a risk flatten and the range end — the kernel states the
last three itself. The bracket-leg cause (`2`) is the kernel's too for a close
its owner's fill armed — a `WaitForApplied` owner relation with an intent that
only closes, which is what `native_toolkit::submit_bracket` builds — so a
native host's take-profit, stop or trail leg reads `2`; an unowned resting
close, or one bound to an opening already live, reads `1`. The Pine adapter
classifies its own rows by order family instead. Which leg closed a row is its
`exit_id`, the leg's own `label`.

## Series, indicators, higher timeframes {#pine_to_native_map_series}

| Pine | C++ | C | Runs in | Notes |
| --- | --- | --- | --- | --- |
| `request.security()` | `NativeTimeframeSubscription` native_run_spec.hpp:484 in `subscriptions` native_run_spec.hpp:631, or `declare_timeframe_subscriptions` native_host.hpp:1101 inside `on_native_run_begin` native_host.hpp:853 | `strategy_native_declare_subscriptions_v1` `sha256:a160b00c4326e49e08fe88aa8de5dee57c87f5d395ac56465c4770b4c45ec590` native_c_api.h:2808 | `native_htf_strategy.cpp` | A subscription is a series instance: several may share one timeframe. Completed buckets arrive at `on_native_timeframe_bar` native_host.hpp:866 and the latest is pulled with `native_series_bar` native_host.hpp:1084. `authoritative_bars` native_run_spec.hpp:486 replace a completed bucket's OHLCV. The row's two delivery words are typed in C: `pf_native_lookahead_e` native_c_api.h:782 and `pf_native_gaps_e` native_c_api.h:793 (lane E7). The Pine adapter runs its own plain sites through these same subscriptions, ruled **adapter-hook** in ADR-0001's `subscriptions` row. |
| `request.security_lower_tf()` | `NativeAuxiliaryFeed` native_run_spec.hpp:513 with `NativeSeriesSource::AuxiliaryFeed` native_run_spec.hpp:481 | `strategy_native_append_auxiliary_bars_v1` `sha256:d21b121cfaa2b5c53847308307cd3da9f9d434aae45a9bf67219f73d6541560c` native_c_api.h:3031 | `native_auxiliary_feed_strategy.cpp` | Not the same shape: Pine returns an intrabar *array* per bar, the kernel gives you a finer *series* routed by time. A host that wants the raw sub-bars puts them in `IntrabarPath` native_run_spec.hpp:377 and reads `on_native_sub_bar` native_host.hpp:908 instead. |
| `barmerge.gaps_off` | `NativeTimeframeSubscription::gaps` native_run_spec.hpp:488 set false | `pf_native_subscription_v1::gaps` = `PF_NATIVE_GAPS_HOLD` native_c_api.h:794 | `native_htf_strategy.cpp` | The delivered bucket stands until the next delivery replaces it. |
| `barmerge.gaps_on` | `NativeTimeframeSubscription::gaps` native_run_spec.hpp:488 set true | `pf_native_subscription_v1::gaps` = `PF_NATIVE_GAPS_CLEAR` native_c_api.h:796 | `native_htf_strategy.cpp` | The series is cleared on every input bar it delivers nothing on, so the pull answers empty — the native spelling of `na`. |
| `barmerge.lookahead_off` | `NativeTimeframeSubscription::lookahead` native_run_spec.hpp:487 set false | `pf_native_subscription_v1::lookahead` = `PF_NATIVE_LOOKAHEAD_AT_COMPLETION` native_c_api.h:783 | `native_htf_strategy.cpp` | The default and the honest one: a bucket is delivered when it completes. |
| `barmerge.lookahead_on` | `NativeTimeframeSubscription::lookahead` native_run_spec.hpp:487 set true | `pf_native_subscription_v1::lookahead` = `PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT` native_c_api.h:785 | `tests/test_native_htf_subscriptions.cpp` | Delivers a bucket's final values on the input that held its first bar. It is lookahead: use it to reproduce a chart, never to trade. |
| `request.currency_rate()` | none — no FX data feed | `strategy_configure_native_fx_curve_v1` `sha256:64e752669b24739d5a68c49d86c86ee8371eacbb9e61a46741f62f2f9d4ab4f4` pineforge.h:1331 | `native_fx_roll_strategy.cpp` / `tests/test_native_fx_curve.cpp` | The kernel converts with the curve you stage; it fetches nothing. |
| `request.dividends()` | none — no fundamentals feed | — | — | Rejected at transpile for a Pine script; a native host fetches and models it itself. |
| `request.earnings()` | none — no fundamentals feed | — | — | Same. |
| `request.financial()` | none — no fundamentals feed | — | — | Same. |
| `request.splits()` | none — no corporate-actions feed | — | — | Same. |
| `request.economic()` | none — no macro data feed | — | — | Same. |
| `request.footprint()` | none — no footprint data | — | — | Same. |
| `request.quandl()` | none — deprecated upstream | — | — | Same. |
| `request.seed()` | none — TradingView infrastructure | — | — | Same. |

Indicators and history need no translation at all: `pineforge::ta` ta.hpp:12 is
the same numerics the adapter uses, and the header pulls only `na`, `series`
and `window_sum` ta.hpp:2-4, so it is engine-free. `pineforge::Series<T>`
series.hpp:94 is Pine's history operator as a fixed-capacity ring you push what
you want to keep into. Pine's `input.*` becomes ordinary constructor
parameters: `set_input` engine.hpp:1822 exists but its getters are protected,
and a native host takes its parameters in C++.

## One strategy, three ways {#pine_to_native_three_ways}

The same trivial strategy — buy one unit on the first bar, flatten on the
third — in the three front doors this repository offers. The C++ and the C
versions are files in the tree, and both are CTest rows.

**Pine**, through codegen and the adapter:

```pine
//@version=6
strategy("Hello", overlay = true, initial_capital = 10000,
         default_qty_type = strategy.fixed, default_qty_value = 1)

if bar_index == 0
    strategy.entry("long", strategy.long, qty = 1)
if bar_index == 2
    strategy.close_all()
```

**C++**, against the kernel — `examples/native/hello_kernel.cpp:14` is this
class, and `examples/native/hello_kernel.cpp:38` the spec it runs:

```cpp
#include <pineforge/native_host.hpp>

class HelloKernel : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit_market({pineforge::order_action::Transact{1.0}, "hello-long", ""});
        } else if (bars_ == 3) {
            submit_market({pineforge::execution::Flatten{}, "hello-flat", ""});
        }
    }
};
```

The host is a class; the run is one `NativeRunSpec` native_run_spec.hpp:552
naming the clock, instrument, account and fees; `configure_native`
native_host.hpp:1161 applies it and `run` `sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` engine.hpp:1676 drives the bars.

**C**, against the same kernel — `examples/native/hello_kernel_c.c:36` is this
callback and `examples/native/hello_kernel_c.c:66` the run spec:

```c
#include <pineforge/pineforge.h>

static int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* decision) {
    struct state* st = (struct state*)user;
    pf_native_request_v1 request;
    ++st->bars;
    if (st->bars == 1) { /* fill request with PF_NATIVE_INTENT_TRANSACT, +1 unit */ }
    else if (st->bars == 3) { /* fill request with PF_NATIVE_INTENT_FLATTEN */ }
    return 0;   /* non-zero fails the run with PF_NATIVE_FAILURE_CALLBACK */
}
```

The host is a `pf_native_callbacks_v1` native_c_api.h:2371 table handed to
`strategy_native_host_create_v1` `sha256:755c4cd5bc693ea9278bdecf757fe64e1094a71536641e1f4753984d572fa8e5` native_c_api.h:2549; the run is
`pf_native_run_spec_v1` pineforge.h:524 plus `pf_native_run_spec_ext_v1`
`sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` native_c_api.h:2207 through `strategy_configure_native_ext_v1`
`sha256:6cf2f65aea67c6054b256e3cf27126d706f60ed1d8b561396aa3f65c3a0389b4` native_c_api.h:3010, and `strategy_native_run_v1` native_c_api.h:2553 drives
the bars into a `pf_report_t` pineforge.h:455.

Three rules make the C door behave like the C++ one:

1. **Every struct is tagged and size-prefixed.** Set `struct_size`
   `sha256:e4be219b92c87da4de9d9864fd1c7dcf1d94f84846799fde4db68ad4f46e1ac0` native_c_api.h:2037 and `version` `sha256:0080a751ff5a4d1a2f4a3769e30c82cc557785ee4546b24e2512170a2d6ddae4` native_c_api.h:2038; a mismatch is refused
   with a documented negative status and mutates nothing.
2. **A callback must not unwind, and must return 0.** A non-zero return latches
   `NativeFailureCode::CallbackException` native_host.hpp:69 and ends the run
   `Failed`, readable with `strategy_native_state_v1` `sha256:2d70db8f53b92dfd83eef8f0b2b349e286198596e0ffb71201f4ba8da9a9c021` native_c_api.h:2789.
3. **Installing `on_recalculate` replaces `on_bar`.** A C host that installs
   only the recalculate hook and still expects the bar hook silently trades
   nothing.

Not every member of the C++ host has a C spelling, and each that has none
carries a recorded reason. Five members have no direct C spelling: `prepare_native_begin` (it
borrows the codegen ingress a C host never supplies), `inspect_current_execution`
native_host.hpp:1072 (its preview has no size-prefixed POD;
`strategy_native_execute_current_v1` answers the same verdicts) and the two
market-only conveniences `submit_market` / `replace_market`, and `native_closed_rows_amended` (the C++-only row-amendment notification). Every policy hook
has a C route: `validate_execution_precommit` native_host.hpp:940 is
`on_precommit`, `resolve_anchored_level` native_host.hpp:992 `on_anchored_level`,
the price and opening-shape halves of `resolve_execution_terms`
native_host.hpp:927 `on_execution_terms`, `native_sized_units`
native_host.hpp:1279 `strategy_native_sized_units_v1` and the
`hash_host_extension` seam `on_hash_extension`; only its deprecated spelling
`hash_source_extension` has none. The `COVERAGE` block at `sha256:395b1137106e8be4a592796a7559d475edc75866c1632213ffd71a1f0f7c253d` native_c_api.h:52
lists every public member with either its C spelling or that reason, and
`scripts/check_native_c_api_surface.py` proves the list is exactly that class's
public surface.

## A worked migration, end to end {#pine_to_native_worked}

The strategy below uses six things a Pine author would expect to be hard to
port: it sizes by **cash with a fee reserve**, brackets on a **price grid**,
trails in **ticks**, runs a **maintenance-only margin model**, carries a **risk
limit**, and decides on a **higher-timeframe series**. Everything it needs is
on this page.

```pine
//@version=6
strategy("Six", overlay = true, initial_capital = 10000,
         default_qty_type = strategy.cash, default_qty_value = 2500,
         commission_type = strategy.commission.percent, commission_value = 0.1,
         margin_long = 20, margin_short = 20, pyramiding = 1)

htfClose = request.security(syminfo.tickerid, "60", close)

if close > htfClose and strategy.position_size == 0
    strategy.entry("long", strategy.long)
    strategy.exit("bracket", "long", profit = 40, loss = 20, trail_points = 30, trail_offset = 8)
```

### 1. The declaration becomes one spec

```cpp
pineforge::NativeRunSpec spec;
spec.identity = {"six", 1};
spec.input_tf = "15";  spec.script_tf = "15";
spec.ticker = "MOCK";  spec.tickerid = "TEST:MOCK";  spec.type = "crypto";
spec.currency = "USDT";  spec.basecurrency = "ETH";
spec.timezone = "UTC";   spec.session = "24x7";
spec.initial_capital = 10000.0;
spec.point_value = 1.0;  spec.account_fx = 1.0;
spec.price_tick = 0.25;                                     // the instrument's ladder
spec.fee_kind = pineforge::NativeFeeKind::Percent;
spec.fee_value = 0.1;                                       // commission_value = 0.1 -> 0.1 %
spec.max_open_lots = 1;                                     // pyramiding = 1
```

Nothing is inferred. `price_tick` native_run_spec.hpp:581 is the ladder the
next three blocks all measure against, and `fee_value`
native_run_spec.hpp:588 is a **percent**, spelled exactly as Pine's
`commission_value` is: `0.1` charges 0.1 % of each execution's notional. A
fraction there (`0.001`) would charge a hundred times too little.

### 2. The price grid, the margin model and the risk limit

```cpp
spec.price_grid = pineforge::NativePriceGrid::QuantizeFillsAndTriggers;
spec.grid_rounding = pineforge::NativeGridRounding::HalfUp;

pineforge::NativeMarginModel margin;
margin.initial_long = 0.20;                 // margin_long = 20 % -> a fraction
margin.initial_short = 0.20;                // margin_short = 20 %; the side is never used
margin.maintenance_long = 0.15;             // what Pine cannot say
margin.sizing = pineforge::NativeLiquidationSizing::Flatten;
margin.liquidation_label = "margin-call";
spec.margin = margin;

pineforge::NativeRiskLimits risk;
risk.max_drawdown = pineforge::NativeLossLimit{20.0, true};   // 20 % of the peak
risk.action = pineforge::NativeRiskAction::FlattenAndBlock;
spec.risk = risk;
```

A side may waive the opening requirement or the liquidation, but not both.
`initial_* == 0.0` is the **maintenance-only** spelling — the host owns opening
admission on that side — and it is legal only where that side's `maintenance_*`
is set. This strategy never shorts, so its short side simply keeps the ordinary
requirement `margin_short = 20` declares, `initial_short = 0.20`. Writing
`initial_short = 0.0` there with no `maintenance_short` would state nothing at
all, and `configure_native` would refuse the whole spec with
`MarginSideUndeclared` native_run_spec.hpp:746.
The grid is what makes a bracket level a *ladder*
price: `native_price_grid_strategy.cpp` runs one strategy under all four
answers and `native_margin_strategy.cpp` drives a real liquidation.

### 3. The higher-timeframe series

```cpp
void on_native_run_begin() override {
    pineforge::NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    declare_timeframe_subscriptions({hourly});
}
```

`native_series_bar(0)` native_host.hpp:1084 then answers the latest completed
hourly bucket inside any callback — `request.security(…, "60", close)` is
`native_series_bar(0)->close`. `native_htf_strategy.cpp` is the runnable
version, including what `gaps` native_run_spec.hpp:488 changes.

### 4. The entry: cash, with the fee reserved

```cpp
pineforge::native_order::Sized sized;
sized.side  = pineforge::native_order::Side::Long;
sized.basis = pineforge::native_order::CashValue{2500.0};
sized.time  = pineforge::native_order::SizeTime::AtAcceptance;
sized.price = pineforge::native_order::SizePrice::SignalOnTick;
sized.reserve_percent_fee = true;           // divide the cash by (1 + fee)
const auto entry = submit({sized, "long", ""});
// SubmitResult::handle is an optional: empty on a rejection, which the
// reject reason beside it names.
```

`reserve_percent_fee` native_order.hpp:179 is the thing Pine has no spelling
for: without it, a cash-sized entry that spends the whole 2500 cannot also pay
its commission. `SizePrice::SignalOnTick` native_order.hpp:146 sizes against
the decision price on the instrument's own ladder, which is what a trader means
by "2500 at the current price". `native_sized_report_strategy.cpp` is the
runnable version of cash sizing; the fee reserve runs in this section's own
row, below.

### 5. The bracket, anchored and trailing in ticks

```cpp
namespace no = pineforge::native_order;
const no::WaitForApplied owner{*entry.handle, no::NativeArmVisibility::PendingUntilArmed,
                               no::NativeArmFirstMatch::AfterArmPrint,
                               no::NativeArmScope::Book};
const no::Member oca{1, 0, no::GroupEffect::Cancel};

no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "tp", ""};
take_profit.trigger = no::Limit{0.0};
take_profit.anchor  = no::FromOwnerFill{+40.0, true, no::NativeAnchorRounding::Directional};
take_profit.owner   = owner;  take_profit.group = oca;

no::Request stop_loss = take_profit;
stop_loss.label   = "sl";
stop_loss.trigger = no::Stop{0.0};
stop_loss.anchor  = no::FromOwnerFill{-20.0, true, no::NativeAnchorRounding::Directional};

no::Request trail{no::Reduce{no::OwnerOpenedUnits{}}, "trail", ""};
// An anchored trail's ARM threshold comes from the owner's fill, so arm_price
// is left absent: that is the anchored spelling. The 0.0 placeholder means the
// same; any other level is refused (InvalidTrigger), as on an anchored limit.
trail.trigger = no::Trail{0.0, std::nullopt, no::TrailTicks{8.0}};
trail.anchor  = no::FromOwnerFill{+30.0, true, no::NativeAnchorRounding::Directional};
trail.owner   = owner;  trail.group = oca;

submit(take_profit);  submit(stop_loss);  submit(trail);
```

Three legs placed before the entry has a price. The trigger levels are left
unwritten on purpose — `0.0` for the limit and the stop, an absent `arm_price`
for the trail: `FromOwnerFill` native_order.hpp:331 supplies them at the arm as
`fill ± ticks * price_tick`, snapped per `NativeAnchorRounding::Directional`
native_order.hpp:312. Pine's `trail_points = 30` is the *arm* distance (the
anchor) and `trail_offset = 8` the *ride* distance (`TrailTicks`
native_order.hpp:260) — two different numbers that Pine spells in one call.
`native_bracket_strategy.cpp` and `native_trail_risk_strategy.cpp` are the
runnable halves.

### 6. The decision

```cpp
void on_native_bar(const pineforge::Bar& bar,
                   const pineforge::NativeDecisionContext&) override {
    const auto hourly = native_series_bar(0);
    if (!hourly || physical_position().signed_units != 0.0) return;
    if (bar.close > hourly->close) { /* the entry and the three legs above */ }
}
```

### Run it

The six blocks are the port, not a sketch of it. The CTest row
`test_pine_to_native_worked` copies them out of this page by script
(`tests/extract_pine_to_native_worked.py`), compiles them against
`PineForge::kernel` with only a class skeleton, a `main` and a bar tape around
them (`tests/test_pine_to_native_worked.cpp`), and runs them on sixteen
15-minute bars whose first hourly close is 100. `configure_native` answers
`Applied` and no request is refused. The first close above the hourly close,
101, buys `2500 / (101 × (1 + 0.1 / 100))` = 24.7277 units at the next open,
101; the take-profit leg closes them at 111, forty ticks up, with `exit_id`
`tp`, and its two siblings are withdrawn with it — the history records them
as `CancelReason::OwnerGone`, because the lots they close are gone; the row's
commission is 0.1 % of both notionals, 5.2423. The row computes every one of those numbers
from the Pine block at the top of this section, so a block that stops
configuring, or stops charging what the Pine declares, fails it.

### What a Pine author gets wrong the first time

1. **Nothing is implicit.** Pine gets the symbol, session, timezone, tick size
   and capital from the chart and the `strategy()` call. The spec names all of
   them, and `configure_native` native_host.hpp:1161 refuses an incomplete value
   rather than filling a default.
2. **Submission is not execution.** A request submitted on bar *k* is
   *accepted* on bar *k* and *filled* at the next eligible point — the open of
   bar *k+1* under the default `NativeCloseExecution::NextEligiblePoint`
   native_run_spec.hpp:34. Acceptance and fill are separate rows in
   `native_events` native_host.hpp:1294.
3. **`strategy.entry` reverses, `Transact` does not.** If the Pine strategy
   relies on an entry flipping a short into a long, spell it `ReverseTo`
   native_order.hpp:60.
4. **Most percents become fractions — two stay out of 100.** `margin_long = 20`
   is `0.20` and `EquityFraction{0.10}` native_order.hpp:97 is ten percent.
   The two exceptions keep Pine's own unit: `fee_value` native_run_spec.hpp:588
   under `NativeFeeKind::Percent`, so `commission_value = 0.1` is
   `fee_value = 0.1`, and a `NativeLossLimit` whose `percent`
   native_run_spec.hpp:260 is set, so `20.0` is twenty percent.
5. **The equity curve is opt-in; the rest of the report is not.** `trade_count`
   engine.hpp:1764 and `get_trade` `sha256:4725bcb6b45a3a87d70c4f733ba6fbe58c12b18832a22bf81ad3e537bbbc24b5` engine.hpp:1766 are always complete, and so
   are the equity extremes and the position-size peaks, which every report
   policy folds. Only the recorded equity **series**, and the
   `pf_equity_stats_t` figures walked out of it, arrive with
   `NativeReportPolicy::KernelRecorded` native_run_spec.hpp:62 alone — the
   rule of [Position, account and report](@ref pine_to_native_map_report).

## Building and exporting {#pine_to_native_building}

Standalone executable — link the library and run:

```bash
cmake -S . -B build -DPINEFORGE_BUILD_EXAMPLES=ON
cmake --build build -j
./build/examples/native/hello_kernel
ctest --test-dir build -R example_          # every host on this page
```

`PINEFORGE_BUILD_EXAMPLES` CMakeLists.txt:37 builds every host under
`examples/native/`, and each gets a CTest row that fails on a nonzero exit, a
signal, a timeout or a missing summary line — the row's assertion is the
`_pf_example_line_hello_kernel` beside it (examples/native/CMakeLists.txt:101).

A kernel-only build drops the Pine layer entirely:

```bash
cmake -S . -B build-kernel -DPINEFORGE_BUILD_SOURCE_LAYER=OFF -DPINEFORGE_BUILD_EXAMPLES=ON
```

`PINEFORGE_BUILD_SOURCE_LAYER` CMakeLists.txt:43 OFF leaves `libpineforge.a`
holding exactly the objects of `PineForge::kernel` CMakeLists.txt:154, and
every example already links that target, so a source-layer symbol reaching an
example is a link error in the default build too.

Loadable module — the form `pineforge-live` and the C ABI harnesses `dlopen`.
Codegen emits the `extern "C"` entry points for a Pine strategy; for a native
host, one macro does it:

```cpp
#include <pineforge/native_module.hpp>

class MyHost : public pineforge::NativeStrategyHost { /* … */ };

PINEFORGE_EXPORT_NATIVE_STRATEGY(MyHost);
```

`PINEFORGE_EXPORT_NATIVE_STRATEGY` native_module.hpp:271 defines
`strategy_create` `sha256:bdc1d58cf27e0aaacfa19c793899d70108d6ac472ca4f29373d6d279a2f20f25` pineforge.h:570, `strategy_free`, `strategy_set_input`
pineforge.h:632, `strategy_set_override` pineforge.h:642,
`strategy_set_magnifier_volume_weighted` pineforge.h:650, `run_backtest`
pineforge.h:583, `run_backtest_full` pineforge.h:600 and `report_free`
pineforge.h:614. It adds **no** new C symbol: every remaining runtime export —
`strategy_configure_native_v1` pineforge.h:524, the `strategy_stream_*` family
c_abi.cpp:527-646, `strategy_execution_contract` pineforge.h:484 — already
lives in the engine, and the generated `strategy_create` references
`pf_abi_version()` so a static link keeps that object.

The host class must derive from `NativeStrategyHost` and must not be `final`:
the macro wraps it in one derived class, `Module` native_module.hpp:87, which
is what makes the engine's protected presentation-error string (returned by
`strategy_get_last_error`) writable from the C boundary.

Two example modules are built twice — as executables by
`PINEFORGE_BUILD_EXAMPLES` CMakeLists.txt:37, and as the runner's MODULE
target `native_market_example` runner/CMakeLists.txt:45 — from the same source.

## Verifying parity {#pine_to_native_parity}

A native host is *not* a TradingView-parity claim. When you port a strategy
that already exists in Pine and want to know exactly where the two differ, run
the twin harness of `native-feature-parity.md:330`:

1. **Same inputs.** One probe, one bar feed, one `NativeRunSpec`. Run it
   through codegen + the adapter, and through your `NativeStrategyHost`.
2. **Diff trades by identity, never by trade number.** Compare entry time,
   exit time, entry price, exit price, quantity, pnl and commission. A trade
   number shifts the moment one row is added or dropped, which turns a
   one-trade difference into a whole-list difference.
3. **Then diff the detail.** Event order and kind from `native_events`
   native_host.hpp:1294, ownership (which opening a close consumed), and the
   continuation hash `native_continuation_hash` native_host.hpp:1335.
4. **Explain every difference against a named row.** §1 of
   `native-feature-parity.md:42` marks each TradingView-only rule — money
   rounding, half-tick trigger thresholds, trail conventions, margin-call
   sizing, same-bar batching. A difference that maps to one of those rows is
   expected. A difference that does not is a bug in the port or in the kernel.

For the repository's own gates, a change to the kernel or the adapter must
keep the validation corpus byte-identical: `scripts/check_corpus_parity.sh`
on top of `scripts/ci_preflight.py` and `scripts/ci_verify.py`. A native host
that only *uses* the public API changes nothing there by construction. The
whole contributor workflow — the boundary rule, the guards, the floors and the
parity contract — is in [CONTRIBUTING.md](../../CONTRIBUTING.md), and the
version written for an LLM contributor is
[Contributing as an LLM](@ref contributing_llm).
