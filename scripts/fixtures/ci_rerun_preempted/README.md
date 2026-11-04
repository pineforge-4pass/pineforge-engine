# Captured runner-loss evidence

Trimmed directly from the original runner-loss captures, without copying
any other controller's tests. `captured.json` retains the original source filenames
as keys. Run objects retain the gate's fields and repository identity; job
pages retain only sanitizers-shard (4/4), job 125760264898, or the ordinary
preflight failure, job 125518846442. Step names/conclusions and failure
annotation messages are unchanged. Other jobs (including jobs still running
in the wave-6 capture), warning annotations and unused fields are omitted.
These are evidence samples, not a claim that the captured full run had finished.

The completed `run-41588378688-attempt1.json` was unavailable when these samples
were trimmed. The original `run-41588378688.json` remains `in_progress` with a
null conclusion. The positive process fixture copies it and changes **only**
status/conclusion to `completed`/`failure`; other mutations are explicit
negative controls or label/event/attempt coverage in the tests.

Both `.log` files retain all captured `##[error]` diagnostics byte-for-byte.
The wave-6 log also retains its five lines containing terminal escapes, so a
mock of gh's real escape-sequence refusal exercises the successful transport
path. No full logs, credentials or response bodies are printed by the tests.
Run on the designated test host with
`python3 scripts/test_ci_rerun_preempted.py`; no network or gh is used.

The aggregate-gate change adds these trims of the supplied capture directory:

| File | Repository | Run | Head prefix |
| --- | --- | --- | --- |
| `e1.json` | engine | 41712833348 | 59f98ba8 |
| `e2.json` | engine | 41755437507 | cf68f156 |
| `hpo.json` | hpo | 41749596952 | bef4c2bf |

Each JSON object retains the original filenames as keys: `run.json`,
`jobs-attempt1.json`, `workflow-at-head.yml`, `failed-ids.txt`, and each failed
job's `annotations-<job>.json` and `job-<job>.log`. Run identity and gate fields
are retained. All job IDs, display names, statuses and conclusions are kept;
failed jobs also keep labels, check identity and every step name/conclusion.
Only failure annotations are kept, with messages unchanged. Workflow data
retains all job IDs and their exact names, needs, reusable calls and strategies;
unneeded scripts, settings and comments are omitted. Long JSON string tokens
use Unicode escapes solely for the commit scan; decoded strings are unchanged.

The stored `e2` run snapshot was captured while attempt 2 was pending;
`hpo` records successful attempt 2. `CaptureAPI` replays failed attempt 1
with the captured attempt-1 jobs. The stored snapshots are not changed.
Amendment 1 supplies real logs for all 11 failed `e1` jobs and all three
failed `hpo` jobs. The trims retain every captured `##[error]` and runner-loss
line, including the display columns added by `gh run view --job --log`.
`api_logs` removes only the matching job-name and step-name columns to model
the controller's raw log API; timestamped diagnostic text stays unchanged.
The `e1` trim contains 16 diagnostic lines, including all five shutdowns.
The supplied HPO logs contain no such diagnostic lines, so their trimmed
strings are empty. Their failure annotations remain unchanged and prove the
memory gate's own failure; missing native loss proof cannot excuse that job.
Only exact `job-<id>.log` files were read; `.log.new` leftovers are ignored.

The four `e2` runner-loss logs come from the supplied 12:28 recapture and
retain nine diagnostic lines, including four shutdowns. Attempt-1 logs for
gates 126342953893, 126346363870 and 126359176509 are unavailable after the
attempt-2 rerun: GitHub now serves the later attempt's logs. Their log fields
contain exactly `attempt-1 log unavailable after the attempt-2 re-run`.
The previous three unsupported log lines and API recovery claim are removed.
The gates' available attempt-1 evidence is the exit-code-1 annotations and
job steps/conclusions captured at 11:05, before the rerun. That evidence is
unchanged. Amendment 6 maps each unavailable marker to `LogUnavailable`,
the exception used for missing or expired log API responses (404/410).
It is never returned as successful log text. The old e2 controller now
refuses with `lookup_or_request_failed` when fetching its first gate log;
the historical independent-failure blocker cannot be replayed without that
attempt's log. The new controller inspects available logs and falls back to
the captured steps and annotations for these three unavailable gate logs.
Both engine cases assert the old refusal and the new rerun with exact
dependent IDs and POST behavior; no pending-log skips remain. The synthetic
controls are separate and clearly labelled.

Each fixture with logs also has a `capture` section containing its original
trimmed capture lines, including their line endings. The e1/e2/HPO sections
are populated directly from the supplied capture files, independently of
the replay log fields. A null entry explicitly records an unavailable gate
log; an empty list means an available capture has no retained diagnostics.
The legacy `captured.json` references the two unchanged checked-in log
assets with `log_files` and keeps their original trimmed rows in `capture`.
`CaptureTests` loads every JSON fixture and every referenced log asset. It
requires replay logs to equal their own capture rows verbatim, or to carry
the exact unavailable marker with a null capture and a matching gate job.
Corruption controls reject inserted log text and replacements of the three
unavailable e2 markers. No raw logs are printed on a provenance failure.

`BeforeTests` loads the unchanged controller with `git show` at `cf68f156`
and asserts e1's original blocker and e2's unavailable-log refusal. It does
not call gh or the network; the base commit must exist on the test host.
`AggregateTests` checks
decisions, dependent IDs, blockers, API reads and the exact failed-jobs POST.
It also loads the full `e2` workflow from its captured Git commit to check
the untrimmed parser, matrix mappings and needs without requiring job logs.
The default suite audits provenance, then runs the red witnesses
before the existing tests and the new behavior tests.
Original `ControllerTests` methods remain unchanged. Tests ran on a separate
test host.

Amendment 2 adds `reuse.json`, with the called workflow paths as keys.
It trims the native-live and corpus-parity workflows read with `git show`
at the captured engine heads, `59f98ba8` and `cf68f156`; both files are
identical between those commits. Every job ID, name, needs list and matrix
is retained. Scripts and unrelated settings are omitted. That amendment
left the three run fixtures and their diagnostic evidence unchanged.

The controller reads those same-repository called files through the contents
API at the run's head SHA, once per file. Child jobs use exact names and their
own workflow needs; caller matrices keep separate child scopes. Caller needs
remain on the caller, and a need on a caller includes its failed child jobs.
Unreadable or invalid called files, call cycles, unknown children, and remote
or non-workflow references refuse with `aggregate_mapping_unavailable`.
The untrimmed workflow test now reads both callees at the captured commit too.

Gate recognition now requires exactly one failed step whose name starts with
`Require every`, together with the existing diagnostic/admission checks.
Amendment 6 restores log inspection for every gate. Runner-loss diagnosis
runs first over all available error evidence: a gate with shutdown evidence
and compatible exit-130 fallout is runner loss, not a dependent gate. An
independent diagnostic in either its log or its failure annotations still
refuses the whole run, even alongside runner-loss evidence. Only ordinary
`Process completed with exit code N.` diagnostics may be excused as gate
exits, and an exit code alone never proves runner loss.
Only a missing or expired gate log permits annotation fallback; permission,
rate-limit, transport and other request failures still refuse. Dependent
gates still require positive failure annotations, and unreadable annotations
refuse. Shards still require log inspection; their annotations remain
supplemental. Tests cover log-only and annotation-only assertion faults,
runner loss in the gate itself, missing-log transport and mandatory shard
logs. The assertion faults reuse the existing synthetic control; none are
written into captured fixtures. No captured evidence was changed.
Amendment 3 resolves the chain rule: every failed workflow need must be a
runner-loss job or a proved dependent gate. A topological pass reaches the
fixed point on the workflow's needs DAG; cyclic or unmappable needs refuse
with `aggregate_mapping_unavailable`. A script reference to needs or an
always condition proves nothing. Any own failure refuses the whole run,
and at least one actual runner-loss job is still required.

The original live-run assertions include e1's final build job, 126229967614,
and e2's final job, 126359176509, in the dependent lists. New controls cover
reverse job order, an inner gate with a second failed step or an ordinary
failed step, chains without runner loss, and chains through reusable calls.
The inline-own-failure and inner-shard controls remain, along with checks
for caller and child needs, matrix isolation, exact names, called-file
lookup failures, and cycles inside called workflows.
The gate-chain cases remain intact after the e2 refresh; no tests ran here.
