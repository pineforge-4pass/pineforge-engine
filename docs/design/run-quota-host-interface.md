# Run quota host interface

Status: **PARTIAL**. Managed accounting currently reports
`enforcement=cooperative`. Paid hosting must establish host containment before
claiming host enforcement.

## Mode and startup order

Only the launcher selects the process environment and inherited descriptors.
It must build that environment from trusted hosting policy, stripping all
quota marker and limit names from user-supplied environment maps. CLI flags,
strategy settings, strategy source and request JSON have no mode override.

For a managed process the launcher sets all three marker variables:

| Variable | Required value |
| --- | --- |
| `PF_RUN_QUOTA_MODE` | Exactly `managed`, case sensitive |
| `PF_RUN_QUOTA_NATIVE_FD` | ASCII decimal descriptor number for the native endpoint |
| `PF_RUN_QUOTA_PYTHON_FD` | ASCII decimal descriptor number for the Python endpoint |

Each descriptor is in `3..2147483647`, open and connected to an
`AF_UNIX` / `SOCK_SEQPACKET` socket. The two descriptors must identify distinct
sockets; two numbers that duplicate the same socket are rejected. They are
inherited across the launcher's `exec`; numbers 0, 1 and 2 retain their usual
stream roles. Both endpoints belong to the same host-owned ledger and attempt.
Each actor receives its own pair; sharing a socket stream across independent
native/Python clients is not supported.

For unmanaged execution the launcher omits **all three** marker variables.
Limit variables alone do not enable management. Existing library, CLI, test
and hosting callers with no marker retain their existing execution behavior.
No ledger, reservation, refusal or quota-channel syscall is introduced on
their ledger call paths.

The Python module captures the marker at import; the native runtime captures
it once at the first `mode()` or attachment call. Their single `mode()`
functions return `managed` / `unmanaged` (`QuotaMode` in C++).
Presence of **any** marker component selects managed
admission. A partial marker, another mode value (including `unmanaged` or an
empty value), a malformed descriptor, a closed/non-socket/wrong-type socket,
an unconnected socket or aliased endpoints refuses with
`run_quota_configuration_invalid`, `reason=protocol`, `setting=channel`.
Unavailable platform facilities use `reason=containment_unavailable`,
`setting=channel`. Neither case falls back to unmanaged execution.

The host must import the quota module, preload the trusted native runtime and
call its `mode()` before loading any strategy library, including its static
initializers. It must keep the original marker environment consistent until
both runtimes have captured it. Before any strategy callback, it parses policy
limits, starts the ledger service, attaches each required client with `from_environment()`
and requires a valid HELLO. A bad HELLO refuses with `protocol/channel`.
Later environment edits cannot change either runtime's captured mode or
descriptor numbers. A fresh process is required for another host decision.

Attachment rechecks the original sockets' device/inode identities, detecting
close/replacement before attachment. Clients own private close-on-exec
duplicates; the Python duplicate is checked non-inheritable. These flags
protect against accidental descriptor reuse and inheritance. They cannot
prevent arbitrary in-process code from closing or replacing descriptors,
modifying interpreter memory or issuing syscalls directly. The host must
provide that containment and must prevent untrusted code from running before
the trusted runtime initialization.

## Host policy limits

The launcher supplies every limit from a versioned hosting policy, before
strategy execution. There is no engine default and no request-derived limit.
The managed host parser is `parse_limits()` in
[`docker/run_quota.py`](../../docker/run_quota.py); `RunQuota.from_environment()`
uses the actual process environment. Neither environment factory accepts a
request/environment mapping as an alternate mode source.

| Variable | Setting | Unit |
| --- | --- | --- |
| `PF_RUN_SCRATCH_BYTES` | `scratch_bytes` | Simultaneously live disk storage bytes, including pending growth |
| `PF_RUN_SCRATCH_FILES` | `scratch_files` | Simultaneously live backing files/directories, including pending creations |
| `PF_RUN_OUTPUT_BYTES` | `output_bytes` | Cumulative produced output bytes, including pending output |
| `PF_RUN_DESCRIPTOR_LIMIT` | `descriptors` | Aggregate live descriptors across the attempt, including pending opens |

All four values are ASCII decimal integers in
`0..9007199254740991` (`0..2^53-1`). Leading zeros are accepted. Signs,
whitespace, fractions, exponents, hexadecimal and non-ASCII digits are
malformed. A missing limit refuses with `missing_limit`; malformed text
refuses with `malformed_limit`; arithmetic outside the range refuses with
`counter_overflow`. These are `run_quota_configuration_invalid` with
`stage=admission` and the affected setting. Explicit zero is literal zero
capacity. No value means unlimited in managed mode.

The host parses and retains the limits once; native clients receive the
host's verdict over the channel rather than parsing strategy data. The
descriptor allowance is also checked against the actor's host-recorded soft
`RLIMIT_NOFILE`. Policy must budget control sockets, ledger records,
observation descriptors, compiler processes and cleanup resources as well as
data artifacts. Storage extent rounding and memory-backed files require a
host policy; they cannot be inferred from a child's claimed inode/extent.

Unmanaged client snapshots return empty accounting solely as a no-op API
result. Those zeros are not policy limits or measured usage and must not be
published as finite caps in a receipt. Managed-only sizing/policy utilities
are not part of an unmanaged execution path.

## Refusal wire format

Use the existing typed failure renderer in
[`docker/run_json.py`](../../docker/run_json.py): `RunFailure`,
`failure_line()`, `write_failure()` and `main()`. A Python `QuotaError`
exposes `code` and a copied `code_args`; the wrapper must adapt it to
`RunFailure(str(error), error.code, error.code_args)` before the generic
exception handler. It is not currently in that handler's typed-helper list.

Native failures already use
[`src/run_failure.cpp`](../../src/run_failure.cpp):
`classify_run_failure()` and the failure getters preserve the latched quota
code and JSON arguments through later error translation. They write no
stdout/stderr themselves. The wrapper reads the native error text, code and
arguments using its existing `run_failure()` path.

For an ordinary run, each of the three refusals exits **1** and writes one
compact, newline-terminated ASCII JSON failure document on **stdout**.
`engine` and `error` lead; `code` and `args` follow. These are examples using
the current renderer, not a separate quota wire format:

```json
{"engine":"pineforge","error":"run quota refused","code":"run_resource_quota_exceeded","args":{"stage":"execution","used":7,"requested":1,"limit":7,"peak":7,"resource":"scratch_bytes","artifact":"table"}}
{"engine":"pineforge","error":"run quota refused","code":"run_descriptor_limit_exceeded","args":{"stage":"execution","used":4,"requested":1,"limit":4,"peak":4,"process_role":"worker","soft_nofile":4}}
{"engine":"pineforge","error":"run quota refused","code":"run_quota_configuration_invalid","args":{"stage":"admission","reason":"missing_limit","setting":"scratch_bytes"}}
```

`run_resource_quota_exceeded` carries `stage`, `resource`, `artifact`,
`used`, `requested`, `limit` and `peak`. `run_descriptor_limit_exceeded`
carries `stage`, `process_role`, `soft_nofile`, `used`, `requested`,
`limit` and `peak`. `run_quota_configuration_invalid` carries `stage`,
`reason` and `setting`. All reported quota counters are nonnegative integers
at most `2^53-1`. `used` includes committed plus pending capacity before the
denial; `requested` is the additional delta; `peak` precedes the denial.
Output deletion does not refund produced bytes.

The `error` string is diagnostic text, preserved by the renderer; local native
configuration failures can instead say `run quota configuration invalid`.
Consumers use `code` and typed `args`, never English text or argument order.
The renderer's existing text/argument caps and Unicode handling still apply.

Ordinary typed refusal handling adds no traceback or second failure document
on stderr. The wrapper's separate quota line below is still required there.
The existing standalone `csv2bin` path uses the **same** `failure_line()`
JSON on **stderr**, exits **2** for a typed refusal and leaves stdout for
conversion output. A report publication failure after stdout has begun
retains the existing stderr-only diagnostic and exit **1**; no second JSON
document may follow a partially published report.

## Quota line and receipt integration

`quota_line()` returns exactly one of these strings, without a newline or
trailing space:

```text
quota=managed enforcement=cooperative
quota=unmanaged
```

The Python `ENFORCEMENT` and native `kRunQuotaEnforcement` constants are both
`cooperative`. Neither runtime prints the string.

The wrapper must call `mode()` and obtain `quota_line()` in the common startup
path of `main()`, before dispatching ordinary/selected-window/converter work,
and before any input processing or strategy library load. It writes that
string plus one newline to **stderr exactly once per run**, including
admission refusals. It records the identical string once in the usage
receipt, including failed attempts. It must not emit another line for native
attachment or subsequent phases. Receipt persistence belongs to the wrapper
and hosting integration; this note introduces no additional stdout document,
report key or alternate failure format.

The quota line never goes on stdout. That stream continues to carry the
existing success report, typed run refusal document or conversion payload.
The managed receipt must retain `enforcement=cooperative` until host
enforcement is proven; only then may the hosting integration add `host`.

## Enforcement boundary

The cooperative ledger authorizes participating reservations, serializes
them across endpoints and keeps unknown growth charged across disconnects.
It does not establish a sandbox. **PARTIAL** remains the label until hosting
proves enforcement for direct strategy syscalls, descriptor close/replacement,
unreported file growth, all subprocesses and descendants, path/mount escape,
disk and memory-backed storage, descriptor inventories, output publication
and cleanup after exit. HELLO sequencing, nonces and owner credentials are
protocol checks, not a defense against arbitrary code already running inside
the client process.

The host owns creation of both endpoint pairs, their association with the
same attempt, Linux per-message credentials, pidfd or verified process
generation checks, finite storage/descriptor containment and the external
reaper. It must refuse a managed route it cannot contain. A successful
cooperative accounting test alone cannot justify changing the enforcement
label.
