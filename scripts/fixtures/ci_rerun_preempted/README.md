# Captured runner-loss evidence

Trimmed directly from laneboss's `lb1012/fixtures/` captures, without copying
the peer backend's tests. `captured.json` retains the original source filenames
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
