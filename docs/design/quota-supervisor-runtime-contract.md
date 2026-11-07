# Quota supervisor runtime contract

This contract describes [`docker/pf_quota_run.py`](../../docker/pf_quota_run.py),
the parent-owned quota ledger supervisor, and its separate CLOUD and ROOT
proofs. Source inventories and expected outcomes below are not runtime
verification results. Receipt-version treatment for the observation diagnostic
remains an explicit publication decision; the implementation and proofs retain
the existing `pf-quota-receipt/v1` literal.

The existing [host interface](run-quota-host-interface.md) still governs the
cooperative ledger, its four mandatory finite resource allowances, the
managed marker, protocol, and hosting integration boundary.

## Refused observation paths

A caller-requested memory-events path rejected by the product's sensitive-name
policy may leave OOM evidence unknown only with a typed diagnostic that names
the exact refused path. Silence cannot stand for that diagnostic. This does
not authorize observing a sensitive file, inventing a counter, or substituting
another path.

`_path_allowed()` refuses components `.ssh`, `.aws`, `.codex`,
any component ending in `.env`, `.config/zsh/keys`, and `.config/pineforge`.
Both the requested spelling and its absolute spelling are checked before
filesystem observation. The receipt retains `oom.proven`, `before`, `after`,
and `delta`; all three counters are null when unknown.

There is an existing typed envelope, `{code, args}`, and a
`launcher_failure` carrier. That carrier also aborts supervision, participates
in first-error selection, and changes the launcher's exit status. Treating it
as a nonfatal observation diagnostic would silently add execution policy.
The ledger refusal's fixed resource/setting vocabulary cannot carry a path.

Nullable `oom.diagnostic` uses the existing typed envelope and existing
code, stage, reason, and option vocabulary, with one explicit path argument:

```json
{"code":"quota_launcher_error","args":{"stage":"oom_observation","reason":"arguments","option":"--memory-events","path":"<exact requested literal>"}}
```

`path` and the diagnostic's placement are the additive schema extension.
For an absent observation request or a permitted observation control the
diagnostic is absent/null. Unknown's existing four facts retain their
meanings. A diagnostic alone does not change the wait, ledger, or deadline
facts. Receipt-version treatment must be decided before accepting this
extension for publication. The source proof deliberately
fails on a missing diagnostic; it never supplies one as observed data.

The six R01 paths are varied, nonexistent literals below the fresh row's
`literal-only` name. No sensitive component is created, resolved, listed,
statted, or opened by the proof. The complete SUT receives the literal only
after source admission confirms lexical refusal before filesystem access.
Mutations that remove that safety check are outside this runnable population.
C11 supplies an actual ordinary file named `memory.events`, with misleading
counter text; it must remain unproven. C12 supplies actual kernel cgroup
counters and independently verifies admission and subsequent emptiness,
without inducing OOM. These controls distinguish ignored inputs and a
diagnostic emitted for every requested path.

## Receipt verdict and separate wait facts

The trusted receipt verdict is the run result. In particular, a non-null
`ledger_verdict` remains a refusal when the leader exits zero and the
supervisor also exits zero. A null ledger verdict is not sufficient for
success when `launcher_failure` is non-null, a deadline expired, or the real
workload wait reports failure. Consumers must inspect those distinct facts.

`wait_status`, `exit_code`, and `signal` describe the supervisor's actual
leader wait. They are not a second encoding of the ledger verdict. The
proof's independent `supervisor.wait.json` describes the observer's OS wait
for the supervisor. `work.wait.json` describes the controller's OS wait for
that row's proof process. These three process levels are never conflated.

C02 consumes varied explicit output allowances through a real shipped client,
writes the actual allowed artifact through an explicitly forwarded fd, then
requests a varied additional amount. The client catches the real refusal and
returns zero; delegated variants also record the bridge's real wait. The
private receipt must retain the exact resource, used, requested, limit, and
peak values. C01 performs permitted work, FINISHes, and returns 0 or 17 as
registered; only its zero-exit members are successful-run controls.

The receipt fd must be a caller-owned private carrier. This proof uses a
parent-created pipe, passes its writer only to the SUT, closes the observer's
writer after spawn, and obtains authority only from that pipe's bytes.
The child and descendants fstat their actual descriptor inventories against
that exact pipe's device/inode. An output artifact fd is separately passed
through the SUT's real `--pass-fd`. No child stdout/stderr or receipt-shaped
file supplies authority or permission. Existing child-forgery coverage stays
in the original ROOT proof; this file does not replace it.

## Actor authentication

An endpoint admits an actor only after the kernel and host checks establish:

1. Exactly one kernel `SCM_CREDENTIALS` record, decoded as signed pid and
   unsigned uid/gid; positive pid and the exact requested child uid/gid.
2. A live pidfd and `/proc` generation; the actor's process group **and**
   session both equal the actual leader pid.
3. All four Linux `Uid:` fields (real, effective, saved, filesystem) equal
   the child uid; all four corresponding `Gid:` fields equal the child gid;
   supplementary groups are empty.
4. A finite, nonnegative actual soft `RLIMIT_NOFILE`. The current product
   uses real `prlimit`, or the actual procfs limits record when cross-uid
   `prlimit` is unavailable. This is not a default limit.
5. Generation/liveness still match before registration, followed by a
   recheck of generation/liveness, all credential fields/groups, and the
   exact same soft fd limit after the ledger opens its own process handle.

Each endpoint binds exactly one actor for the run and then closes. It cannot
be shared or reassigned to a second process. The ledger service checks the
real per-message credentials, nonce, role, sequencing, and protocol on every
frame. Naming these rules does not weaken authentication.

All launched rows independently witness the four uid/gid fields, empty groups,
real group/session, finite soft limit, and descriptor custody before releasing
the workload barrier. C03 changes only the descendant's group, or its session
and group, before a real HELLO. Both must produce authentication-stage
`actor_outside_owned_run`, kill/reap the owned tree, and preserve the actual
SIGKILL wait. C04 varies the actual soft limit and endpoint, with aggregate
descriptor policy larger than the soft limit; `soft+1` must receive the
typed descriptor refusal naming the actual soft value.

C05 is a separate instrumented kernel experiment. A trusted wrapper executes
the complete SUT under `PTRACE_TRACEME`; it does not import or copy the SUT.
The observer follows actual endpoint threads with `PTRACE_GET_SYSCALL_INFO`.
After the second successful native `pidfd_open` for the independently observed
actor, it changes that actor's real soft limit with cross-uid `prlimit`:
native 96→95, Python 112→111. That stop is the ledger's registration pin,
after the initial checks and before the final recheck. The required outcome
is `actor_changed_during_registration`; disconnecting that newly registered,
unfinished owner also leaves a genuine `protocol/channel` ledger refusal.
Missing syscall metadata, a different call path, failed intervention, or
denied facilities is INCOMPLETE. No race win or guessed timing is evidence.

C06 first proves a valid HELLO/snapshot, keeps that actor alive, and forks a
second process that sends the next valid request using the inherited real
client state and endpoint. It must poison the ledger with `protocol/channel`
even though the leader exits zero. The second actor's pid is independently
pinned. No SCM credentials are forged.

The new proof does not claim negative mutation coverage for every saved or
filesystem identity field, supplementary-group mutation, infinite nofile
(not normally representable on Linux), high-ID kernel transitions, or every
registration recheck conjunct. Initial credential facts are real positive
evidence. The original ROOT high-ID/outsider cases and later admitted
mutations remain separate required evidence.

## Optional caller deadline and retained runtime behavior

`--deadline-seconds` is **optional, without a default**. Omission means no
deadline at all, not a long duration, a receipt-selected policy, or an
inferred hosting budget. R02 actually omits the option. On admission it must
retain `deadline.seconds=null`, `expired=false`, and `kill_sent=false` while
performing the commanded workload. No harness clock or user time cap is an
oracle for this requirement.

An explicitly supplied policy retains the existing positive finite decimal
grammar `[0-9]+(\.[0-9]+)?`, including leading zeros, and existing rejection
of zero, malformed, nonfinite, overflowing or underflow-to-zero values.
The current implementation converts it to float and starts its
`CLOCK_MONOTONIC` deadline after argument validation, before privileged setup;
setup time is included. Expiry uses SIGKILL without SIGTERM grace. The
existing setup gate can prevent exec when that explicit deadline has already
expired. The receipt does not currently distinguish pre-exec expiry from
expiry after execution. No new cap is introduced by this declaration.

The mandatory-deadline baseline emits
`quota_launcher_error/admission/invalid_deadline` when the option is absent.
The absent-deadline regression rows require actual work instead, covering
admission, the setup gate, and the supervision loop with no deadline value.

The following current behavior is retained:

| Area | Runtime contract |
| --- | --- |
| Subreaper | `PR_SET_CHILD_SUBREAPER`; on normal exit and failure, kill the owned leader group and every adopted orphan, including descendants in another session, and reap until no child remains. No cleanup time bound is added. |
| PID custody | Hold the unreaped leader and its pidfd/generation through signaling; numeric fallbacks rely on retained ownership, not an unpinned pid. |
| Launcher signals | SIGHUP, SIGINT, SIGQUIT, SIGTERM become typed `launcher_interrupted` with the actual signal; SIGKILL the run and return `128+N`. These signals are not forwarded as workload signals. |
| Exec admission | Child sets its session, attempts the optional cgroup entry, closes parent controls including the private receipt, drops privileges/capabilities/groups, sets no-new-privileges and parent-death SIGKILL after the drop, and blocks at the gate. Parent checks generation/group/session and cgroup admission before releasing exec. |
| Environment | Inherit the launcher's environment, replacing the three quota markers and four limit variables from trusted launcher inputs. No wholesale environment scrub is currently implemented. The proof uses a deliberately small synthetic environment, including varied unrelated sentinels and poisoned old markers/limits. |
| Limits | All four finite ledger allowances stay mandatory; `0..2^53-1`, zero is zero. Making deadline optional does not change these limits. |
| Preconditions | Require Linux containment facilities, real root launcher, separate non-root child credentials, a fresh launcher with no existing children, one thread before fork, valid arguments and a private usable receipt carrier. |
| Typed failures | Preserve `privileged_launcher_required`, `linux_containment_unavailable`, `launcher_already_has_children`, `threads_before_fork`, `child_identity_not_separate`, explicit-policy `deadline_before_spawn`, receipt/collision/argument faults, and typed child setup failure including operation/errno. |
| OOM | Optional observation only. Proven attribution needs the actual exclusive, empty, root-owned, undelegated cgroup; pinned controls, admission before exec, emptiness after cleanup, and a positive real `oom_kill` delta. Otherwise unknown, with the additional diagnostic when policy refused the requested name. |

C07 verifies real escaped-orphan adoption and reaping on normal exits 0/17.
C08 varies all four real launcher signals while the child ignores them.
C09 waits for an actual caller-policy SIGKILL without measuring elapsed time.
C10 actually aborts the workload and preserves the real SIGABRT wait.
C13 covers missing uid/gid, zero uid/gid, and collision of the actual private
receipt writer with `--pass-fd`. C14 requests a genuinely nonexistent
executable and requires the real wait 126 and typed `exec/ENOENT`.

## CLOUD host contract and separate ROOT evidence

`docker/run_json_quota_supervisor_test.py` is a Linux, non-root harness in
preflight's `run_json_*_test.py` discovery glob.
Its admission rows require ordinary uid and gid in `1..2^31-1`,
with real/effective/saved ids equal, and real Linux credentials/procfs.
Host identity must be measured, not inferred from a profile name, image tag,
serialized credential input, or stale job. No skip counts as PASS.

The source inventory is 129 CLOUD rows in 15 families and 176 ROOT
rows in 37 families; these numbers are not fresh collection or execution
evidence. CLOUD admission and parser rows do not establish high-ID kernel
delivery or privileged transitions. The ROOT proof retains its full-range
kernel identity rows as separate required evidence.
ROOT is a separate route with its own real host prerequisites and results.
The non-root file runs on interpreters with or without `os.pidfd_open` and
`signal.pidfd_send_signal`, including the shared preflight interpreter without
those native functions. When either function is absent, the supervisor uses
libc's `syscall` through stdlib ctypes: `pidfd_open` is syscall 434 and
`pidfd_send_signal` is syscall 424 on 64-bit Linux x86_64 and aarch64
(including the machine-name aliases amd64 and arm64). Any other architecture
without the required native functions retains the typed
`linux_containment_unavailable` refusal. A missing kernel facility is a setup
failure, never a skip. CLOUD rows exercise real descriptors, poll readiness,
signals, errno subclasses and CLI refusals with the native functions hidden.
The native-comparison row still runs all kernel assertions when the interpreter
has no native functions; only its direct native comparison is unavailable.
ROOT includes normal-exit and deadline/group-reaping proofs with those
functions hidden in the supervisor process. ROOT and the 47-row policy proof
keep their existing prerequisites and remain explicit, separate actions;
CLOUD collection does not substitute for either privileged proof.

Both original suites retain missing-child-identity refusals. The CLOUD
absent-deadline row requires the real non-root launcher refusal with null
seconds, proving that omission reached privileged admission. The ROOT
absent-deadline row requires an actual dropped-identity child, its output,
real exit 17, null seconds, and cleanup. Null seconds do not imply pre-launch
failure; invalid-argument rows still require absent work and absent wait facts.

## Exact new discovery and registration proposal

Register `docker/pf_quota_policy_privileged_proof.py` explicitly as a separate
ROOT policy action. It is a stdlib
standalone proof, not a pytest file or an addition to the preflight glob. No
existing workflow is edited here. Admission must freeze its whole file,
this contract, the entire SUT and ledger, and the existing CLOUD/ROOT inputs.

Remote-only discovery command:

```text
python3 -I -B docker/pf_quota_policy_privileged_proof.py --list-rows
```

Discovery returns `DISCOVERY_ONLY`, exact ordered row IDs **and every input
field**, never an execution verdict. The controller's `required_rows` must
equal that admitted manifest exactly; a count alone is insufficient.
The source registration has 47 rows: 37 controls and 10 intended target rows.
This is an authored inventory, not an observed collection count.

| Family and exact suffix expansion | Rows | Expected own result on unmodified base | Expected after admitted repair |
| --- | ---: | --- | --- |
| `C00.recorder-deliberate-crash` | 1 | Crashed work: INCOMPLETE/exit 2, no completion; control judge GREEN | Same |
| `C01.normal.{native,python}.{leader,descendant}` | 4 | GREEN/exit 0 | GREEN/exit 0 |
| `C02.refusal-exit0.{native,python}.{leader,descendant}` | 4 | GREEN/exit 0; actual SUT leader/supervisor exit 0 with ledger refusal | Same |
| `C03.membership.{native,python}.{pgrp,session}` | 4 | GREEN/exit 0; typed authentication refusal | Same |
| `C04.finite-nofile.{native,python}.{64,144}` | 4 | GREEN/exit 0; typed descriptor refusal | Same |
| `C05.registration-recheck.{native,python}` | 2 | GREEN/exit 0 if real barrier/intervention established | Same |
| `C06.one-actor.{native,python}` | 2 | GREEN/exit 0; typed ledger refusal | Same |
| `C07.cleanup.normal-exit{0,17}` | 2 | GREEN/exit 0 | Same |
| `C08.launcher-signal.{SIGHUP,SIGINT,SIGQUIT,SIGTERM}` | 4 | GREEN/exit 0 | Same |
| `C09.explicit-caller-deadline` | 1 | GREEN/exit 0 | Same |
| `C10.real-leader-crash` | 1 | GREEN/exit 0; actual SUT exits 128+SIGABRT | Same |
| `C11.permitted-noncgroup-path` | 1 | GREEN/exit 0 | Same |
| `C12.permitted-kernel-cgroup` | 1 | GREEN/exit 0 if real cgroup admission established | Same |
| `C13.precondition.{missing-uid,missing-gid,root-uid,root-gid,receipt-forwarding-collision}` | 5 | GREEN/exit 0 | Same |
| `C14.real-exec-failure` | 1 | GREEN/exit 0 | Same |
| `R01.refused-path-diagnostic.{ssh,aws,codex,env-suffix,zsh-keys,pineforge-config}` | 6 | Intended RED/exit 1 at `P2_1_REFUSED_PATH_HAS_TYPED_DIAGNOSTIC` | GREEN/exit 0; publication also requires receipt-version approval |
| `R02.absent-deadline.{native,python}.{leader,descendant}` | 4 | Intended RED/exit 1 at `P2_4_ABSENT_DEADLINE_ACCEPTED` | GREEN/exit 0, null seconds and real work |

Ordering is exactly `rows()` order, including its native block followed by
its Python block. The table groups families for reading; it does not reorder
execution. Limits, deltas, actor ownership, endpoint and requested leader exit
vary in the manifest. There is no row filtering in `--run`. `--work` is an
internal single-work entry; using it alone cannot establish whole-proof
completion. A failed control or INCOMPLETE row stops the sequence immediately.
Unexpected target assertion IDs also stop it as INCOMPLETE.

## Remote prerequisites and result handling

Use a separately allocated validation host with the prerequisites below.
The proof performs no provider action, installation, or allocation.
The policy proof does not induce OOM; the original ROOT proof's induction
rows require separate authorization. C12 does not create cgroups, set limits,
or allocate for OOM.

The host must supply:

- Real 64-bit Linux root, visible procfs, pidfd open/signaling, waitid/WNOWAIT,
  Unix seqpacket sockets with per-message credentials, subreaping and the
  capabilities needed for the SUT's irreversible credential/capability drop.
- Python 3.10 or later with the used Linux APIs; a checkout and interpreter
  readable/traversable by the explicit child uid/gid. A real transition probe
  runs before each SUT experiment. No uid/account fallback is chosen.
- Native x86-64 or aarch64 for C05, kernel syscall-info tracing/clone events,
  permission to trace the protected launcher, and cross-uid `prlimit`
  capability. Root in a restricted container alone is not sufficient.
- A finite host soft nofile above 272 for the private receipt writer, and a
  hard limit at least 144. Per-actor test inputs remain explicit finite
  64/96/112/144 values rather than inferred limits.
- An existing sealed, exclusively reserved, empty cgroup v2 leaf supplied as
  `--memory-events-control`; memory accounting enabled, complete visible
  undelegated ancestry, actual protected controls, usable observation lock,
  and sufficient ordinary workload memory. Observer/supervisor stay outside
  that leaf. The owner creates it separately; this file does not allocate one.
- A root-owned named `.policy-author-scratch` directory inside the admitted
  remote checkout. Use a fresh child result name. The proof creates new
  outputs exclusively, retains everything, and uses no system temp directory.

The exact remote command must be registered with actual values in place of
the placeholders below; these are required caller inputs, not defaults:

```text
python3 -I -B docker/pf_quota_policy_privileged_proof.py --run --child-uid CHILD_UID --child-gid CHILD_GID --caller-deadline-seconds EXPLICIT_POLICY_SECONDS --memory-events-control /HOST_RESERVED_LEAF/memory.events --result-dir /ADMITTED_CHECKOUT/.policy-author-scratch/FRESH_RUN_NAME
```

The explicit policy applies to the positive and negative explicit-policy
controls, including C09. It is never supplied to R02's supervisor. Choose it
as actual caller policy; if it expires before a row establishes the required
actors/intervention, the experiment is INCOMPLETE. There is no harness timeout
and no use of elapsed time to infer deadline behavior. A hang, interruption,
wrong host, missing permission, unavailable facility or setup error cannot
be called RED or GREEN; absence of completed results remains INCOMPLETE.

Each row retains `RUN.START.json`, work stdout/stderr, `work.wait.json`,
`work.exit`, its own `.exit`, and `RUN.RESULT.json`. Completed work has
`RUN.COMPLETE` binding the result's actual bytes and SHA-256. Product rows
also retain the raw `supervisor.receipt`, stdout/stderr, workload events,
actual supervisor wait, actual output artifact, real identity-probe output
and wait; C05 retains its syscall/intervention record. The result retains
actual host facts and before/after source hashes. Kernel-control observations
must be retained with the row rather than inferred from a label.

The controller judges the work's actual OS wait together with its own
result, completion binding, registered inputs, and exit agreement.
C00's genuine exception travels through the same work recorder: own exit 2,
INCOMPLETE with `DeliberatePolicyProofCrash`, no completion; the outer
controller's wait and `work.exit` must agree before that control is GREEN.
The outer wait also records nonzero signal termination when an uncatchable
work crash cannot write its own `.exit`; missing own evidence is INCOMPLETE.
The remote native executor must independently retain the controller's real
exit/log as well. No marker, wrapper success, or last printed line is a
verdict, and no pipeline is used by this proof.

A completed baseline proof is expected to have all controls GREEN, all ten
target rows RED for the named assertions, controller exit 1, and a COMPLETE
binding to a RED result. This is **observed RED only if the actual row files
say so**. A setup/control stop yields controller INCOMPLETE/exit 2 and no
completion binding. A fully repaired population would yield GREEN/exit 0;
that claim still requires exact-candidate remote evidence and admitted
mutations, not this source prediction.

Validation requires registered discovery, meaningful remote RED/controls,
source review, and exact-candidate remote GREEN and mutation evidence.
Final verification must bind the exact candidate,
full native/guard actions, and every remote row's own results/log/wait.
Ordinary C++ unit tests and full corpus verification remain required before
claiming a product change complete.
