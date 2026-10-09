# Recorded outputs

Recorded outputs are unreleased, targeted for 1.5.0, and not part of 1.4.0.

A module can record what it computes on each bar besides its trades: values
kept per bar, values kept once per run, and events with an optional message.
A generated module records them for a script compiled to record outputs; a
C++ host records them through the `BacktestEngine` members below. A caller
reads the record through the `strategy_outputs_*` functions of
`<pineforge/pineforge.h>` (group `pf_outputs`), and `docker/run_json.py
--outputs` writes it into the JSON report.

Recording is off until a caller turns it on. Nothing a run computes depends on
it: the trades, the report (`pf_report_t`), every hash and `PF_ABI_VERSION`
are what they are with recording off, and no matching, fill, margin, sizing or
report path reads the record. `tests/test_outputs_off_identity.cpp` runs one
trading host with outputs undeclared, declared and off, and recording, and
holds the trades, the equity curve, the per-bar broker-state hashes and the
stream-state hash equal.

## The record

A module declares the record's shape once: `S` series slots per row, `O`
outputs an event may name, and `K` run constants. What each slot, output and
constant means is not the engine's business; the module's manifest
(`strategy_outputs_manifest`) says it.

- **Rows.** One per bar the module published in this run, keyed by the bar's
  open time and also holding its close time. A row's position is its
  `bar_index`. A module that opens at most one row per calculation, as a
  generated one does, never records more rows than the run dispatched
  calculations (`strategy_script_bars_processed`).
- **Values.** A double per slot per row. NaN is na; a slot not written in a
  row stays NaN; the last write of a slot in a row wins.
- **Events.** Each names an output and the row it was recorded on, carries a
  double and, optionally, a message. `sequence` numbers the run's events from
  1; `ordinal_in_bar` numbers one output's events within its row from 0;
  `phase` is the run phase (batch, stream warm-up, stream realtime);
  `confirmed` is 0 only for an event recorded while a stream finalizes a bar
  it never saw close (`strategy_stream_end` with a partial input bar);
  `message_hash64` is the FNV-1a 64 hash of every byte recorded as the
  message (0 without a message). A C reader receives the message as
  NUL-terminated text, so a message holding a NUL byte reads to its first NUL
  while its hash covers all of it: re-hash the bytes you recorded, not the
  text you read back.
- **Run constants.** One double per index per run, for a value fixed for the
  whole run, such as a level read from an input. NaN until written; the
  run's first write stores it and a later write must be equal (bit for bit,
  or both NaN).

A colour is an ordinary value. A manifest that marks a slot or a constant with
the encoding `rgba-u32` stores a colour as the integer R·2²⁴ + G·2¹⁶ + B·2⁸ + A,
whose eight hex digits read as the CSS literal `#RRGGBBAA` (A is opacity, 255
opaque), and na as NaN. The engine stores the double and knows nothing of
colours.

## Writing the record from C++

The writers are protected members of `BacktestEngine`
(`include/pineforge/engine.hpp`), so a generated module or a C++ host
subclass calls them; a C host has no writer in this version. Each writer but
`declare_outputs` and `output_run_begin` returns at once while recording is
off, so a module can call them unconditionally; generated code tests
`outputs_enabled_` first.

| Member | Call it | Fails when |
|---|---|---|
| `declare_outputs(slots, outputs, run_constants = 0)` | once, before the first run (a generated constructor) | a count is negative; the host is running; a second call names other counts |
| `output_run_begin()` | at the start of every run, before any bar; a C++ host calls it in `on_native_run_begin` | never; calling it twice is calling it once |
| `output_bar(open_ms, close_ms)` | first, in every calculation the host publishes | `output_run_begin()` was never called; `open_ms` is below the last row's; the row count reaches `INT_MAX` |
| `output_value(slot, value)` | after `output_bar` | no row is open; `slot` is out of range |
| `output_event(output, value)`, `output_event(output, value, message)` | after `output_bar` | no row is open; `output` is out of range; the queue holds `INT_MAX` events |
| `output_constant(index, value)` | any time in the run | `output_run_begin()` was never called; `index` is out of range; the value differs from the run's first write |

Call `output_bar` first in every calculation you write in. A row stays open
until the next `output_bar`, so a calculation that skips it and still writes
lands in the previous bar's row.

Each failure carries a run-failure code (`docs/pages/run-failure-codes.md`)
beside its text. A broken precondition of the table above is a host or
generated-code defect, thrown as `std::logic_error` and coded
`engine_invariant`. An event queue or a row count that reaches `INT_MAX`
throws `std::runtime_error` coded `outputs_limit` (reason `too_many_events`
or `too_many_rows`). A recalculation of a bar after a caller cleared that
bar's events throws `std::runtime_error` coded `outputs_rejected` (reason
`recalculated_after_clear`), the code of the switch's refusals below.
Thrown inside a callback, the exception fails the run with its text in
`last_error()` (`strategy_get_last_error`) and its code in
`strategy_get_last_error_code`. A caller reads nothing after a failed run:
the record then holds whatever the run wrote before it failed.

**The open and close a C++ host states.** The open is
`NativeDecisionContext::script_bar_open_ms`. The close is the second argument;
the script interval's end, `context.script_interval.next_period_open_ms`, is
the natural choice, and a host that cannot state a close passes `INT64_MIN`.

**Row identity.** The host states which bar a calculation is about, so the
record follows the bars the host publishes, not the calculations the kernel
counts:

- an open above the last row's appends a row (every slot NaN, the ordinals at
  0);
- an open equal to the last row's recalculates that row: its values return
  to NaN, its close is replaced, its ordinals restart, and its queued events
  are withdrawn, their sequence numbers issued again. A row with an event a
  caller has already cleared cannot be recalculated: the call fails the run,
  so an event a caller has taken is never withdrawn or renumbered;
- an open below the last row's fails the run.

Two calculations that state one open, with no event cleared between them,
are therefore one row, and the bar indices after it run one short of the
calculation count (`tests/test_outputs_recorder.cpp`). The Pine host publishes
strictly increasing opens on every path but one: a daily stream of ticks on a
session that crosses midnight in its time zone can stamp two bars alike.

A stream can dispatch a calculation for a bar that is not after the last one
the host published: on a `FeedTolerant` stream whose last warm-up bar carries
an off-grid label, the first clock advance calculates the quiet slot that
holds that label. A host that records only what it publishes skips such a
calculation, as the Pine host does (compare `script_bar_open_ms` with the
last open you published); one that calls `output_bar` for it fails the run.
`tests/test_outputs_stream_equivalence.cpp` reproduces the case.

**The run boundary.** A host that never calls `output_run_begin()` fails its
first recorded bar. One that calls it on its first run only appends the next
run to the old record when that run's bars open later, and fails when they do
not (`tests/test_outputs_run_reuse.cpp`).

A minimal host:

```cpp
class Recorder : public pineforge::NativeStrategyHost {
public:
    Recorder() { declare_outputs(/*slots*/ 1, /*outputs*/ 1); }

    void on_native_run_begin() override {
        output_run_begin();
        last_open_ = std::numeric_limits<int64_t>::min();
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext& context) override {
        if (context.script_bar_open_ms <= last_open_) return;   // not a new bar
        last_open_ = context.script_bar_open_ms;
        output_bar(context.script_bar_open_ms, context.script_interval.next_period_open_ms);
        output_value(0, bar.close);
        if (bar.close > bar.open) output_event(0, bar.close, "up");
    }

private:
    int64_t last_open_ = 0;
};
```

## Reading the record

| Function | Answers |
|---|---|
| `strategy_outputs_set_enabled(s, on)` | 0, or -1, nothing changed and the refusal coded `outputs_rejected`: reason `not_declared` (turning on a module that declares nothing) or `run_in_progress` (changing the switch while a run is in progress: a batch, or a stream from `strategy_stream_begin` to `strategy_stream_end`). A change clears the record; a call that changes nothing answers 0, also during a run. As with `strategy_set_trace_enabled`, a success leaves `strategy_get_last_error` and its code as they were, and a refusal keeps a failed run's own: read a failed run's error before or after the switch alike. |
| `strategy_outputs_series_count(s)` | slots per row; 0 while recording is off |
| `strategy_outputs_bars_len(s)` | rows |
| `strategy_outputs_bar_times_copy(s, from_bar, open_ms, close_ms, capacity, written)` | the rows' times from `from_bar` on |
| `strategy_outputs_series_copy(s, series, from_bar, out, capacity, written)` | one slot's values from `from_bar` on |
| `strategy_outputs_events_len(s)` | events queued since the run began or the last clear |
| `strategy_outputs_event_get(s, index, out, size_in)` | one event, copied as `pf_output_event_v1_t` |
| `strategy_outputs_events_clear(s)` | drops the queue; rows and the sequence stay |
| `strategy_outputs_constants_copy(s, out, capacity)` | the run constants; returns their count |

The copies take `from_bar` in `[0, rows]`, a non-negative `capacity` and a
non-NULL `written`; they write `min(capacity, rows - from_bar)` items and
return 0, or return -1 and write nothing. A NULL handle answers -1.
`strategy_outputs_event_get` copies `min(size_in, sizeof(pf_output_event_v1_t))`
bytes and refuses a `size_in` below 8, so an older or newer caller's struct
works; the struct puts every 8-byte field at a multiple of 8, so its 72 bytes
before `message` are laid out alike on every target. `message` is borrowed
until the next call that runs, streams or clears on the handle.

Every `strategy_outputs_*` call runs on the thread that drives the handle,
between its run and stream calls, never during one; the readers take no
lock.

A module that records nothing still answers the readers (they are runtime
exports of every module built on this engine): turning recording on answers
-1 with `outputs: this module declares no outputs`, and every reader answers
empty. Three functions are a recording module's own:
`strategy_outputs_api_version` (`PF_OUTPUTS_API_VERSION`),
`strategy_outputs_manifest` and `strategy_signal_safety_receipt`, with the
buffer protocol of `strategy_capabilities_receipt`. Detect a recording module
with `dlsym("strategy_outputs_api_version")`; `PINEFORGE_HAS_OUTPUTS_V1`
tells a C or C++ caller that its `pineforge.h` declares the group.

## Batches and streams

A batch caller turns recording on before the run and reads after it; it need
not clear. A streaming caller reads and clears after `strategy_stream_begin`
returns and after each input it pushes. Rows and the sequence carry on across
the warm-up's end; events recorded during the warm-up carry `phase` warm-up,
later ones realtime. A stream records what a batch over the same bars
records, in every value and every event field but `phase`
(`tests/test_outputs_stream_equivalence.cpp`), and a batch cut at bar `t`
records, for every bar up to `t`, what the batch over all the bars records
(`tests/test_outputs_truncation.cpp`).

## `run_json --outputs`

`docker/run_json.py --outputs` turns recording on for every run it makes
(with `--bench`, the timed runs too, so a timing then includes recording) and
writes the record as the report's `outputs` key, just before `fingerprint`;
`applied_runtime` and the fingerprint's `provenance.runtime` then hold
`"outputs": true`. Without the flag the report is unchanged. On a module that
records nothing, or one that refuses to record, the harness prints its one
failure line, `{"engine":"pineforge","error":...,"code":"outputs_rejected",
"args":{"reason":"not_declared"}}`, and exits 1. A recording module whose
outputs exports it cannot read (another outputs API version, a missing
export, a manifest that is not a JSON object or does not list what the module
recorded) fails as `strategy_library_incompatible` (reason
`outputs_api_mismatch` or `outputs_manifest_invalid`).

```json
"outputs": {
  "schema_version": "pineforge-outputs/v1",
  "message_format": "pineforge/v1",
  "manifest_sha256": "<sha256 of the manifest bytes the module returned>",
  "manifest": { "...": "the module's manifest" },
  "bars": {"open_ms": [1704067200000], "close_ms": [1704067500000]},
  "series": [{"slot": 0, "output": "o0", "values": [101.25]}],
  "constants": [50.0, 2021161215],
  "hlines": [{"output": "h0", "price": 50.0}],
  "events": [{"sequence": 1, "output": "o2", "bar_index": 0,
              "bar_open_ms": 1704067200000, "bar_close_ms": 1704067500000,
              "ordinal_in_bar": 0, "phase": "batch", "value": null,
              "message": "close=101.25", "freq": "once_per_bar_close"}]
}
```

`series` has one entry per slot the manifest lists, in slot order; `constants`
one value per run-constant index; `hlines` one entry per horizontal-level
output, its price the run constant the manifest names (null when it was never
written, as in a run with no rows); an event of an alert output also carries
the output's `freq`. A double that is not finite is null, a time equal to
`INT64_MIN` is null, and a value whose manifest encoding is `rgba-u32` is
written as an integer. `scripts/test_report_outputs_keys.py` pins the keys,
and `scripts/test_run_json_outputs.py` runs the harness end to end.

## Costs and limits

- A row costs 8 bytes per slot and is kept for the whole run or stream.
- The event queue holds at most `INT_MAX` events between clears, and a run
  at most `INT_MAX` rows; one more fails the run as `outputs_limit`.
- Recording is a branch per writer call while it is off, and generated code
  takes that branch once per call site.
