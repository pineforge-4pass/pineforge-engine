# Fix round 1: documentation citation repair

Branch: `cx/o1-h`. HEAD remains `e524db5ac34e3b006f46b87b18c0e0021e60d49b`; the supervisor commits this diff.

Exactly the supervisor's 162 listed citation occurrences move: 33 in the ADR, 5 in CR150 contracts, 121 in native feature parity, and 3 in native refactor progress. They occupy 104 document lines. Document line counts, prose, path spellings and all unlisted anchors remain unchanged. Each numbered entry below corresponds to the same number in the supervisor's list, including repeated occurrences on one document line.

No C++ or build file was touched in this round: no changes under `src/`, `include/`, `tests/`, or to `CMakeLists.txt`. The corpus gitlink is untouched. The only changed files are the four named documentation files, this report and `DONE-fix1`. No files were staged or committed.

Evidence below quotes the current source text read for each selected line or range. The CMake entries quote CMake code; entry 13 intentionally quotes the explicitly cited calibration comment. All other citation targets include real C++ or CMake code. Short declaration citations identify the relevant member or function; notes distinguish ambiguous definitions and call sites.

The pinned comment moves from `include/pineforge/engine.hpp:738-751` to `include/pineforge/engine.hpp:741-754`. Those fourteen source lines are byte-identical to `ceb399fb70`'s original range. SHA-256 over the lines joined by LF, without a trailing LF, is `sha256:ba90d695d99260d555d1f44bd05110f4977eeb1e54323e6362ea6bd451572399`, exactly the printed pin, so the hash is unchanged. No comment text was edited.

## UNVERIFIED

- Unresolved citation mappings: none; all 162 were resolved from the source text.
- The repository's doc-anchors checker and other preflight stages were not run in this round. A zero-bad-anchor verdict remains for the supervisor's lab proof.
- Release, debug, docs, unit tests and corpus verification were not run in this round. No compiler, CMake, CTest, test binary, repository script or package installer was run on this Mac.

## Citation evidence

### 1. `docs/adr/0001-kernel-adapter-boundary.md:179`

Old: `engine.hpp:1381` → New: `engine.hpp:1384`.

Source read: `include/pineforge/engine.hpp:1384`. Claim: `record_equity_point`.

```cpp
    void record_equity_point(int64_t script_bar_ts) {
```

### 2. `docs/adr/0001-kernel-adapter-boundary.md:197`

Old: `engine.hpp:2044` → New: `engine.hpp:2047`.

Source read: `include/pineforge/engine.hpp:2047`. Claim: `broker_state_hash`.

```cpp
    uint64_t broker_state_hash() const;
```

### 3. `docs/adr/0001-kernel-adapter-boundary.md:202`

Old: `engine.hpp:406` → New: `engine.hpp:409`.

Source read: `include/pineforge/engine.hpp:409`. Claim: `hash_host_extension`.

```cpp
    virtual void hash_host_extension(BrokerStateHashSink&) const;
```

### 4. `docs/adr/0001-kernel-adapter-boundary.md:203`

Old: `engine.hpp:412` → New: `engine.hpp:415`.

Source read: `include/pineforge/engine.hpp:415`. Claim: `hash_source_extension`.

```cpp
    virtual void hash_source_extension(BrokerStateHashSink&) const;
```

### 5. `docs/adr/0001-kernel-adapter-boundary.md:356`

Old: `engine.hpp:379` → New: `engine.hpp:382`.

Source read: `include/pineforge/engine.hpp:382`. Claim: `BacktestEngine`.

```cpp
class BacktestEngine {
```

### 6. `docs/adr/0001-kernel-adapter-boundary.md:373`

Old: `CMakeLists.txt:161` → New: `CMakeLists.txt:162`.

Source read: `CMakeLists.txt:162`. Claim: `add_library`.

```cmake
add_library(pineforge_kernel STATIC ${PINEFORGE_KERNEL_SOURCES})
```

### 7. `docs/adr/0001-kernel-adapter-boundary.md:404`

Old: `engine.hpp:154` → New: `engine.hpp:156-157`.

Source read: `include/pineforge/engine.hpp:156-157`. Claim: `skip_entry_bar_high / skip_entry_bar_low`.

```cpp
    bool skip_entry_bar_high = false;
    bool skip_entry_bar_low = false;
```

### 8. `docs/adr/0001-kernel-adapter-boundary.md:419`

Old: `engine.hpp:1164` → New: `engine.hpp:1167`.

Source read: `include/pineforge/engine.hpp:1167`. Claim: `SecurityEvalState`.

```cpp
    struct SecurityEvalState {
```

### 9. `docs/adr/0001-kernel-adapter-boundary.md:451`

Old: `CMakeLists.txt:390` → New: `CMakeLists.txt:391`.

Source read: `CMakeLists.txt:391`. Claim: `add_subdirectory`.

```cmake
    add_subdirectory(examples/native)
```

### 10. `docs/adr/0001-kernel-adapter-boundary.md:760`

Old: `engine.hpp:412` → New: `engine.hpp:415`.

Source read: `include/pineforge/engine.hpp:415`. Claim: `hash_source_extension`.

```cpp
    virtual void hash_source_extension(BrokerStateHashSink&) const;
```

### 11. `docs/adr/0001-kernel-adapter-boundary.md:767`

Old: `native_execution_consumer.cpp:8836-8848` → New: `native_execution_consumer.cpp:8864-8874`.

Source read: `src/native_execution_consumer.cpp:8864-8874`. Claim: `sub_bars_processed / sample_ticks_processed`.

The counter increments are in deliver_intrabar_script, rather than the unchanged hash fold near line 1131.

```cpp
        ++driver_statistics_.sub_bars_processed;
        engine.diag_magnifier_sub_bars_processed_ =
            static_cast<std::int64_t>(driver_statistics_.sub_bars_processed);
        driver_statistics_.samples_per_sub_bar = static_cast<int>(samples.size());
        callback_context_.driver_statistics = driver_statistics_;
        double previous = samples.front();
        for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
            const double price = samples[sample_index];
            ++driver_statistics_.sample_ticks_processed;
            ++engine.diag_magnifier_sample_ticks_processed_;
            callback_context_.driver_statistics = driver_statistics_;
```

### 12. `docs/adr/0001-kernel-adapter-boundary.md:822`

Old: `include/pineforge/engine.hpp:754` → New: `include/pineforge/engine.hpp:757`.

Source read: `include/pineforge/engine.hpp:757`. Claim: `round_to_mintick`.

```cpp
    double round_to_mintick(double price) const {
```

### 13. `docs/adr/0001-kernel-adapter-boundary.md:822`

Old: `:738-751` → New: `:741-754`.

Source read: `include/pineforge/engine.hpp:741-754`. Claim: `TradingView calibration comment`.

The exact fourteen comment lines are byte-identical to base ceb399fb70; SHA-256 is unchanged.

```cpp
    // Calibration record (finding-446): TradingView's NASDAQ:AAPL and
    // OANDA:EURUSD series carry sub-tick prints (x.xx5 and a few 4-dp
    // values), and every fill taken at one of those RAW BAR PRICES is
    // exactly this function of it: 24,582 /
    // 24,582 half-cent AAPL closes (22,122 rounded up, 2,460 rounded DOWN
    // because the binary quotient lands just under the midpoint —
    // 228.765 / 0.01 = 22876.499999999996 -> 228.76, while
    // 214.385 / 0.01 = 21438.5 -> 214.39) and 142,938 / 142,938 EURUSD
    // fills at tick 1e-5. Any epsilon nudge (floor(r + 0.5 + 1e-6)) or a
    // decimal half-up rule forces every binary midpoint up and breaks the
    // 2,460 down cases. For r >= 1 the form below is bit-identical to
    // std::round(r) (adding 0.5 is exact inside one binade), so the legacy
    // std::round shape was already the right function; it is spelled out so
    // the code reads as the census formula it was fitted to.
```

### 14. `docs/adr/0001-kernel-adapter-boundary.md:826`

Old: `src/native_execution_consumer.cpp:9919-9922` → New: `src/native_execution_consumer.cpp:9953-9965`.

Source read: `src/native_execution_consumer.cpp:9953-9965`. Claim: `projected_first_index`.

The lookahead branch tests this input index and delivers the projected bucket.

```cpp
        if (subscription.lookahead) {
            while (subscription.projected_cursor < subscription.projected_bars.size()
                   && subscription.projected_first_index[subscription.projected_cursor]
                          == index) {
                const std::size_t at = subscription.projected_cursor++;
                if (!deliver_timeframe_bar(engine, subscription,
                                           subscription.projected_bars[at],
                                           subscription.projected_first_ms[at],
                                           bar.timestamp,
                                           subscription.projected_completion[at])) {
                    return false;
                }
                delivered = true;
```

### 15. `docs/adr/0001-kernel-adapter-boundary.md:826`

Old: `:9906-9909` → New: `:9942-9945`.

Source read: `src/native_execution_consumer.cpp:9942-9945`. Claim: `clear_if_gapped`.

The lambda clears both the pull-side latest bar and the evaluator.

```cpp
    const auto clear_if_gapped = [&engine](TimeframeSubscription& subscription) {
        if (subscription.gaps) subscription.latest.reset();
        if (subscription.gaps) engine.clear_security(subscription.sec_id);
    };
```

### 16. `docs/adr/0001-kernel-adapter-boundary.md:830`

Old: `include/pineforge/engine.hpp:154` → New: `include/pineforge/engine.hpp:156-157`.

Source read: `include/pineforge/engine.hpp:156-157`. Claim: `skip_entry_bar_high / skip_entry_bar_low`.

```cpp
    bool skip_entry_bar_high = false;
    bool skip_entry_bar_low = false;
```

### 17. `docs/adr/0001-kernel-adapter-boundary.md:840`

Old: `src/native_execution_consumer.cpp:8297` → New: `src/native_execution_consumer.cpp:8333`.

Source read: `src/native_execution_consumer.cpp:8333`. Claim: `NativeExecutionConsumer::present_session_day`.

```cpp
void NativeExecutionConsumer::present_session_day(NativeDecisionContext& context,
```

### 18. `docs/adr/0001-kernel-adapter-boundary.md:841`

Old: `src/native_execution_consumer.cpp:9073-9086` → New: `src/native_execution_consumer.cpp:9109-9123`.

Source read: `src/native_execution_consumer.cpp:9109-9123`. Claim: `NativeExecutionConsumer::script_bucket_completions`.

```cpp
bool NativeExecutionConsumer::script_bucket_completions(
        const Bar* bars, std::size_t n, std::vector<unsigned char>& completes,
        std::vector<unsigned char>& boundary_completes) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    completes.assign(n, 0U);
    boundary_completes.assign(n, 0U);
    ScriptBucketCursor cursor(*spec);
    for (std::size_t i = 0; i < n; ++i) {
        const auto flags = cursor.next(bars[i]);
        completes[i] = flags.complete;
        boundary_completes[i] = flags.boundary;
    }
    return true;
}
```

### 19. `docs/adr/0001-kernel-adapter-boundary.md:842`

Old: `src/native_execution_consumer.cpp:9048-9069` → New: `src/native_execution_consumer.cpp:9084-9107`.

Source read: `src/native_execution_consumer.cpp:9084-9107`. Claim: `append_open_position_report_rows`.

This is the producer definition, including its account_fx argument, context assignment and close-row builder; not the later caller.

```cpp
double NativeExecutionConsumer::append_open_position_report_rows(
        BacktestEngine& engine, double mark_price, int64_t mark_time_ms,
        int interval_index, double account_fx) const {
    const bool was_long = engine.position_side_ == PositionSide::LONG;
    double marked = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = mark_time_ms;
        context.interval_index = interval_index;
        context.account_fx = account_fx;
        if (!std::isnan(engine.fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = engine.fold_exit_trail_peak_;
        Trade row = engine.build_close_trade_with_costs(
            lot, lot.qty, mark_price, was_long,
            engine.allocated_entry_commission(lot, lot.qty),
            engine.calc_commission_at(mark_price, lot.qty, account_fx), context);
        if (!internal::closed_row_all_finite(row))
            throw std::logic_error("non-finite open-position report row");
        row.open_at_end = true;
        marked += row.pnl;
        engine.range_end_trades_.push_back(std::move(row));
    }
    return marked;
}
```

### 20. `docs/adr/0001-kernel-adapter-boundary.md:843`

Old: `src/native_execution_consumer.cpp:7576` → New: `src/native_execution_consumer.cpp:7612`.

Source read: `src/native_execution_consumer.cpp:7612`. Claim: `NativeExecutionConsumer::execute_current`.

```cpp
NativeCurrentExecutionResult NativeExecutionConsumer::execute_current(
```

### 21. `docs/adr/0001-kernel-adapter-boundary.md:844`

Old: `src/native_execution_consumer.cpp:7299` → New: `src/native_execution_consumer.cpp:7335`.

Source read: `src/native_execution_consumer.cpp:7335`. Claim: `NativeExecutionConsumer::validate_current_execution`.

The definition is the command legality path; the owner allow-list is in its body at lines 7389-7392.

```cpp
std::optional<NativeCurrentRefusal> NativeExecutionConsumer::validate_current_execution(
```

### 22. `docs/adr/0001-kernel-adapter-boundary.md:845`

Old: `include/pineforge/engine.hpp:1219` → New: `include/pineforge/engine.hpp:1222`.

Source read: `include/pineforge/engine.hpp:1222`. Claim: `chart_day_partition_`.

```cpp
    NativeDayPartition chart_day_partition_;
```

### 23. `docs/adr/0001-kernel-adapter-boundary.md:846`

Old: `include/pineforge/engine.hpp:1067` → New: `include/pineforge/engine.hpp:1070`.

Source read: `include/pineforge/engine.hpp:1070`. Claim: `_decompose_bar_time`.

```cpp
    BarTime _decompose_bar_time() const {
```

### 24. `docs/adr/0001-kernel-adapter-boundary.md:846`

Old: `include/pineforge/engine.hpp:1087-1094` → New: `include/pineforge/engine.hpp:1090-1097`.

Source read: `include/pineforge/engine.hpp:1090-1097`. Claim: `_bar_hour through _bar_weekofyear`.

The complete eight-accessor block is cited, not the guessed range extending into magnifier state.

```cpp
    int _bar_hour() const { return _decompose_bar_time().hour; }
    int _bar_minute() const { return _decompose_bar_time().minute; }
    int _bar_second() const { return _decompose_bar_time().second; }
    int _bar_dayofmonth() const { return _decompose_bar_time().dayofmonth; }
    int _bar_dayofweek() const { return _decompose_bar_time().dayofweek; }
    int _bar_month() const { return _decompose_bar_time().month; }
    int _bar_year() const { return _decompose_bar_time().year; }
    int _bar_weekofyear() const { return _decompose_bar_time().weekofyear; }
```

### 25. `docs/adr/0001-kernel-adapter-boundary.md:848`

Old: `include/pineforge/engine.hpp:800` → New: `include/pineforge/engine.hpp:803`.

Source read: `include/pineforge/engine.hpp:803`. Claim: `tick_grid_price`.

```cpp
    double tick_grid_price(double price) const {
```

### 26. `docs/adr/0001-kernel-adapter-boundary.md:848`

Old: `include/pineforge/engine.hpp:834` → New: `include/pineforge/engine.hpp:837`.

Source read: `include/pineforge/engine.hpp:837`. Claim: `price_grid_decimals`.

```cpp
    int price_grid_decimals() const {
```

### 27. `docs/adr/0001-kernel-adapter-boundary.md:848`

Old: `include/pineforge/engine.hpp:844` → New: `include/pineforge/engine.hpp:847`.

Source read: `include/pineforge/engine.hpp:847`. Claim: `level_on_price_grid`.

```cpp
    double level_on_price_grid(double level) const {
```

### 28. `docs/adr/0001-kernel-adapter-boundary.md:848`

Old: `include/pineforge/engine.hpp:933` → New: `include/pineforge/engine.hpp:936`.

Source read: `include/pineforge/engine.hpp:936`. Claim: `apply_qty_step`.

```cpp
    double apply_qty_step(double qty) const {
```

### 29. `docs/adr/0001-kernel-adapter-boundary.md:848`

Old: `include/pineforge/engine.hpp:953` → New: `include/pineforge/engine.hpp:956`.

Source read: `include/pineforge/engine.hpp:956`. Claim: `apply_exit_qty_step`.

```cpp
    double apply_exit_qty_step(double qty) const {
```

### 30. `docs/adr/0001-kernel-adapter-boundary.md:944`

Old: `src/native_execution_consumer.cpp:2476` → New: `src/native_execution_consumer.cpp:2512`.

Source read: `src/native_execution_consumer.cpp:2512`. Claim: `NativeExecutionConsumer::prepare_day_labels`.

Definition that reads the installed daily feed.

```cpp
void NativeExecutionConsumer::prepare_day_labels(const BacktestEngine& engine,
```

### 31. `docs/adr/0001-kernel-adapter-boundary.md:944`

Old: `src/native_execution_consumer.cpp:3122` → New: `src/native_execution_consumer.cpp:3158`.

Source read: `src/native_execution_consumer.cpp:3158`. Claim: `prepare_day_labels`.

Call at the end of begin_ready after subscription feeds are installed.

```cpp
    prepare_day_labels(engine, begin_bars, begin_n);
```

### 32. `docs/adr/0001-kernel-adapter-boundary.md:944`

Old: `src/native_execution_consumer.cpp:2426` → New: `src/native_execution_consumer.cpp:2462`.

Source read: `src/native_execution_consumer.cpp:2462`. Claim: `script_interval_resolved`.

```cpp
NativeExecutionConsumer::script_interval_resolved(std::int64_t timestamp) const {
```

### 33. `docs/adr/0001-kernel-adapter-boundary.md:944`

Old: `src/native_execution_consumer.cpp:2529` → New: `src/native_execution_consumer.cpp:2565`.

Source read: `src/native_execution_consumer.cpp:2565`. Claim: `day_label_origin`.

```cpp
std::int64_t NativeExecutionConsumer::day_label_origin(std::int64_t timestamp) const {
```

### 34. `docs/design/cr150-contracts.md:211`

Old: `src/native_execution_consumer.cpp:9621-9624` → New: `src/native_execution_consumer.cpp:9657-9660`.

Source read: `src/native_execution_consumer.cpp:9657-9660`. Claim: `validate_native_timeframe_subscriptions`.

Call in declare_timeframe_subscriptions.

```cpp
    result.validation = native_bar_views_v1::validate_native_timeframe_subscriptions(
        declared, running->spec.input_tf, running->spec.timeframe_undetected,
        running->spec.auxiliary_feed, running->spec.instrument_feeds, bar_views());
    if (!result.validation) return result;
```

### 35. `docs/design/cr150-contracts.md:211`

Old: `src/native_execution_consumer.cpp:9646-9648` → New: `src/native_execution_consumer.cpp:9682-9685`.

Source read: `src/native_execution_consumer.cpp:9682-9685`. Claim: `validate_native_timeframe_subscriptions`.

Distinct call in declare_auxiliary_feed, validating the replacement auxiliary feed.

```cpp
    result.validation = native_bar_views_v1::validate_native_timeframe_subscriptions(
        running->spec.subscriptions, running->spec.input_tf,
        running->spec.timeframe_undetected, declared, running->spec.instrument_feeds, bar_views());
    if (!result.validation) return result;
```

### 36. `docs/design/cr150-contracts.md:213`

Old: `src/native_execution_consumer.cpp:2788-2789` → New: `src/native_execution_consumer.cpp:2824-2825`.

Source read: `src/native_execution_consumer.cpp:2824-2825`. Claim: `native_instrument_feeds_digest`.

Configure-time cached instrument digest component.

```cpp
        components.instruments = native_bar_views_v1::native_instrument_feeds_digest(
            candidate.instrument_feeds, views);
```

### 37. `docs/design/cr150-contracts.md:213`

Old: `src/native_execution_consumer.cpp:13363-13368` → New: `src/native_execution_consumer.cpp:13399-13404`.

Source read: `src/native_execution_consumer.cpp:13399-13404`. Claim: `native_run_spec_digest`.

The original no-view overload folds hash_spec directly; not the neighboring NativeBarViews overload.

```cpp
uint64_t native_run_spec_digest(const NativeRunSpec& spec) noexcept {
    Fnv f;
    f.run_base = spec.identity.run_number;
    hash_spec(f, spec);
    return f.h;
}
```

### 38. `docs/design/cr150-contracts.md:219`

Old: `src/native_execution_consumer.cpp:8698-8731` → New: `src/native_execution_consumer.cpp:8758-8764`.

Source read: `src/native_execution_consumer.cpp:8758-8764`. Claim: `candidate`.

The lower-feed vector branch's lower_bound and bounded walk, not another candidate variable.

```cpp
            auto candidate = std::lower_bound(
                lower->bars.begin(), lower->bars.end(), begin,
                [](const Bar& bar, int64_t time) { return bar.timestamp < time; });
            for (; candidate != lower->bars.end() && candidate->timestamp < end; ++candidate) {
                sub_bars.push_back(&*candidate);
            }
        }
```

### 39. `docs/design/native-feature-parity.md:50`

Old: `native_execution_consumer.cpp:6647` → New: `native_execution_consumer.cpp:6683-6684`.

Source read: `src/native_execution_consumer.cpp:6683-6684`. Claim: `first_region_entry`.

```cpp
            hit = native_matching::first_region_entry(
                from_price, to_price, start, stop->price, !buy, include_current, grid);
```

### 40. `docs/design/native-feature-parity.md:60`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 41. `docs/design/native-feature-parity.md:69`

Old: `native_execution_consumer.cpp:6656` → New: `native_execution_consumer.cpp:6692-6714`.

Source read: `src/native_execution_consumer.cpp:6692-6714`. Claim: `TrailWaitArm`.

This reach_trigger block covers arm, track and trail_stop_hit.

```cpp
        } else if (std::holds_alternative<native_order::TrailWaitArm>(state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            if (!trail) return TriggerReach::Absent;
            if (!trail->arm_price) {
                hit = start;
            } else {
                hit = native_matching::first_region_entry(
                    from_price, to_price, start, *trail->arm_price, buy,
                    include_current, grid);
            }
            kind = Kind::BeginTrail;
        } else if (const auto* track = std::get_if<native_order::TrailTrack>(&state)) {
            const auto* trail = std::get_if<native_order::Trail>(&trigger);
            if (!trail) return TriggerReach::Absent;
            double stop = 0.0;
            if (!native_matching::checked_trail_stop(track->best, trail->offset, buy,
                                                    &stop, ladder_tick())) {
                return TriggerReach::Unrepresentable;
            }
            hit = native_matching::trail_stop_hit(
                from_price, to_price, start, track->best, trail->offset, buy, grid,
                ladder_tick());
            kind = Kind::ActivateTrail;
```

### 42. `docs/design/native-feature-parity.md:71`

Old: `native_execution_consumer.cpp:5391` → New: `native_execution_consumer.cpp:5427-5430`.

Source read: `src/native_execution_consumer.cpp:5427-5430`. Claim: `checked_trail_stop`.

The active-trail trigger check throws on an unrepresentable stop in the terms coordinator.

```cpp
        if (!trail || !native_matching::checked_trail_stop(
                active->best_at_trigger, trail->offset, out.is_buy, &stop, ladder_tick())) {
            throw std::logic_error("native active trail has no representable trigger");
        }
```

### 43. `docs/design/native-feature-parity.md:80`

Old: `native_execution_consumer.cpp:5876` → New: `native_execution_consumer.cpp:5912`.

Source read: `src/native_execution_consumer.cpp:5912`. Claim: `resolve_execution_terms`.

Actual host-hook invocation in consume_matched_request, not the preview hook.

```cpp
            terms = host->resolve_execution_terms(terms_facts);
```

### 44. `docs/design/native-feature-parity.md:80`

Old: `native_execution_consumer.cpp:5777-5811` → New: `native_execution_consumer.cpp:5909-5923`.

Source read: `src/native_execution_consumer.cpp:5909-5923`. Claim: `try`.

The local try and both catches directly wrap that same host terms callback.

```cpp
        try {
            auto* host = native_host(engine);
            if (!host) throw std::logic_error("native terms require a native host");
            terms = host->resolve_execution_terms(terms_facts);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render_exception(engine, e);
            return std::nullopt;
        } catch (...) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Settlement, P});
            render_unclassified(engine, "native terms callback exception");
            return std::nullopt;
        }
```

### 45. `docs/design/native-feature-parity.md:81`

Old: `native_execution_consumer.cpp:5905` → New: `native_execution_consumer.cpp:5940-5942`.

Source read: `src/native_execution_consumer.cpp:5940-5942`. Claim: `TermsUnresolved`.

The matched-request terminal rejection, not inspect_current_execution's preview.

```cpp
        if (unresolved && !terms.units) {
            return terminal(native_order::MatchRejectReason::TermsUnresolved, terms);
        }
```

### 46. `docs/design/native-feature-parity.md:97`

Old: `native_execution_consumer.cpp:3757` → New: `native_execution_consumer.cpp:3791-3799`.

Source read: `src/native_execution_consumer.cpp:3791-3799`. Claim: `OpeningDirection`.

Both long and short direction refusals in admit_opening_inspect.

```cpp
    const auto mask = static_cast<uint32_t>(spec->allowed_open_directions);
    if (inspect.incoming_short && (mask & 2u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (!inspect.incoming_short && (mask & 1u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
```

### 47. `docs/design/native-feature-parity.md:98`

Old: `native_execution_consumer.cpp:3764` → New: `native_execution_consumer.cpp:3800-3803`.

Source read: `src/native_execution_consumer.cpp:3800-3803`. Claim: `max_abs_units`.

```cpp
    if (spec->max_abs_units && inspect.resulting_abs_units > *spec->max_abs_units) {
        if (reason) *reason = native_order::MatchRejectReason::MaxAbsUnits;
        return false;
    }
```

### 48. `docs/design/native-feature-parity.md:99`

Old: `native_execution_consumer.cpp:3769` → New: `native_execution_consumer.cpp:3804-3807`.

Source read: `src/native_execution_consumer.cpp:3804-3807`. Claim: `MaxOpenLots`.

```cpp
    if (spec->max_open_lots && inspect.resulting_lot_count > *spec->max_open_lots) {
        if (reason) *reason = native_order::MatchRejectReason::MaxOpenLots;
        return false;
    }
```

### 49. `docs/design/native-feature-parity.md:100`

Old: `native_execution_consumer.cpp:3787` → New: `native_execution_consumer.cpp:3821-3831`.

Source read: `src/native_execution_consumer.cpp:3821-3831`. Claim: `resulting_abs_notional * fraction`.

```cpp
    if (!skip_initial_margin && fraction > 0.0) {
        const double equity = engine.marked_equity_at(resolved_price, fx) - inspect.current_ticket;
        const double required = inspect.resulting_abs_notional * fraction;
        if (!std::isfinite(equity) || !std::isfinite(required) || required > equity) {
            if (!std::isfinite(equity) || !std::isfinite(required)) {
                engine_command_value_invalid_or_count("margin.required", EngineValueStage::margin,
                                                     EngineValueReason::nonfinite);
            }
            if (reason) *reason = native_order::MatchRejectReason::InitialMargin;
            return false;
        }
```

### 50. `docs/design/native-feature-parity.md:100`

Old: `native_execution_consumer.cpp:3633` → New: `native_execution_consumer.cpp:3669`.

Source read: `src/native_execution_consumer.cpp:3669`. Claim: `make_command_context`.

```cpp
native_order::CommandContext NativeExecutionConsumer::make_command_context(
```

### 51. `docs/design/native-feature-parity.md:100`

Old: `engine.hpp:650` → New: `engine.hpp:653-655`.

Source read: `include/pineforge/engine.hpp:653-655`. Claim: `inspect_native_settlement_scoped_at`.

```cpp
    execution::SettlementInspection inspect_native_settlement_scoped_at(
        const execution::Action& action, const execution::Fill& fill,
        execution::CloseScope scope, double fx) const;
```

### 52. `docs/design/native-feature-parity.md:102`

Old: `native_execution_consumer.cpp:3746` → New: `native_execution_consumer.cpp:3781-3782`.

Source read: `src/native_execution_consumer.cpp:3781-3782`. Claim: `would_open`.

```cpp
    const auto* spec = spec_ptr();
    if (!spec || !inspect.would_open) return true;
```

### 53. `docs/design/native-feature-parity.md:105`

Old: `engine.hpp:730` → New: `engine.hpp:733`.

Source read: `include/pineforge/engine.hpp:733`. Claim: `BacktestEngine::marked_equity_at`.

```cpp
    double marked_equity_at(double price, double fx) const;
```

### 54. `docs/design/native-feature-parity.md:118`

Old: `native_execution_consumer.cpp:8217` → New: `native_execution_consumer.cpp:8253`.

Source read: `src/native_execution_consumer.cpp:8253`. Claim: `invoke_callback`.

```cpp
void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
```

### 55. `docs/design/native-feature-parity.md:118`

Old: `native_execution_consumer.cpp:8245` → New: `native_execution_consumer.cpp:8281-8282`.

Source read: `src/native_execution_consumer.cpp:8281-8282`. Claim: `on_native_recalculate`.

BarClose invocation, not the modeled-point recalculation hook.

```cpp
        host->on_native_recalculate(bar, presented, NativeCalculationReason::BarClose,
                                    nullptr);
```

### 56. `docs/design/native-feature-parity.md:119`

Old: `native_execution_consumer.cpp:8661` → New: `native_execution_consumer.cpp:8697-8701`.

Source read: `src/native_execution_consumer.cpp:8697-8701`. Claim: `AfterCalculation`.

```cpp
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        emit_discrete(bar.close, close_time, NativePriceProvenance::AfterCalculationClose,
                      NativePathPhase::Close, true);
        if (failed()) return;
    }
```

### 57. `docs/design/native-feature-parity.md:120`

Old: `native_execution_consumer.cpp:8091` → New: `native_execution_consumer.cpp:8127`.

Source read: `src/native_execution_consumer.cpp:8127`. Claim: `on_native_bar_open`.

```cpp
        host->on_native_bar_open(open_view, presented);
```

### 58. `docs/design/native-feature-parity.md:121`

Old: `native_execution_consumer.cpp:8034` → New: `native_execution_consumer.cpp:8070`.

Source read: `src/native_execution_consumer.cpp:8070`. Claim: `invoke_bar_open_callback`.

```cpp
void NativeExecutionConsumer::invoke_bar_open_callback(
```

### 59. `docs/design/native-feature-parity.md:121`

Old: `native_execution_consumer.cpp:8085` → New: `native_execution_consumer.cpp:8121`.

Source read: `src/native_execution_consumer.cpp:8121`. Claim: `engine.current_bar_ = open_view`.

```cpp
    engine.current_bar_ = open_view;
```

### 60. `docs/design/native-feature-parity.md:123`

Old: `native_execution_consumer.cpp:7938` → New: `native_execution_consumer.cpp:7974`.

Source read: `src/native_execution_consumer.cpp:7974`. Claim: `on_native_applied`.

First occurrence on this document line.

```cpp
        host->on_native_applied(applied, presented);
```

### 61. `docs/design/native-feature-parity.md:123`

Old: `native_execution_consumer.cpp:7938` → New: `native_execution_consumer.cpp:7974`.

Source read: `src/native_execution_consumer.cpp:7974`. Claim: `on_native_applied`.

Second occurrence on this document line.

```cpp
        host->on_native_applied(applied, presented);
```

### 62. `docs/design/native-feature-parity.md:123`

Old: `native_execution_consumer.cpp:1857` → New: `native_execution_consumer.cpp:1893-1904`.

Source read: `src/native_execution_consumer.cpp:1893-1904`. Claim: `commands_allowed`.

```cpp
bool NativeExecutionConsumer::commands_allowed() const {
    // A command asked for from inside a capture or an observer call is refused
    // and latched, so the terminal helper rejects that generation; every
    // ordinary admission below is unchanged.
    if (refuse_dispatch_reentry()) return false;
    if (failed() || consuming_request_) return false;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return false;
    if (in_callback_) return true;
    if (processing_input_) return false;
    return running->phase == NativeRunPhase::Realtime;
}
```

### 63. `docs/design/native-feature-parity.md:123`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 64. `docs/design/native-feature-parity.md:124`

Old: `native_execution_consumer.cpp:8173` → New: `native_execution_consumer.cpp:8209`.

Source read: `src/native_execution_consumer.cpp:8209`. Claim: `on_native_tick`.

```cpp
        host->on_native_tick(bar, presented);
```

### 65. `docs/design/native-feature-parity.md:125`

Old: `native_execution_consumer.cpp:8768` → New: `native_execution_consumer.cpp:8803-8804`.

Source read: `src/native_execution_consumer.cpp:8803-8804`. Claim: `IntrabarPath`.

The intrabar delivery path's sample-eligibility choice, not the hash_spec field.

```cpp
    const bool distribution_samples = synthesized || lower->sample_eligibility
        == IntrabarPath::SampleEligibility::DistributionSamples;
```

### 66. `docs/design/native-feature-parity.md:127`

Old: `engine.hpp:1772` → New: `engine.hpp:1775-1777`.

Source read: `include/pineforge/engine.hpp:1775-1777`. Claim: `stream_begin`.

```cpp
    bool stream_begin(const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf = "");
```

### 67. `docs/design/native-feature-parity.md:127`

Old: `engine.hpp:1778` → New: `engine.hpp:1781`.

Source read: `include/pineforge/engine.hpp:1781`. Claim: `stream_push_bar`.

```cpp
    bool stream_push_bar(const Bar& bar);
```

### 68. `docs/design/native-feature-parity.md:127`

Old: `engine.hpp:1782` → New: `engine.hpp:1785`.

Source read: `include/pineforge/engine.hpp:1785`. Claim: `stream_end`.

```cpp
    bool stream_end(bool finalize_partial_input_bar = false);
```

### 69. `docs/design/native-feature-parity.md:128`

Old: `engine.hpp:1333-1334` → New: `engine.hpp:1336-1338`.

Source read: `include/pineforge/engine.hpp:1336-1338`. Claim: `snapshot_script_state`.

The contiguous snapshot, restore and commit hooks.

```cpp
    virtual void snapshot_script_state() {}
    virtual void restore_script_state() {}
    virtual void commit_script_state() {}
```

### 70. `docs/design/native-feature-parity.md:134`

Old: `engine.hpp:1323` → New: `engine.hpp:1326`.

Source read: `include/pineforge/engine.hpp:1326`. Claim: `configure_security_evaluators`.

```cpp
    virtual void configure_security_evaluators() {}
```

### 71. `docs/design/native-feature-parity.md:135`

Old: `engine.hpp:1732` → New: `engine.hpp:1735-1736`.

Source read: `include/pineforge/engine.hpp:1735-1736`. Claim: `set_native_security_feed`.

```cpp
    bool set_native_security_feed(const std::string& timeframe,
                                  const Bar* bars, int n);
```

### 72. `docs/design/native-feature-parity.md:135`

Old: `native_execution_consumer.cpp:1892` → New: `native_execution_consumer.cpp:1928-1944`.

Source read: `src/native_execution_consumer.cpp:1928-1944`. Claim: `refuse_source_mutation`.

The pre-run staging guard and its return; the later refusal is cited separately.

```cpp
void NativeExecutionConsumer::refuse_source_mutation(const char* operation) {
    // Source-free ingress is staging while no run is active: before a first
    // begin, between completed runs, and after a cooperative abort. The
    // existing in-run refusal stays fail-closed.
    if (recoverable_abort()
        || (!failed() && (std::holds_alternative<NativeUnconfigured>(state_)
                          || std::holds_alternative<NativeReady>(state_)
                          || std::holds_alternative<NativeCompleted>(state_)
                          || preparing_begin_
                          // The kernel's own registration of the declared
                          // higher-timeframe series, which runs after the run
                          // is Running and calls no host callback inside its
                          // scope. It is the consumer writing the engine's
                          // feed store, never a source host mutating a run.
                          || wiring_subscriptions_))) {
        return;
    }
```

### 73. `docs/design/native-feature-parity.md:135`

Old: `engine.hpp:1640` → New: `engine.hpp:1643`.

Source read: `include/pineforge/engine.hpp:1643`. Claim: `prepare_native_security_feeds`.

```cpp
    void prepare_native_security_feeds(const Bar* input_bars, int n_input);
```

### 74. `docs/design/native-feature-parity.md:135`

Old: `native_execution_consumer.cpp:1910` → New: `native_execution_consumer.cpp:1945-1948`.

Source read: `src/native_execution_consumer.cpp:1945-1948`. Claim: `UnsupportedSource`.

```cpp
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
```

### 75. `docs/design/native-feature-parity.md:135`

Old: `native_execution_consumer.cpp:9575` → New: `native_execution_consumer.cpp:9611`.

Source read: `src/native_execution_consumer.cpp:9611`. Claim: `prepare_native_security_feeds`.

The consumer's begin-time feed preparation call.

```cpp
        engine.prepare_native_security_feeds(input_bars, n_input);
```

### 76. `docs/design/native-feature-parity.md:139`

Old: `native_execution_consumer.cpp:9073` → New: `native_execution_consumer.cpp:9109`.

Source read: `src/native_execution_consumer.cpp:9109`. Claim: `script_bucket_completions`.

```cpp
bool NativeExecutionConsumer::script_bucket_completions(
```

### 77. `docs/design/native-feature-parity.md:139`

Old: `native_execution_consumer.cpp:9080` → New: `native_execution_consumer.cpp:9116`.

Source read: `src/native_execution_consumer.cpp:9116`. Claim: `ScriptBucketCursor`.

```cpp
    ScriptBucketCursor cursor(*spec);
```

### 78. `docs/design/native-feature-parity.md:145`

Old: `native_execution_consumer.cpp:6647` → New: `native_execution_consumer.cpp:6683-6684`.

Source read: `src/native_execution_consumer.cpp:6683-6684`. Claim: `native_matching`.

The actual geometric matching call, rather than the nearby GeometricHit type in the lambda signature.

```cpp
            hit = native_matching::first_region_entry(
                from_price, to_price, start, stop->price, !buy, include_current, grid);
```

### 79. `docs/design/native-feature-parity.md:145`

Old: `native_execution_consumer.cpp:7237` → New: `native_execution_consumer.cpp:7273-7281`.

Source read: `src/native_execution_consumer.cpp:7273-7281`. Claim: `NativeCandidatePriceKind`.

```cpp
        native_order::NativeCandidatePriceKind price_kind =
            native_order::NativeCandidatePriceKind::PointPrice;
        if (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state)
            || std::holds_alternative<native_order::StopActive>(live->trigger_state)
            || std::holds_alternative<native_order::TrailActive>(live->trigger_state)) {
            price_kind = winner->at_level
                ? native_order::NativeCandidatePriceKind::TriggerLevel
                : native_order::NativeCandidatePriceKind::PointPrice;
```

### 80. `docs/design/native-feature-parity.md:145`

Old: `native_execution_consumer.cpp:7233` → New: `native_execution_consumer.cpp:7268-7271`.

Source read: `src/native_execution_consumer.cpp:7268-7271`. Claim: `protect_limit`.

```cpp
            if (!fill_through) {
                resolved = native_matching::protect_limit(
                    resolved, grid_limit_cap(*spec, level, buy), buy);
            }
```

### 81. `docs/design/native-feature-parity.md:145`

Old: `native_execution_consumer.cpp:5902` → New: `native_execution_consumer.cpp:6063-6082`.

Source read: `src/native_execution_consumer.cpp:6063-6082`. Claim: `InvalidTerms`.

This post-hook limit-price check reads fill_through and rejects an out-of-limit resolved price; the guessed line 5938 rejects unrelated units/shape overrides.

```cpp
        if (!no_plan_terminal
            && (std::holds_alternative<native_order::LimitReady>(live->trigger_state)
            || std::holds_alternative<native_order::StopLimitLive>(live->trigger_state))) {
            std::optional<double> level;
            bool fill_through = false;
            if (const auto* limit = std::get_if<native_order::Limit>(&live->request().trigger)) {
                level = limit->price;
                fill_through = limit->fill_through;
            } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(
                           &live->request().trigger)) {
                level = stop_limit->limit;
            }
            // A fill-through limit is a touch trigger: its terms may settle
            // past the level.
            if (!level || (!fill_through
                           && ((terms_facts.is_buy && resolved_price > *level)
                               || (!terms_facts.is_buy && resolved_price < *level)))) {
                return terminal(native_order::MatchRejectReason::InvalidTerms, terms);
            }
        }
```

### 82. `docs/design/native-feature-parity.md:146`

Old: `native_execution_consumer.cpp:5538` → New: `native_execution_consumer.cpp:7251-7255`.

Source read: `src/native_execution_consumer.cpp:7251-7255`. Claim: `slippage_ticks`.

The matched-fill basis and its single slippage application support this fill-pricing claim; the shifted line 5574 belongs to sizing_point_price.

```cpp
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        // An opted-in grid books on the tick ladder BEFORE slippage, which is
        // itself a whole number of ticks.
        const double basis = grid_fill_basis(*spec, winner->price, buy, limit_governed);
        double resolved = native_matching::apply_slippage(basis, slip, buy);
```

### 83. `docs/design/native-feature-parity.md:146`

Old: `native_execution_consumer.cpp:7233` → New: `native_execution_consumer.cpp:7268-7271`.

Source read: `src/native_execution_consumer.cpp:7268-7271`. Claim: `protect_limit`.

```cpp
            if (!fill_through) {
                resolved = native_matching::protect_limit(
                    resolved, grid_limit_cap(*spec, level, buy), buy);
            }
```

### 84. `docs/design/native-feature-parity.md:148`

Old: `native_execution_consumer.cpp:7215` → New: `native_execution_consumer.cpp:7251`.

Source read: `src/native_execution_consumer.cpp:7251`. Claim: `price_tick`.

```cpp
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
```

### 85. `docs/design/native-feature-parity.md:148`

Old: `engine.hpp:772` → New: `engine.hpp:775`.

Source read: `include/pineforge/engine.hpp:775`. Claim: `bar_fill_price`.

```cpp
    double bar_fill_price(double raw_bar_price) const {
```

### 86. `docs/design/native-feature-parity.md:148`

Old: `native_execution_consumer.cpp:7388` → New: `native_execution_consumer.cpp:7424-7425`.

Source read: `src/native_execution_consumer.cpp:7424-7425`. Claim: `NativeCurrentPriceRule::NearestTick`.

```cpp
    const double basis = rule == NativeCurrentPriceRule::NearestTick
        ? engine.bar_fill_price(current_frame_->point.price) : current_frame_->point.price;
```

### 87. `docs/design/native-feature-parity.md:151`

Old: `native_execution_consumer.cpp:10015` → New: `native_execution_consumer.cpp:10051-10057`.

Source read: `src/native_execution_consumer.cpp:10051-10057`. Claim: `refuse_fx_curve_tick_input`.

```cpp
bool NativeExecutionConsumer::refuse_fx_curve_tick_input(BacktestEngine& engine) {
    if (!staged_fx_curve_) return false;
    present_refusal(engine,
        "a declared native FX curve requires confirmed-bar stream input",
        RunFailureCode::run_mode_unsupported, {{"feature", "stream_fx_curve_input"}});
    return true;
}
```

### 88. `docs/design/native-feature-parity.md:158`

Old: `native_execution_consumer.cpp:3416` → New: `native_execution_consumer.cpp:3452`.

Source read: `src/native_execution_consumer.cpp:3452`. Claim: `apply_excursion`.

```cpp
void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
```

### 89. `docs/design/native-feature-parity.md:160`

Old: `engine.hpp:1363` → New: `engine.hpp:1366`.

Source read: `include/pineforge/engine.hpp:1366`. Claim: `update_equity_extremes`.

```cpp
    void update_equity_extremes() {
```

### 90. `docs/design/native-feature-parity.md:160`

Old: `engine.hpp:1381` → New: `engine.hpp:1384`.

Source read: `include/pineforge/engine.hpp:1384`. Claim: `record_equity_point`.

```cpp
    void record_equity_point(int64_t script_bar_ts) {
```

### 91. `docs/design/native-feature-parity.md:162`

Old: `engine.hpp:1832` → New: `engine.hpp:1835-1837`.

Source read: `include/pineforge/engine.hpp:1835-1837`. Claim: `report_trade_count`.

```cpp
    int report_trade_count() const {
        return (int)(trades_.size() + range_end_trades_.size());
    }
```

### 92. `docs/design/native-feature-parity.md:163`

Old: `engine.hpp:1392` → New: `engine.hpp:1395-1407`.

Source read: `include/pineforge/engine.hpp:1395-1407`. Claim: `closed_trade_*`.

The protected closed-trade accessor definitions, not an unrelated member.

```cpp
    double closed_trade_profit(int index) const {
        if (index >= 0 && index < (int)trades_.size())
            return trades_[index].pnl;
        return 0.0;
    }
    double closed_trade_profit_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].pnl_pct;
    }
    double closed_trade_commission(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].commission;
    }
```

### 93. `docs/design/native-feature-parity.md:163`

Old: `engine.hpp:1850` → New: `engine.hpp:1853`.

Source read: `include/pineforge/engine.hpp:1853`. Claim: `closed_trade_close_cause`.

```cpp
    int closed_trade_close_cause(int i) const;
```

### 94. `docs/design/native-feature-parity.md:164`

Old: `native_execution_consumer.cpp:10751` → New: `native_execution_consumer.cpp:10787`.

Source read: `src/native_execution_consumer.cpp:10787`. Claim: `stream_observe_actions_`.

```cpp
        engine.stream_observe_actions_ = true;
```

### 95. `docs/design/native-feature-parity.md:165`

Old: `native_execution_consumer.cpp:12334` → New: `native_execution_consumer.cpp:12370-12373`.

Source read: `src/native_execution_consumer.cpp:12370-12373`. Claim: `native_continuation_hash`.

```cpp
uint64_t NativeStrategyHost::native_continuation_hash() const {
    return NativeExecutionConsumer::bound(*this)
        .calc_continuation_hash();
}
```

### 96. `docs/design/native-feature-parity.md:167`

Old: `engine.hpp:412` → New: `engine.hpp:415`.

Source read: `include/pineforge/engine.hpp:415`. Claim: `hash_source_extension`.

First occurrence on this document line.

```cpp
    virtual void hash_source_extension(BrokerStateHashSink&) const;
```

### 97. `docs/design/native-feature-parity.md:167`

Old: `engine.hpp:406` → New: `engine.hpp:409`.

Source read: `include/pineforge/engine.hpp:409`. Claim: `hash_host_extension`.

```cpp
    virtual void hash_host_extension(BrokerStateHashSink&) const;
```

### 98. `docs/design/native-feature-parity.md:167`

Old: `engine.hpp:412` → New: `engine.hpp:415`.

Source read: `include/pineforge/engine.hpp:415`. Claim: `hash_source_extension`.

Second occurrence on this document line.

```cpp
    virtual void hash_source_extension(BrokerStateHashSink&) const;
```

### 99. `docs/design/native-feature-parity.md:173`

Old: `engine.hpp:1872` → New: `engine.hpp:1875-1878`.

Source read: `include/pineforge/engine.hpp:1875-1878`. Claim: `set_input`.

```cpp
    void set_input(const std::string& key, const std::string& value) {
        guard_native_mutation("set_input");
        inputs_[key] = value;
    }
```

### 100. `docs/design/native-feature-parity.md:173`

Old: `engine.hpp:1877` → New: `engine.hpp:1876`.

Source read: `include/pineforge/engine.hpp:1876`. Claim: `guard_native_mutation`.

The guard in set_input, rather than the neighboring clear_inputs guard.

```cpp
        guard_native_mutation("set_input");
```

### 101. `docs/design/native-feature-parity.md:176`

Old: `engine.hpp:2175` → New: `engine.hpp:2178`.

Source read: `include/pineforge/engine.hpp:2178`. Claim: `request_abort`.

```cpp
    void request_abort() { abort_requested_.store(true, std::memory_order_relaxed); }
```

### 102. `docs/design/native-feature-parity.md:179`

Old: `native_execution_consumer.cpp:8297` → New: `native_execution_consumer.cpp:8333`.

Source read: `src/native_execution_consumer.cpp:8333`. Claim: `present_session_day`.

```cpp
void NativeExecutionConsumer::present_session_day(NativeDecisionContext& context,
```

### 103. `docs/design/native-feature-parity.md:188`

Old: `CMakeLists.txt:161` → New: `CMakeLists.txt:162`.

Source read: `CMakeLists.txt:162`. Claim: `pineforge_kernel`.

```cmake
add_library(pineforge_kernel STATIC ${PINEFORGE_KERNEL_SOURCES})
```

### 104. `docs/design/native-feature-parity.md:205`

Old: `engine.hpp:1797-1802` → New: `engine.hpp:1804-1816`.

Source read: `include/pineforge/engine.hpp:1804-1816`. Claim: `run`.

The full rich overload declaration includes const void* overrides, rather than a different run overload.

```cpp
    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             const std::unordered_map<std::string, std::string>& inputs,
             const SymInfo& syminfo,
             // Opaque host-override handle: the kernel only forwards it to
             // prepare_native_begin() as NativeBeginArgs::overrides_opaque
             // and never dereferences it. A host layer that defines its own
             // override record passes its address and casts it back there.
             const void* overrides = nullptr,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);
```

### 105. `docs/design/native-feature-parity.md:207`

Old: `native_execution_consumer.cpp:2382` → New: `native_execution_consumer.cpp:2418-2421`.

Source read: `src/native_execution_consumer.cpp:2418-2421`. Claim: `spec_tolerant_slot_labels`.

```cpp
bool NativeExecutionConsumer::spec_tolerant_slot_labels() const noexcept {
    const auto* spec = spec_ptr();
    return spec && spec->slot_label_policy == NativeSlotLabelPolicy::FeedTolerant;
}
```

### 106. `docs/design/native-feature-parity.md:207`

Old: `native_execution_consumer.cpp:2399` → New: `native_execution_consumer.cpp:2435`.

Source read: `src/native_execution_consumer.cpp:2435`. Claim: `legacy_tolerant_slot_labels`.

This is the original running-policy cache read in cache_running_policy, not the guessed spec_raw_label_partition read at line 2425.

```cpp
    running_tolerant_labels_ = legacy_tolerant_slot_labels();
```

### 107. `docs/design/native-feature-parity.md:207`

Old: `:10149` → New: `:10185-10186`.

Source read: `src/native_execution_consumer.cpp:10185-10186`. Claim: `legacy_tolerant_slot_labels`.

The distinct realtime confirmed-input read.

```cpp
    const bool tolerant_realtime = legacy_tolerant_slot_labels() && running
        && running->phase == NativeRunPhase::Realtime;
```

### 108. `docs/design/native-feature-parity.md:209`

Old: `engine.hpp:754` → New: `engine.hpp:757`.

Source read: `include/pineforge/engine.hpp:757`. Claim: `round_to_mintick`.

```cpp
    double round_to_mintick(double price) const {
```

### 109. `docs/design/native-feature-parity.md:209`

Old: `engine.hpp:772` → New: `engine.hpp:775`.

Source read: `include/pineforge/engine.hpp:775`. Claim: `bar_fill_price`.

```cpp
    double bar_fill_price(double raw_bar_price) const {
```

### 110. `docs/design/native-feature-parity.md:209`

Old: `native_execution_consumer.cpp:7389` → New: `native_execution_consumer.cpp:7425`.

Source read: `src/native_execution_consumer.cpp:7425`. Claim: `bar_fill_price`.

The current_price call site, not the range-end report mark.

```cpp
        ? engine.bar_fill_price(current_frame_->point.price) : current_frame_->point.price;
```

### 111. `docs/design/native-feature-parity.md:210`

Old: `engine.hpp:1791` → New: `engine.hpp:1794`.

Source read: `include/pineforge/engine.hpp:1794`. Claim: `stream_state_hash`.

```cpp
    uint64_t stream_state_hash() const;
```

### 112. `docs/design/native-feature-parity.md:214`

Old: `engine.hpp:154` → New: `engine.hpp:156-157`.

Source read: `include/pineforge/engine.hpp:156-157`. Claim: `skip_entry_bar_high / skip_entry_bar_low`.

```cpp
    bool skip_entry_bar_high = false;
    bool skip_entry_bar_low = false;
```

### 113. `docs/design/native-feature-parity.md:270`

Old: `native_execution_consumer.cpp:8252` → New: `native_execution_consumer.cpp:8288-8292`.

Source read: `src/native_execution_consumer.cpp:8288-8292`. Claim: `CallbackException`.

invoke_callback's exception catch latches the callback failure.

```cpp
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render_exception(engine, e);
        }
```

### 114. `docs/design/native-feature-parity.md:271`

Old: `native_execution_consumer.cpp:1857` → New: `native_execution_consumer.cpp:1893-1904`.

Source read: `src/native_execution_consumer.cpp:1893-1904`. Claim: `commands_allowed`.

```cpp
bool NativeExecutionConsumer::commands_allowed() const {
    // A command asked for from inside a capture or an observer call is refused
    // and latched, so the terminal helper rejects that generation; every
    // ordinary admission below is unchanged.
    if (refuse_dispatch_reentry()) return false;
    if (failed() || consuming_request_) return false;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return false;
    if (in_callback_) return true;
    if (processing_input_) return false;
    return running->phase == NativeRunPhase::Realtime;
}
```

### 115. `docs/design/native-feature-parity.md:284`

Old: `native_execution_consumer.cpp:9261` → New: `native_execution_consumer.cpp:9297`.

Source read: `src/native_execution_consumer.cpp:9297`. Claim: `contribute_input`.

```cpp
bool NativeExecutionConsumer::contribute_input(
```

### 116. `docs/design/native-feature-parity.md:284`

Old: `native_execution_consumer.cpp:9171` → New: `native_execution_consumer.cpp:9207`.

Source read: `src/native_execution_consumer.cpp:9207`. Claim: `seal_script`.

The bucket-sealing definition, not the neighboring seal_stale_script call.

```cpp
void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
```

### 117. `docs/design/native-feature-parity.md:285`

Old: `native_execution_consumer.cpp:8123` → New: `native_execution_consumer.cpp:8159`.

Source read: `src/native_execution_consumer.cpp:8159`. Claim: `invoke_input_callback`.

```cpp
bool NativeExecutionConsumer::invoke_input_callback(
```

### 118. `docs/design/native-feature-parity.md:290`

Old: `engine.hpp:1296-1298` → New: `engine.hpp:1299-1300`.

Source read: `include/pineforge/engine.hpp:1299-1300`. Claim: `register_security_eval`.

```cpp
    void register_security_eval(int sec_id, const std::string& requested_tf,
                                const std::string& input_tf);
```

### 119. `docs/design/native-feature-parity.md:290`

Old: `engine.hpp:1732` → New: `engine.hpp:1735-1736`.

Source read: `include/pineforge/engine.hpp:1735-1736`. Claim: `set_native_security_feed`.

```cpp
    bool set_native_security_feed(const std::string& timeframe,
                                  const Bar* bars, int n);
```

### 120. `docs/design/native-feature-parity.md:290`

Old: `engine.hpp:1640` → New: `engine.hpp:1643`.

Source read: `include/pineforge/engine.hpp:1643`. Claim: `prepare_native_security_feeds`.

```cpp
    void prepare_native_security_feeds(const Bar* input_bars, int n_input);
```

### 121. `docs/design/native-feature-parity.md:290`

Old: `engine.hpp:1313-1315` → New: `engine.hpp:1316`.

Source read: `include/pineforge/engine.hpp:1316`. Claim: `feed_security_eval_state`.

```cpp
    void feed_security_eval_state(SecurityEvalState& state, const Bar& input_bar);
```

### 122. `docs/design/native-feature-parity.md:293`

Old: `native_execution_consumer.cpp:1910` → New: `native_execution_consumer.cpp:1945-1948`.

Source read: `src/native_execution_consumer.cpp:1945-1948`. Claim: `UnsupportedSource`.

```cpp
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
```

### 123. `docs/design/native-feature-parity.md:326`

Old: `engine.hpp:1640` → New: `engine.hpp:1643`.

Source read: `include/pineforge/engine.hpp:1643`. Claim: `prepare_native_security_feeds`.

```cpp
    void prepare_native_security_feeds(const Bar* input_bars, int n_input);
```

### 124. `docs/design/native-feature-parity.md:326`

Old: `engine.hpp:1323` → New: `engine.hpp:1326`.

Source read: `include/pineforge/engine.hpp:1326`. Claim: `configure_security_evaluators`.

```cpp
    virtual void configure_security_evaluators() {}
```

### 125. `docs/design/native-feature-parity.md:326`

Old: `engine.hpp:1732` → New: `engine.hpp:1735-1736`.

Source read: `include/pineforge/engine.hpp:1735-1736`. Claim: `set_native_security_feed`.

```cpp
    bool set_native_security_feed(const std::string& timeframe,
                                  const Bar* bars, int n);
```

### 126. `docs/design/native-feature-parity.md:329`

Old: `engine.hpp:153` → New: `engine.hpp:156`.

Source read: `include/pineforge/engine.hpp:156`. Claim: `skip_entry_bar_high`.

```cpp
    bool skip_entry_bar_high = false;
```

### 127. `docs/design/native-feature-parity.md:333`

Old: `CMakeLists.txt:161` → New: `CMakeLists.txt:162`.

Source read: `CMakeLists.txt:162`. Claim: `add_library`.

First occurrence on this document line.

```cmake
add_library(pineforge_kernel STATIC ${PINEFORGE_KERNEL_SOURCES})
```

### 128. `docs/design/native-feature-parity.md:333`

Old: `CMakeLists.txt:161` → New: `CMakeLists.txt:162`.

Source read: `CMakeLists.txt:162`. Claim: `add_library`.

Second occurrence on this document line.

```cmake
add_library(pineforge_kernel STATIC ${PINEFORGE_KERNEL_SOURCES})
```

### 129. `docs/design/native-feature-parity.md:346`

Old: `native_execution_consumer.cpp:10585` → New: `native_execution_consumer.cpp:10621`.

Source read: `src/native_execution_consumer.cpp:10621`. Claim: `run_rich`.

```cpp
void NativeExecutionConsumer::run_rich(BacktestEngine& engine,
```

### 130. `docs/design/native-feature-parity.md:416`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 131. `docs/design/native-feature-parity.md:416`

Old: `native_execution_consumer.cpp:7258` → New: `native_execution_consumer.cpp:7294`.

Source read: `src/native_execution_consumer.cpp:7294`. Claim: `drain_applied_notifications`.

The matching pass's notification drain call.

```cpp
        drain_applied_notifications(engine);
```

### 132. `docs/design/native-feature-parity.md:449`

Old: `native_execution_consumer.cpp:5905` → New: `native_execution_consumer.cpp:5940-5942`.

Source read: `src/native_execution_consumer.cpp:5940-5942`. Claim: `TermsUnresolved`.

```cpp
        if (unresolved && !terms.units) {
            return terminal(native_order::MatchRejectReason::TermsUnresolved, terms);
        }
```

### 133. `docs/design/native-feature-parity.md:521`

Old: `native_execution_consumer.cpp:4517` → New: `native_execution_consumer.cpp:4553`.

Source read: `src/native_execution_consumer.cpp:4553`. Claim: `risk_evaluate`.

```cpp
void NativeExecutionConsumer::risk_evaluate(BacktestEngine& engine,
```

### 134. `docs/design/native-feature-parity.md:871`

Old: `engine.hpp:546` → New: `engine.hpp:549`.

Source read: `include/pineforge/engine.hpp:549`. Claim: `fold_exit_trail_peak_`.

```cpp
    double fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
```

### 135. `docs/design/native-feature-parity.md:874`

Old: `native_execution_consumer.cpp:9058` → New: `native_execution_consumer.cpp:9094-9095`.

Source read: `src/native_execution_consumer.cpp:9094-9095`. Claim: `fold_exit_trail_peak_`.

The mark-row producer's conditional carry.

```cpp
        if (!std::isnan(engine.fold_exit_trail_peak_))
            context.preceding_exit_trail_peak = engine.fold_exit_trail_peak_;
```

### 136. `docs/design/native-feature-parity.md:1115`

Old: `engine.hpp:351` → New: `engine.hpp:354-356`.

Source read: `include/pineforge/engine.hpp:354-356`. Claim: `BrokerStateHashSink`.

The sink declaration and FNV-1a offset basis.

```cpp
class BrokerStateHashSink {
public:
    uint64_t h = 1469598103934665603ULL;
```

### 137. `docs/design/native-feature-parity.md:1287`

Old: `engine.hpp:1668` → New: `engine.hpp:1671-1676`.

Source read: `include/pineforge/engine.hpp:1671-1676`. Claim: `ExecutionConsumerSlot`.

The slot declaration includes its heap-owned unique_ptr<IExecutionConsumer>.

```cpp
    struct ExecutionConsumerSlot {
        bool native = false;
        mutable std::unique_ptr<IExecutionConsumer> ptr;
        ExecutionConsumerSlot() = default;
        ExecutionConsumerSlot(const ExecutionConsumerSlot& other) noexcept
            : native(other.native) {}
```

### 138. `docs/design/native-feature-parity.md:1292`

Old: `engine.hpp:547` → New: `engine.hpp:550`.

Source read: `include/pineforge/engine.hpp:550`. Claim: `trades_`.

```cpp
    TradeLedger trades_;
```

### 139. `docs/design/native-feature-parity.md:1605`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 140. `docs/design/native-feature-parity.md:1605`

Old: `native_execution_consumer.cpp:7258` → New: `native_execution_consumer.cpp:7294`.

Source read: `src/native_execution_consumer.cpp:7294`. Claim: `drain_applied_notifications`.

```cpp
        drain_applied_notifications(engine);
```

### 141. `docs/design/native-feature-parity.md:1635`

Old: `native_execution_consumer.cpp:12177` → New: `native_execution_consumer.cpp:12213-12216`.

Source read: `src/native_execution_consumer.cpp:12213-12216`. Claim: `current_execution_point`.

The NativeStrategyHost forwarding accessor.

```cpp
std::optional<NativeCurrentPointView> NativeStrategyHost::current_execution_point() const {
    return NativeExecutionConsumer::bound(*this)
        .current_execution_point();
}
```

### 142. `docs/design/native-feature-parity.md:1675`

Old: `engine.hpp:1363` → New: `engine.hpp:1366`.

Source read: `include/pineforge/engine.hpp:1366`. Claim: `update_equity_extremes`.

```cpp
    void update_equity_extremes() {
```

### 143. `docs/design/native-feature-parity.md:1675`

Old: `engine.hpp:1381` → New: `engine.hpp:1384`.

Source read: `include/pineforge/engine.hpp:1384`. Claim: `record_equity_point`.

```cpp
    void record_equity_point(int64_t script_bar_ts) {
```

### 144. `docs/design/native-feature-parity.md:1675`

Old: `native_execution_consumer.cpp:8217` → New: `native_execution_consumer.cpp:8253`.

Source read: `src/native_execution_consumer.cpp:8253`. Claim: `invoke_callback`.

First occurrence on this document line.

```cpp
void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
```

### 145. `docs/design/native-feature-parity.md:1675`

Old: `:8217` → New: `:8253`.

Source read: `src/native_execution_consumer.cpp:8253`. Claim: `invoke_callback`.

Second occurrence on this document line.

```cpp
void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
```

### 146. `docs/design/native-feature-parity.md:1675`

Old: `:8217` → New: `:8253`.

Source read: `src/native_execution_consumer.cpp:8253`. Claim: `invoke_callback`.

Third occurrence on this document line.

```cpp
void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
```

### 147. `docs/design/native-feature-parity.md:1675`

Old: `:8217` → New: `:8253`.

Source read: `src/native_execution_consumer.cpp:8253`. Claim: `invoke_callback`.

Fourth occurrence on this document line.

```cpp
void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
```

### 148. `docs/design/native-feature-parity.md:1679`

Old: `native_execution_consumer.cpp:9171` → New: `native_execution_consumer.cpp:9207`.

Source read: `src/native_execution_consumer.cpp:9207`. Claim: `seal_script`.

```cpp
void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
```

### 149. `docs/design/native-feature-parity.md:1702`

Old: `native_execution_consumer.cpp:5876` → New: `native_execution_consumer.cpp:5912`.

Source read: `src/native_execution_consumer.cpp:5912`. Claim: `resolve_execution_terms`.

```cpp
            terms = host->resolve_execution_terms(terms_facts);
```

### 150. `docs/design/native-feature-parity.md:1729`

Old: `native_execution_consumer.cpp:3416` → New: `native_execution_consumer.cpp:3452`.

Source read: `src/native_execution_consumer.cpp:3452`. Claim: `apply_excursion`.

```cpp
void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
```

### 151. `docs/design/native-feature-parity.md:1731`

Old: `native_execution_consumer.cpp:3740` → New: `native_execution_consumer.cpp:3776`.

Source read: `src/native_execution_consumer.cpp:3776`. Claim: `admit_opening_inspect`.

```cpp
bool NativeExecutionConsumer::admit_opening_inspect(
```

### 152. `docs/design/native-feature-parity.md:1751`

Old: `native_execution_consumer.cpp:8002-8014` → New: `native_execution_consumer.cpp:8020-8039`.

Source read: `src/native_execution_consumer.cpp:8020-8039`. Claim: `OrderFill`.

The notification loop's reentry guard, FIFO delivery, budget check and fill recalculation are one contiguous block.

```cpp
void NativeExecutionConsumer::drain_queued_notifications(BacktestEngine& engine) {
    if (in_callback_ || consuming_request_ || draining_notifications_ || failed()) return;
    draining_notifications_ = true;
    while (notification_head_ < applied_notifications_.size() && !failed()) {
        const auto notification = applied_notifications_[notification_head_++];
        raise_floor(notification.point.decision.coordinate.effective_time_ms);
        invoke_applied_callback(engine, notification);
        if (failed() || !recalculates_on_fills()) continue;
        // The event is delivered whatever the budget says; only the
        // recalculation it would drive is dropped once this point has spent
        // max_recalculations_per_point. Executions the callback drives
        // through execute_current land at the same cursor, so they spend the
        // same point's budget and the cascade is bounded.
        if (!claim_recalculation()) continue;
        const auto applied = std::get<native_order::ExecutionAppliedEvent>(
            requests_.history_at(notification.history_index));
        enter_point_frame(engine, notification.point, CallbackPhase::Applied);
        invoke_recalculation(engine, calculating_bar(engine),
                             NativeCalculationReason::OrderFill, &applied);
    }
```

### 153. `docs/design/native-feature-parity.md:1752`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

First occurrence on this document line.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 154. `docs/design/native-feature-parity.md:1752`

Old: `native_execution_consumer.cpp:6575` → New: `native_execution_consumer.cpp:6611-6615`.

Source read: `src/native_execution_consumer.cpp:6611-6615`. Claim: `born_on_remaining_path`.

Second occurrence on this document line.

```cpp
    auto born_on_remaining_path = [&](const native_order::LiveRequest& live) {
        return continuous && live.birth().acceptance_ordinal > P
            && remaining_path_coordinate(point.coordinate)
            && point.coordinate.effective_time_ms >= live.birth().decision_time_lower_bound;
    };
```

### 155. `docs/design/native-feature-parity.md:1753`

Old: `native_execution_consumer.cpp:7848` → New: `native_execution_consumer.cpp:7884`.

Source read: `src/native_execution_consumer.cpp:7884`. Claim: `recalculate_at_modeled_point`.

```cpp
void NativeExecutionConsumer::recalculate_at_modeled_point(
```

### 156. `docs/design/native-feature-parity.md:1754`

Old: `native_execution_consumer.cpp:3360` → New: `native_execution_consumer.cpp:3396`.

Source read: `src/native_execution_consumer.cpp:3396`. Claim: `record_driver`.

```cpp
void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
```

### 157. `docs/design/native-feature-parity.md:1820`

Old: `native_execution_consumer.cpp:7215-7219` → New: `native_execution_consumer.cpp:7251-7255`.

Source read: `src/native_execution_consumer.cpp:7251-7255`. Claim: `native_matching::apply_slippage`.

```cpp
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        // An opted-in grid books on the tick ladder BEFORE slippage, which is
        // itself a whole number of ticks.
        const double basis = grid_fill_basis(*spec, winner->price, buy, limit_governed);
        double resolved = native_matching::apply_slippage(basis, slip, buy);
```

### 158. `docs/design/native-feature-parity.md:1820`

Old: `native_execution_consumer.cpp:7576` → New: `native_execution_consumer.cpp:7612`.

Source read: `src/native_execution_consumer.cpp:7612`. Claim: `execute_current`.

```cpp
NativeCurrentExecutionResult NativeExecutionConsumer::execute_current(
```

### 159. `docs/design/native-feature-parity.md:1840`

Old: `native_execution_consumer.cpp:3740` → New: `native_execution_consumer.cpp:3776`.

Source read: `src/native_execution_consumer.cpp:3776`. Claim: `admit_opening_inspect`.

```cpp
bool NativeExecutionConsumer::admit_opening_inspect(
```

### 160. `docs/native-refactor-progress.md:185`

Old: `include/pineforge/engine.hpp:547` → New: `include/pineforge/engine.hpp:550`.

Source read: `include/pineforge/engine.hpp:550`. Claim: `trades_`.

```cpp
    TradeLedger trades_;
```

### 161. `docs/native-refactor-progress.md:197`

Old: `src/native_execution_consumer.cpp:3379-3384` → New: `src/native_execution_consumer.cpp:3415-3425`.

Source read: `src/native_execution_consumer.cpp:3415-3425`. Claim: `reserve_driver_log`.

The definition includes the Full-retention guard and reservation.

```cpp
void NativeExecutionConsumer::reserve_driver_log(std::size_t expected_points) {
    // The driver log alone. The command history is sized by pump_batch from
    // what the batch records; a reservation made here before begin_ready
    // never reached a run anyway, since begin_ready resets the request core.
    // A run that keeps no driver points reserves none.
    if (retention() != NativeEventRetention::Full) return;
    if (expected_points > driver_log_.max_size()) {
        throw std::length_error("native driver-log capacity exhausted");
    }
    if (expected_points > driver_log_.capacity()) driver_log_.reserve(expected_points);
}
```

### 162. `docs/native-refactor-progress.md:199`

Old: `src/native_execution_consumer.cpp:10435-10446` → New: `src/native_execution_consumer.cpp:10471-10489`.

Source read: `src/native_execution_consumer.cpp:10471-10489`. Claim: `presize_logs`.

The lambda definition contains the Window early return and retained-journal reservation.

```cpp
    const auto presize_logs = [&](std::size_t consumed_inputs) {
        if (consumed_inputs != checkpoint || checkpoint >= total) return;
        const std::size_t consumed = checkpoint;
        checkpoint *= 2;
        if (const std::size_t points = presize(driver_base, driver_log_.size(),
                                               driver_log_.capacity(), consumed,
                                               driver_log_.max_size())) {
            hint([&] { reserve_driver_log(points); });
        }
        // v19-B: under Window the journal is a window, whose length says
        // nothing about the batch's; only a retained journal is presized.
        if (retention() == NativeEventRetention::Window) return;
        const auto& history = requests_.history();
        if (const std::size_t events = presize(history_base, history.size(),
                                               history.capacity(), consumed,
                                               history.max_size())) {
            hint([&] { requests_.reserve(events); });
        }
    };
```

## Repeated occurrences read back

- `docs/design/native-feature-parity.md:123`: both `on_native_applied` citations now point to line 7974 (entries 60 and 61).
- `docs/design/native-feature-parity.md:167`: both `hash_source_extension` citations point to line 415; `hash_host_extension` points to line 409 (entries 96–98).
- `docs/design/native-feature-parity.md:333`: both `add_library` citations point to line 162 (entries 127 and 128).
- `docs/design/native-feature-parity.md:1675`: the full `invoke_callback` citation and all three continuations point to line 8253 (entries 144–147); the two listed equity citations also moved (entries 142 and 143).
- `docs/design/native-feature-parity.md:1752`: both `born_on_remaining_path` citations point to lines 6611–6615 (entries 153 and 154).

The edited documentation was read back and compared to the original text with only these 162 explicit pointer substitutions. The four files retain their original line counts; no prose or unlisted citation moved. `git diff --check` returned no diagnostics. This is a text/diff review, not a preflight or build result.
