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

The unmodified candidate must pass these rows first. A failed patch application,
compile error, fixture/setup error, or timeout is not a mutation result.
Signals, alarm exit 124 and any unexpected assertion are harness failures.
The patches are intentionally not applied to the candidate delivered here.
