# Changelog

Notable changes to pineforge-engine. From 1.0.0 the version number follows
semantic versioning over the surfaces the
[public contract](docs/pages/public-contract.md) lists. Releases before 1.0.0
are summarized in the README's *Releases* section and on the GitHub releases
page.

## 1.0.0

Release candidates are tagged `v1.0.0-rc.N`. This entry covers every change
since v0.13.1, the last release: `VERSION` read 0.14.0 from 439bd520 on, and
0.14.0 was never tagged. The engine and pineforge-codegen ship 1.0.0
together, and from 1.0.0 the only supported pair is the same version,
prerelease included.

### What 1.0 means

- **The kernel runs without any Pine adapter.** A C++ host
  (`NativeStrategyHost`, `<pineforge/native_host.hpp>` and the native headers)
  or a C host (`<pineforge/native_c_api.h>`) drives the execution kernel that
  generated PineScript strategies run on: batch and stream runs, the native
  request vocabulary, margin and liquidation, risk limits, FX conversion, the
  per-bar broker-state hash and the report. `-DPINEFORGE_BUILD_SOURCE_LAYER=OFF`
  builds that kernel alone, `libpineforge_kernel.a` (`PineForge::kernel`,
  #257), and `examples/native` holds 18 Pine-free hosts (16 C++, 2 C) that run
  as CTest rows.
- **Pine is an adapter over that kernel.** The Pine source layer was cut out of
  `BacktestEngine` (#253), and generated Pine execution was lowered onto the
  native kernel, retiring the legacy loop (#254). Its host
  (`PineStrategyHost`) and headers live under `include/pineforge/source/`.
- **The version number now promises something:** the C ABI, the native C++
  API, the script ABI epoch and its state-hash values, the 1.0 C-surface
  boundary and the pairing with codegen, as the
  [public contract](docs/pages/public-contract.md) states.

### Breaking changes: what a 0.x user must act on

- **C ABI version 4.** `PF_ABI_VERSION` is 4 (0.13.1: 3). `pf_report_t`
  appends `broker_state_hash` and `broker_state_hash_len`, and it is
  caller-allocated, so a mirror built for ABI 3 under-sizes it: update FFI
  mirrors and check `pf_abi_version() == PF_ABI_VERSION` before a run
  (439bd520, d9b80809). No `PF_API` function was removed or renamed; `pineforge.h`
  declares 34 more and `native_c_api.h` adds the native host API.
- **Two C fields renamed.** `pf_equity_stats_t::sharpe_tv` / `sortino_tv` are
  `sharpe_monthly` / `sortino_monthly`, at the same offsets (48 and 56); the
  old spellings, kept as deprecated aliases during development, are removed
  for 1.0. Rename them in C code and in ctypes or Rust mirrors. The JSON
  report keys stay `sharpe_tv` / `sortino_tv`. Development builds' C++
  `exit_legs::Domain::Coof` / `MagnifierCoof` are likewise gone; the
  enumerators are `FillRecalc` / `MagnifierFillRecalc`, with the same values.
- **Regenerate and relink every generated strategy.** The C++ a generated
  strategy compiles against now carries the script ABI epoch
  `engine_script_run_v19` (an inline namespace 0.13.1 did not have), so an
  object built against 0.x headers does not link. Transpile with the
  pineforge-codegen release of the same version (engine 1.0.0 with codegen
  1.0.0; `v1.0.0-rc.N` with codegen `1.0.0-rc.N`) and rebuild. `PF_ABI_VERSION`
  equality alone does not make a pair.
- **State hashes are new, and v19's.** The broker-state hash, the per-bar hash
  rows, the stream fingerprint and the native continuation hash did not exist
  in 0.13.1; their values are the v19 epoch's as the release candidates fix
  them (#282, #283, #284, #286, #289), and a value recorded by a development
  build before 1.0.0 is not comparable. From 1.0.0 their recipe is fixed for
  all of 1.x.
- **Event retention defaults to a window.** `NativeRunSpec::event_retention`
  defaults to `NativeEventRetention::Window`, which keeps the command journal
  only until every reader has consumed it (#283). A C++ host that reads the
  whole run's events after the run sets `Full` (or `Commands`). A C host that
  does not send the retention word keeps `PF_NATIVE_EVENT_RETENTION_FULL`.
- **`-ffp-contract=off` reaches consumers.** The CMake package hands it to
  every translation unit that links `PineForge::pineforge` or
  `PineForge::kernel` (#282); 0.13.1 compiled the library alone with it. A
  build outside CMake compiles generated strategy code with
  `-ffp-contract=off` itself (docs/pages/integration-cmake.md). The package
  also hands every Clang-compiled consumer `-fbracket-depth=1024`, so the
  deeply nested C++ codegen emits for deep Pine compiles; a Clang build
  outside CMake adds that flag itself.
- **One header moved.** `<pineforge/pine_float_compare.hpp>` is now
  `<pineforge/source/pine_float_compare.hpp>`, a source-layer header
  (e71e589c); new code includes `<pineforge/ta_compare_band.hpp>`. A
  kernel-only build installs neither `include/pineforge/source/` nor
  `include/pineforge/compat/`.
- **CMake pins move to 1.** The package's version file is `SameMajorVersion`,
  so `find_package(PineForge 0.N ...)` does not find a 1.x install: pin
  `find_package(PineForge 1.0 REQUIRED)` (docs/pages/integration-cmake.md).
- **Versions may carry a prerelease.** `VERSION` is `X.Y.Z` or `X.Y.Z-rc.N`.
  `PINEFORGE_VERSION_STRING`, `PineForge_VERSION` and `pf_version_get()` stay
  numeric; the full version is `PINEFORGE_VERSION_FULL`, the package's new
  `PineForge_VERSION_FULL` and `pf_version_string()`, so
  `find_package(PineForge 1.0.0 EXACT)` also accepts `1.0.0-rc.N`. The CMake
  smoke consumer (`cmake/smoke_consumer`) prints `pf_version_string()`.

### What a native host must handle

- **An unrepresentable quantity is a refusal, not a failed run.** A request
  the settlement cannot book exactly in binary64 (a close that would leave a
  sub-ulp rest, an opening a surviving dust lot absorbs) is refused with
  `MatchRejectReason::UnrepresentableQuantity` (C
  `PF_NATIVE_MATCH_REJECT_UNREPRESENTABLE_QUANTITY`, 10) and the run goes on.
  Handle it where you handle the other match rejections (#289).
- **Quantity tolerance is opt-in.** `NativeRunSpec::quantity_tolerance` (C:
  the sixth `pf_native_run_spec_ext_v1` layout, flag
  `PF_NATIVE_SPEC_EXT_QUANTITY_TOLERANCE`) lets a close within that distance
  of a FIFO lot boundary end on it; leave it unset and nothing changes. On a
  quantity grid, a whole-scope `ScopeFraction` and a close of one lot's own
  size are on the grid (#289).
- **OCA groups never stop a run on binary64 dust.** A sibling deduction too
  small to move the sibling's units is absorbed and recorded with a zero
  deduction (#289). A sibling re-priced with `ReplaceOptions::keep_handle`
  inside a group no longer fails the run when another member fills; the run
  records what a plain replace records. On a quantity grid, a request whose
  pending group deduction its units absorb settles in one fill, so a
  whole-scope close closes its lot whole.
- **Aggregated runs count script bars.** When the script timeframe aggregates
  the input, `NativeCoordinate::interval_index` and every lot, trade-row and
  metric index are script-bar indices; read the input slot from
  `NativeCoordinate::input_interval_index` (#289).
- **Report points at the host's cadence.** Under
  `NativeReportPolicy::KernelRecordedAtHostMarks`, a C++ host calls
  `NativeStrategyHost::mark_native_report_point` from its own callbacks; it
  returns `false` outside such a run. The C surface has no such call (#289).
- **Final calendar buckets calculate.** A batch, or `stream_end(true)`,
  calculates its last pending script bucket when that bucket's last input
  reached the session close, so the final daily bucket of an hourly feed now
  calculates; warmup and `stream_end(false)` still carry it (#289).
- **A new margin check kind.** On a continuous intrabar path
  (`IntrabarPath::lower_tf` with its default `ContinuousSegments`
  eligibility) the kernel checks the margin model at every later sample of
  the bar, at that sample's price:
  `NativeMarginCheckKind::IntrabarSample` (C
  `PF_NATIVE_MARGIN_CHECK_INTRABAR_SAMPLE`, 4). A requirement hook sees it; a
  host that switches on the kind must handle it.
- **A current execution refuses a request bound to a host roster.**
  `execute_current` on a request whose owner is a host roster (`BindCohort`)
  answers `NativeCurrentRefusal::UnsupportedRequest` (C
  `PF_NATIVE_REFUSAL_UNSUPPORTED_REQUEST`, 5) before its point is taken; the
  request stays queued and fills at the next match like any queued close.
  When the roster held a live lot it used to fail the run. To close a
  cohort's lots at once, bind the request to its openings (`BindOpenings`).
- **The post-fill margin check measures the fill's own segment.** The
  kernel's `AfterApplied` margin point measures from the waypoint a fill was
  reached on the way to, with the fill price as that point's mark, so a limit
  or stop filled on the way to a bar's extreme is checked at that extreme on
  its own bar. A host with a margin model can book a margin call a bar
  earlier than before; a requirement hook sees that point.
- **Two C++ calendar queries.** `NativeStrategyHost::native_aggregates_input_bars()`
  says whether the run's script bars are buckets the kernel gathers from its
  input bars, and `native_calendar::native_civil_date` / `native_civil_days`
  convert between days since the epoch and a civil date. Neither has a C
  spelling (the 1.0 C boundary table).
- **The C callback table's marker must be zero.** A C host built against the
  current `pf_native_callbacks_v1` leaves its trailing `reserved1` at 0:
  `strategy_native_host_create_v1` returns NULL for any other value. Tables of
  the three earlier published lengths keep working (#289).

### Behaviour that moves results against 0.13.1

These change a strategy's trades or report values; each commit states the
TradingView behaviour it matches.

- `str.tostring` / `str.format` render numbers by the rule TradingView's tapes
  pin (3872c46a, #287).
- Exact quantities: a Transact that crosses the book is charged exactly its
  units, a close that spans lots and ends inside one exactly its request, and
  a close whose binary64 sum reaches its request exactly at a lot closes that
  lot whole (deb344b1, 99cf9252, 1e9e0761; #287).
- Indicators: `ta::ALMA` takes the floor input, `ta::KC` / `ta::KCW` take
  use_true_range, KC's middle band is its source's EMA on every bar,
  `ta::AnchoredVWAP` restarts on a caller-supplied anchor, and
  `ta::PivotPointLevels` computes an anchored period's pivot levels
  (f2ae16c7, de612473, 24c173ed, be41c1bb, 1eef78eb; #281); the six-argument
  `ta::pivot_point_levels` overload computes every type (adb8f2b6, #287).
- Sessions: `session.isfirstbar` / `session.islastbar` are the kernel's
  session-day facts, a session ends at the session day, and an aggregated
  chart and a stream's realtime bar read `session.islastbar` as TradingView
  does (c68fab7e, b531a8d9, 8b843499, be19463d).
- A margin call never revives an exit the script cancelled (4b00da92, #287),
  whether the exit was live, dormant, or a gapped stop a declined reversal
  had parked for the next margin call (#289).
- Margin: a leveraged position (margin below 100 %) is checked on the rest of
  the bar its fill was on, from the fill to the extreme it was heading for
  (the low for a long, the high for a short), so its margin call lands where
  TradingView books it, not a bar later or never. This holds for an opening
  and for an add to a carried position, under `process_orders_on_close` and
  `calc_on_order_fills` too; a timestamped FX curve keeps its own schedule. A
  default `percent_of_equity` stop entry sized above 100 % keeps the quantity
  it was sized at when placed (at the signal close, or at the snapped stop
  level when not yet marketable), as TradingView does; it was re-sized at the
  fill. A gap-open stop entry's margin slice is sized by the rule every other
  margin call takes, its one-contract minimum included. Under the bar
  magnifier, the margin call of a leveraged long, or of a short at any margin,
  lands at the first lower-timeframe price that crosses its line, and again at
  each later one that crosses the reduced position's line, under
  `calc_on_order_fills` too and, for a long, under `process_orders_on_close`.
  A margin call books the nearest tick of the price it fired at, as
  TradingView's market fill does; stop and limit fills keep their directional
  rounding.
- A trailing `strategy.exit` placed while its entry is still pending starts
  its running best at the activation, as TradingView's does; on TradingView's
  tapes it exited 1 to 24 bars late.
- `calc_on_order_fills`: a market order or a `strategy.close` that a
  recalculation places at a fill on a bar's second high or low fills there,
  as TradingView does; an order an earlier fill of the same bar pushed onto
  that extreme still waits for the close or the next open.
- `process_orders_on_close`: a stop entry whose stop the placing bar's close
  already reached, and the bar's only entry marketable there, fills at that
  close (slippage applied), on the chart, an aggregated chart and under the
  bar magnifier, and so does one placed while the position still holds the
  other side, once a `strategy.close` on the same bar has closed it; each
  filled at the next open. Such an entry placed again at the close of the
  bar whose `strategy.exit` limit closed the previous one keeps the fresh
  bracket it was placed with; the previous bracket's fill withdrew it at the
  next open, and the position ran on without an exit.
  An add can no longer exceed `pyramiding` because an opposite entry was
  resting from an earlier bar. The corpus probe
  `order-deferred-flip-pooc-cross-bar-01` now books all 792 of its
  TradingView tape's trades as TradingView does.
- An aggregated chart under the bar magnifier dates every fill at its chart
  bar's open, as TradingView does.
- A closed trade's run-up and drawdown (`strategy.closedtrades.max_runup` /
  `max_drawdown` and the report's columns) come from the kernel's per-lot
  sampler for Pine strategies, as for every other host. In 19 corpus probes
  the excursions of 335 trades move: 324 now equal TradingView's and 11 are
  closer.
- Under FIFO closing, a default `strategy.exit(from_entry)` reserves its own
  entry's quantity, oldest lots first, not the whole position, as
  TradingView does; where it closed lots of other entries, the trade rows
  split differently.
- A `percent_of_equity` default quantity under a cash commission sizes from
  `strategy.equity`, with the open entries' fees charged, and keeps back the
  fee its own order pays, as TradingView does: per order
  `floor((pct × equity − fee) / (price × point value))`, per contract
  `floor(pct × equity / (price × point value + fee))`. It used to size too
  large. The corpus probe `order-percent-equity-cash-commission-01` now books
  355 of its tape's 366 quantities within TradingView's 0.0001 cell (none
  before).
- `strategy.margin_liquidation_price` reads `na` when margin calls are
  switched off (`set_margin_call_enabled(false)`, which TradingView has no
  counterpart for); it used to return a price.
