Apply each patch separately to an isolated copy of the candidate tree, rebuild
all targets, and run the named rows. Neither mutation changes a test assertion.

| Patch | Rows that must fail | Failure witness |
| --- | --- | --- |
| `sink_poll_total_time.patch` | `test_report_sink_stall_slow` | Real one-byte reads continue at each injected poll wake, but a total wait deadline expires before the pipe releases enough space for `POLLOUT`. The executable prints `report sink reader stalled`. |
| `run_budget_end_only.patch` | `test_host_run_budget`, `test_host_run_budget_cpu`, `test_host_run_budget_real_cpu` | All three callbacks run before the retained end check; the first two rows print `budget must stop before the next input callback, not at teardown`. Begin and end checks remain present, so failure proves the missing input-boundary check. |

The unmodified candidate must pass these rows first. A failed patch application,
compile error, fixture/setup error, or timeout is not a mutation result.
The patches are intentionally not applied to the candidate delivered here.
