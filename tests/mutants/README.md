On the Linux verification host run:

```
python3 scripts/test_resource_budget_mutants.py --work-dir /tmp/pineforge-resource-mutants --jobs 4
```

The directory must be new. The runner copies the candidate, builds all targets,
requires every witness row to pass, applies each patch separately, rebuilds all
targets and requires exit 1 with the exact assertion below. It retains command
logs and `summary.json`. Neither mutation changes a test assertion. Pass extra
configure flags as `--cmake-arg=-DFETCHCONTENT_SOURCE_DIR_EIGEN=/path/to/eigen`.

| Patch | Rows that must fail | Failure witness |
| --- | --- | --- |
| `sink_poll_total_time.patch` | `test_report_sink_stall_slow`, `test_report_sink_stall_socket_short_send` | Real one-byte pipe reads continue at each wake, but the per-flush total deadline expires before `POLLOUT`: `report sink reader stalled`. Socket bytes accepted after the first wake must move the deadline by one sample; disabling that reset prints `accepted socket bytes must reset the deadline at the first wake`. |
| `run_budget_end_only.patch` | `test_host_run_budget`, `test_host_run_budget_cpu`, `test_host_run_budget_real_cpu`, `test_host_run_budget_sink` | All supplied callbacks run before the retained end check; wall/cpu print `budget must stop before the next input callback, not at teardown`, real_cpu prints `real CPU spin must stop at the next bar boundary`, and sink prints `budget must stop at an input boundary and abandon the sink trailer`. Begin and end checks remain present, so failure proves the missing input-boundary check. |
| `sink_zero_builtin.patch` | `test_report_sink_stall_default`, `test_report_sink_stall_default_socket` | Restores the former 30 s default. Both fixtures hold the reader for 2*30 s of virtual time, then print `off sink must still wait after twice the former deadline`. |
| `sink_consumer_zero_builtin.patch` | `test_report_sink_stall_engine_default` | Adds a built-in fallback to the copied host configuration; prints `default host sink must wait beyond the former deadline`. |
| `sink_sample_no_floor.patch` | `test_report_sink_stall_cadence_floor` | At S=1 ms, interval/300=0; removing the 1 ms floor prints `poll cadence must respect the configured interval and sampling clamps`. |
| `sink_sample_no_ceiling.patch` | `test_report_sink_stall_cadence_ceiling` | S=2*300*100+1 ms yields a raw 200 ms sample; removing the 100 ms ceiling prints the same cadence assertion. |
| `sink_sample_no_remaining.patch` | `test_report_sink_stall_configured` | S=2*(300*1)+1=601 ms has P=2 ms and a final 1 ms remainder. An uncapped last poll overshoots and prints `host sink interval must be copied and retained after descriptor refusal`. |
| `sink_budget_no_range.patch` | `test_report_sink_stall_setter` | Accepts UINT64_MAX as the sink interval; prints `invalid descriptor must be refused before replacing the sink interval`. Each bad descriptor is tried with a fresh host and prior nonzero interval. |
| `run_budget_old_size.patch` | `test_host_run_budget_layout` | Accepts the previous 8+2*8=24-byte candidate instead of only 8+3*8=32; prints `unreleased descriptor must reject the old 24-byte size`. |
| `run_budget_zero_wall_clock.patch` | `test_host_run_budget_unchanged` | Samples wall time without a limit; prints `absent budget must not sample resource clocks`. |
| `sink_small_header_input.patch` | `test_report_sink_stall_begin_sink` | Input mutation: shorten the run ID below one 64 KiB writer window. The header no longer flushes before the first callback, so the unchanged assertion prints `sink interval must apply before the header flush`. |

The unmodified candidate must pass these rows first. The small-header patch
changes only fixture input; all other patches mutate production code.
A failed patch application,
compile error, fixture/setup error, or timeout is not a mutation result.
Signals, alarm exit 124 and any unexpected assertion are harness failures.
The patches are intentionally not applied to the candidate delivered here.
