# Fix round 2 — documentation anchors

Base and current HEAD: `090106f52be52025622b96e7f3a74933f7e085c8`.
Branch: `core/cr150-perf441-fix1`.
Worktree: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-perf441/wt2`.

Exactly the 18 requested citations were moved across 16 existing lines in the
three named documents. Both citations on native-feature-parity lines 1091 and
1622 were handled independently. No prose, formatting, other anchors, or
document line counts changed.

**No C++ or build file was touched.** Nothing under `src/`, `include/`, or
`tests/`, and no `CMakeLists.txt`, was edited. This round adds only this report
and the `DONE-fix2` handoff marker besides the three document edits.
No git add, commit, or push was run.

## Review and verification boundary

Read the cited C++ windows and their surrounding functions. Reviewed the git
diff against the explicit 18-citation manifest: exactly 16 old/new document
line pairs, each differing only by its specified citation replacements.
The document diff is 16 insertions and 16 deletions across three files.
All 19 quoted C++ windows (18 citation entries plus the writer context) were
re-read and matched the report exactly. `git diff --check` exited 0.

No compiler, build, test binary, CTest, repository script, package install,
remote lab job, corpus sweep, or performance measurement was run.
There are no build/test logs or PASS claims for this round.
The supervisor's committed-SHA lab proof (preflight, release, debug, docs)
remains pending; the doc-anchors checker was not executed here.

Initial git status already contained ` m corpus` and `?? DONE-fix1`.
The corpus gitlink remained
`e99bbd9db257740d9c77db37a8567616627130bf` with internal dirt.
Both pre-existing entries were left untouched and remain outside this round.
Consequently, final git status also includes those two pre-existing entries.
There are no implementation deviations or unresolved target choices.

## Citation evidence

The quoted lines below are C++ read from this tree, not checker suggestions.
Bare `pine_adapter.cpp` and `pine_state_hash.cpp` resolve under `src/source/`;
the bare `market_admission.cpp` citation resolves to
`src/source/market_admission.cpp`, which precedes its compat namesake in the
document checker's search order.

### 1. docs/adr/0001-kernel-adapter-boundary.md:275

Old: `pine_adapter.cpp:24343`
New: `pine_adapter.cpp:24342-24349`

Read `PineExecutionAdapter::on_bar_close_policy`. The range contains the
non-POOC commissioned-short predicate and the adverse-price checkpoint it
selects, rather than only a continuation of the predicate.
Quoted `src/source/pine_adapter.cpp` lines 24342–24349:

```cpp
    const bool non_pooc_commissioned_short = !config_.process_orders_on_close
        && position.signed_units < 0.0
        && config_.margin_short == 100.0
        && config_.commission_type == static_cast<int>(CommissionType::PERCENT)
        && config_.commission_value > 0.0;
    if (non_pooc_commissioned_short && finite_positive(bar.high)) {
        const double adverse = nearest_tick(bar.high, staged_.syminfo.mintick);
        (void)submit_margin_call_slice(adverse, context);
```

### 2. docs/design/cr150-contracts.md:215

Old: `src/source/pine_state_hash.cpp:1184-1190`
New: `src/source/pine_state_hash.cpp:1164-1170`

The host hash folds the cached symbol-data digest inside the symbol-data
domain, after its collection sizes. Quoted `src/source/pine_state_hash.cpp`
lines 1164–1170:

```cpp
    if (!foreign_security_sites_.empty() || !symbol_feeds_.empty()
        || !symbol_facts_.empty() || !recorded_series_.empty()) {
        f.s(kSourceSymbolDataDomain);
        f.u(symbol_feeds_.size());
        f.u(symbol_facts_.size());
        f.u(recorded_series_.size());
        f.u(symbol_data_digest_);
```

### 3. docs/design/native-feature-parity.md:61

Old: `pine_adapter.cpp:27224`
New: `pine_adapter.cpp:27223`

Read `PendingIntentView::copy_v1`; this is the actual POD field assignment.
Quoted `src/source/pine_adapter.cpp` line 27223:

```cpp
    out->pine_birth_reach = static_cast<std::int32_t>(snapshot.birth_reach);
```

### 4. docs/design/native-feature-parity.md:89

Old: `pine_adapter.cpp:26811`
New: `pine_adapter.cpp:26810`

The function's first signature line contains the named probe; the old line
only continued its parameters. Quoted `src/source/pine_adapter.cpp` line 26810:

```cpp
int PendingIntentView::probe_fill_qty(int index, double fill_price, double* qty,
```

### 5. docs/design/native-feature-parity.md:213

Old: `pine_adapter.cpp:27224`
New: `pine_adapter.cpp:27223`

This independent citation names the same POD writer in
`PendingIntentView::copy_v1`. Quoted `src/source/pine_adapter.cpp` line 27223:

```cpp
    out->pine_birth_reach = static_cast<std::int32_t>(snapshot.birth_reach);
```

### 6. docs/design/native-feature-parity.md:214

Old: `src/source/pine_state_hash.cpp:708`
New: `src/source/pine_state_hash.cpp:709-713`

Read `PineExecutionAdapter::hash_state`. The range names the adapter side
table in code and includes its sorted hash fold; the old line was a comment.
Quoted `src/source/pine_state_hash.cpp` lines 709–713:

```cpp
    std::vector<std::uint64_t> market_add_keys(market_pyramid_adds_.begin(),
                                               market_pyramid_adds_.end());
    std::sort(market_add_keys.begin(), market_add_keys.end());
    f.u(market_add_keys.size());
    for (const auto key : market_add_keys) f.u(key);
```

### 7. docs/design/native-feature-parity.md:529

Old: `pine_adapter.cpp:25879`
New: `pine_adapter.cpp:25876-25885`

Read the closed-trade loop in `PineExecutionAdapter::on_applied`. This range
covers the new-loss-day gate, increment, and winning-trade reset, rather
than a precommit read or only the overflow guard.
Quoted `src/source/pine_adapter.cpp` lines 25876–25885:

```cpp
                if (pnl < 0.0 && day != day_ledger_.last_loss_day) {
                    day_ledger_.last_loss_day = day;
                    if (day_ledger_.consecutive_loss_days
                        == std::numeric_limits<int>::max()) {
                        throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                                         "closed trade counter exhausted");
                    }
                    ++day_ledger_.consecutive_loss_days;
                } else if (pnl > 0.0) {
                    day_ledger_.consecutive_loss_days = 0;
```

### 8. docs/design/native-feature-parity.md:530

Old: `pine_adapter.cpp:24856`
New: `pine_adapter.cpp:24844-24856`

Read `PineExecutionAdapter::on_applied`. This range shows removal of the
closing fill's P&L before the loss calculation, then the epsilon and
assignment to the named predicate. The other occurrences only initialize or
consume that predicate. Quoted `src/source/pine_adapter.cpp` lines 24844–24856:

```cpp
        const double after = host.native_marked_equity(event.resolved_price);
        const double before = after - closed_pnl;
        const double loss = day_ledger_.intraday_start_equity - before;
        const double threshold = risk_.max_intraday_loss_percent
            ? day_ledger_.intraday_start_equity * risk_.max_intraday_loss / 100.0
            : risk_.max_intraday_loss;
        if (std::isfinite(risk_.max_intraday_loss) && !std::isfinite(threshold)) {
            engine_command_value_invalid_or_count("risk.threshold",
                EngineValueStage::risk, EngineValueReason::numeric_overflow);
        }
        const double epsilon = 1e-9 * std::max(1.0, std::abs(threshold));
        preclose_intraday_loss = std::isfinite(before) && threshold > 0.0
            && loss > 0.0 && loss + epsilon >= threshold;
```

### 9. docs/design/native-feature-parity.md:1085

Old: `pine_state_hash.cpp:272`
New: `pine_state_hash.cpp:273`

This is the adapter hash definition, rather than the preceding blank line.
Quoted `src/source/pine_state_hash.cpp` line 273:

```cpp
void source::PineExecutionAdapter::hash_state(BrokerStateHashSink& f) const {
```

### 10. docs/design/native-feature-parity.md:1088

Old: `pine_state_hash.cpp:332`
New: `pine_state_hash.cpp:333-335`

Read the placement fold in `PineExecutionAdapter::hash_state`. These lines
name `placement_` and walk its retained rows; the old line was a comment.
Quoted `src/source/pine_state_hash.cpp` lines 333–335:

```cpp
    f.u(placement_.high_water()); f.u(placement_.erased_rows()); f.u(placement_.erased_digest());
    f.u(placement_.size());
    for (const auto& row : placement_) { f.u(row.first); hash_placement(f, row.second); }
```

### 11. docs/design/native-feature-parity.md:1091 — admission citation

Old: `pine_state_hash.cpp:775`
New: `pine_state_hash.cpp:776-777`

The adapter hash's journal frontier and running event digest are the claim's
current fold site. The helper's separate journal reference at line 781
prepares events instead. Quoted `src/source/pine_state_hash.cpp` lines 776–777:

```cpp
    f.u(admission_journal.sequence_frontier());
    f.u(admission_events_folded_); f.u(admission_events_digest_);
```

### 12. docs/design/native-feature-parity.md:1091 — scheduler citation

Old: `pine_state_hash.cpp:886`
New: `pine_state_hash.cpp:866`

Selected the scheduler's hash definition, independently of entry 11 and of
the adapter hash and host call sites.
Quoted `src/source/pine_state_hash.cpp` line 866:

```cpp
void source::PineScheduler::hash_state(BrokerStateHashSink& f) const {
```

### 13. docs/design/native-feature-parity.md:1093

Old: `pine_state_hash.cpp:890-891`
New: `pine_state_hash.cpp:870-873`

Read `PineScheduler::hash_state` and its consumed-digest helper. The chosen
window is inside the named hash function and contains both its consumed
prefix length and its count/digest fold.
Quoted `src/source/pine_state_hash.cpp` lines 870–873:

```cpp
    const std::size_t consumed = std::min(
        retained_n_, static_cast<std::size_t>(std::max(source_bar_count_, 0)));
    f.s("pineforge-pine-scheduler/v3");
    f.u(consumed); f.u(consumed_bars_digest_);
```

### 14. docs/design/native-feature-parity.md:1109

Old: `market_admission.cpp:261-264`
New: `market_admission.cpp:379-380`

Read `Reflect<Writer>` and both writers. These lines are `Reflect::field`
itself, rather than one of the unrelated or writer-specific `field`
occurrences. Quoted `src/source/market_admission.cpp` lines 379–380:

```cpp
    void field(const Path& path, const char (&name)[N], const T& value) const {
        writer.field(path, name, value);
```

Also read the owning string-path writer to establish the surrounding claim:
`src/source/market_admission.cpp` lines 304–312:

```cpp
struct FieldWriter {
    using Path = std::string;
    const FieldVisitor& visit;
    template<class T, std::size_t N>
    void field(const Path& path, const char (&name)[N], const T& value) const {
        if constexpr(std::is_same_v<T,bool>)visit({path+"."+name,uint64_t(value?1:0)});
        else if constexpr(std::is_enum_v<T>||std::is_same_v<T,int>)visit({path+"."+name,int64_t(value)});
        else visit({path+"."+name,value});
    }
```

The citation remains on the claimed `Reflect::field`; no sentence was changed.

### 15. docs/design/native-feature-parity.md:1622 — adapter citation

Old: `pine_state_hash.cpp:272`
New: `pine_state_hash.cpp:273`

Independently points to the adapter hash definition.
Quoted `src/source/pine_state_hash.cpp` line 273:

```cpp
void source::PineExecutionAdapter::hash_state(BrokerStateHashSink& f) const {
```

### 16. docs/design/native-feature-parity.md:1622 — scheduler citation

Old: `pine_state_hash.cpp:886`
New: `pine_state_hash.cpp:866`

Independently points to the scheduler hash definition.
Quoted `src/source/pine_state_hash.cpp` line 866:

```cpp
void source::PineScheduler::hash_state(BrokerStateHashSink& f) const {
```

### 17. docs/design/native-feature-parity.md:1626

Old: `pine_adapter.cpp:26626`
New: `pine_adapter.cpp:26625`

The setter named in the chain writes `risk_.direction`; the old line instead
defined the consecutive-loss-days setter.
Quoted `src/source/pine_adapter.cpp` line 26625:

```cpp
void PineExecutionAdapter::set_risk_direction(int direction) noexcept { risk_.direction = direction; }
```

### 18. docs/design/native-feature-parity.md:1839

Old: `pine_adapter.cpp:24561`
New: `pine_adapter.cpp:24560`

This is the callback definition's named signature line, rather than the
continuation containing only the context parameter.
Quoted `src/source/pine_adapter.cpp` line 24560:

```cpp
void PineExecutionAdapter::on_applied(const native_order::ExecutionAppliedEvent& event,
```
