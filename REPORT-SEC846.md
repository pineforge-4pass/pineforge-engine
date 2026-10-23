# SEC846 engine handoff, corrected by SEC846E

Edit-only work is complete in the engine lane. Validation is **NOT RUN**.
HEAD remains `dcbdc1f16fb9ddec6bb4e01b084ca6a057762b3f` on
`ci/sec846-engine`; the supervisor's merge of origin/main
`3a0d1504fd1b7b754ca08fb4fbdbaa7cecb2bbd6` remains in progress.
No index, branch, history or remote was changed by this lane.

This is the full replacement for root REPORT-SEC846.md. The supervisor must
install it and COMMIT-MSG-SEC846.new.md; the root handoff files were read but
not edited. Earlier claims that they were untracked, that HEAD was the
pre-SEC846 base, or that both promotion jobs remained hosted are superseded.

Read in order: the SEC846 base brief, Addendum 2, Addendum 4, the permitted
ci-labels.md canon, and READ-CODEX12.md. The current author case, including
its private-membership comment and permission fallback, remains verbatim.

## PR BODY

`.github/workflows/promote-baseline.yml:69` uses
`pull_request_target: types: [closed]`; line 74 also allows
`workflow_dispatch`. A new first job, `promote-guard` at line 103, must
succeed before the credential job: line 162 is `needs: [promote-guard]`.
Its empty job-level permissions at line 106 override workflow permissions.
It has no checkout, repository secret or OIDC use. GH_TOKEN at line 120
is github.token for author and merger metadata permission reads.

| Credential or permission | File:line | Scope |
| --- | --- | --- |
| `id-token: write` | `.github/workflows/promote-baseline.yml:100` | Workflow grant retained for `promote`; overridden by guard permissions |
| `secrets.PINEFORGE_APP_ID` | `.github/workflows/promote-baseline.yml:252` | Existing App token step |
| `secrets.PINEFORGE_APP_PRIVATE_KEY` | `.github/workflows/promote-baseline.yml:253` | Existing App token step |
| Google OIDC authentication | `.github/workflows/promote-baseline.yml:274` | Existing auth action |
| Provider / service account variables | `.github/workflows/promote-baseline.yml:276` | Existing identity inputs; service account at line 277 |

Every close first checks head-repository equality and the exact false fork
flag, then author authorization, including the canon permission fallback.
OWNER, MEMBER, COLLABORATOR and the two named bots pass the author rule;
every other author needs admin, maintain or write permission. Failed or
empty lookups refuse, including an error that printed a permission first.
This admits a private-member CONTRIBUTOR with write access.

Only an authorized same-repository unmerged close exits successfully
without a merger lookup, with summary
`promote-guard: unmerged pull request; promotion is skipped`.
The existing merged-only credential condition then skips promotion.
A merged own PR independently requires a nonempty MERGED_BY and either a
named bot merger or admin/maintain/write from the merger's own lookup.
A writer author merged by a reader refuses. The bot list is exactly
`dependabot[bot] pineforge-app[bot]`. Dispatch passes; every other event
refuses. An unmerged fork close still fails red.

Exact refusal templates in `.github/workflows/promote-baseline.yml:131`,
line 132, line 144, line 148, line 155 and line 157, respectively:

```text
pull request from another repository (${HEAD_REPO:-unknown})
pull request from a fork
author ${PR_AUTHOR:-unknown} (${ASSOCIATION:-NONE}) has permission '$perm', not write, and is not an allowlisted bot
pull_request_target merger is empty
pull_request_target merger ${MERGED_BY:-unknown} has permission '$perm', not write
event $EVENT does not run baseline promotion (only pull_request_target and workflow_dispatch are allowed)
```

`fail` emits `::error title=authz::<message>`, appends
`authz: REFUSED: <message>` and exits 1. The credential condition has no
status override, so a failed guard prevents it from running.

The original merged-or-dispatch condition remains verbatim at
`.github/workflows/promote-baseline.yml:163`:

```yaml
    if: ${{ github.event_name == 'workflow_dispatch' || github.event.pull_request.merged == true }}
```

Base main is **not** required: the target trigger has no branches filter,
and neither job checks `github.event.pull_request.base.ref == 'main'`.
All executable base-ref bindings/uses remain:

| File:line | Exact text |
| --- | --- |
| `.github/workflows/promote-baseline.yml:189` | `BASE_REF: ${{ github.event.pull_request.base.ref || 'main' }}` |
| `.github/workflows/promote-baseline.yml:193` | `git fetch --quiet origin "$BASE_REF" "+refs/pull/$PR_NUMBER/head:refs/remotes/origin/pr-head"` |
| `.github/workflows/promote-baseline.yml:194` | `if ! git merge-base --is-ancestor "$MERGE_SHA" "origin/$BASE_REF"; then` |
| `.github/workflows/promote-baseline.yml:195` | `echo "::notice::merge commit $MERGE_SHA is not on origin/$BASE_REF; skipping"` |

No base-ref or other workflow condition was changed by SEC846E.

### Shared promotion guard and literal pin

`.github/workflows/promote-baseline.yml:103` is the final reference for the
codegen and workflow lanes. No repository-specific byte belongs in this
guard. It uses github.repository rather than a repository-name literal.
Its full-block MD5, including the separating blank line, is
`933cc1f869787b37f3b339978d3828a2`.

`scripts/ci_preflight.py:389` contains one independent literal copy of that
entire block, including the header, comments and separator.
`scripts/ci_preflight.py:1417` compares the actual complete first job slice
against the literal without `_code` normalization. Missing, duplicate,
renamed and reordered jobs still fail safely before indexing headers.
The literal's MD5 is also `933cc1f869787b37f3b339978d3828a2`.
The workflow, literal and canon author-case MD5 is
`f684468222dd2d9915868e54fee8accc`.

The token-use comment now accurately names both author and merger lookups.
All existing shell behavior is retained. Added must-fail rows at
`scripts/test_ci_preflight.py:808` mutate the real bot comment, step name,
separator and job header; the existing no-op, authorization, lookup,
credential-isolation, dependency and decision mutations remain.

Engine `promote` retains main's `["self-hosted","pf-gcp-main"]` at workflow
line 164. Codegen/template currently retain ubuntu-latest for their
credential jobs. This runner difference is outside the shared guard.
No per-repository guard difference is necessary or authorized.

The codegen and template worktrees were read-only here. Their present guard
hashes are `f1c9bd69db010986a5c168e0d62b03f4` and
`992e4138351a228a119db7956d9487e3`, respectively. They must copy the engine
reference and update their independent literal pins in the following lanes.
Cross-repository equality is not claimed at this engine-first checkpoint.
Exact commands and results are in REPORT-SEC846E.md.

### Release-pool scope after merging main

Main removed POOL_CLASSES and JOB_RUNNERS. The current policy is
JOB_COMMANDS plus RELEASE_JOB_RUNNERS and event reachability through local
reusable calls. Main already refuses retired pf-ci-release labels and
requires pf-gcp-main only for named isolated release jobs, including its
existing baseline-promotion exception. Those pins and main's PR/post fleet
rules remain intact.

`scripts/ci_preflight.py:160` names the old and current release labels for
the scope check. `runner_findings` at line 807 refuses incoming untrusted
events even for a workflow with a release filename and valid direct
triggers. At line 817 it scans runner values and matrix strategy text:
release labels require an existing RELEASE_JOB_RUNNERS job pin.
Renaming a PR workflow or adding a job cannot grant release-fleet access.

`.github/workflows/flat-peak.yml` is absent. The current graph propagates
events, not a proof that a callee has only release.yml callers, and no
flat-peak job is pinned. It receives no release-pool exception by default,
including when a non-release trusted workflow calls it.

Tests at `scripts/test_ci_preflight.py:186`, line 206, line 219 and line 244
mutate real workflow text to assert:

- PR jobs naming either pf-ci-release or pf-gcp-main refuse, under both
  the real CI filename and an arbitrary filename.
- Real release.yml with its current literal pf-gcp-main runner is accepted.
  Restoring pf-ci-release is refused, preserving main's existing negative
  controls rather than re-enabling a retired label.
- A workflow_call flat-peak mutation has no release privilege when orphaned,
  called from PR CI, or called from non-release post-merge CI.
- Local PR call edges cannot reach either pinned release workflow.

All these assertions are authored expectations: **NOT RUN**.
The brief's old positive pf-ci-release example is adapted to main's current
pf-gcp-main pin. The promotion exception is retained because the brief
requires main's new rules and features to stay. Other repositories' pool
checks are outside this engine-only item.

### WIF second layer and blast radius

Read-only sources are local origin/main in
`/Users/haoliangwen/code/pineforge-workflow`. No Terraform was edited.
`terraform/gcp/github-wif.tf:129` joins these requirements:

- repository_owner_id matches var.github_owner_id (line 130);
- repository_id is in the promotion repository IDs (line 131);
- repository is in the promotion names (line 132);
- assertion.ref is refs/heads/main (line 133; ref definition at line 81);
- job_workflow_ref is this repository's
  `.github/workflows/promote-baseline.yml@refs/heads/main`
  (line 134; workflow path at line 69).

Current origin/main:docs/VERIFY.md:1247 describes the exact workflow pin;
line 1248 describes target runs of main's copy and line 1249 the default
branch GITHUB_REF. A base-repository main-pinned pull_request_target run
about a fork PR satisfies these WIF conditions too. **The guard is the
only layer that distinguishes that fork head.** The old sentence beginning
“Fork PRs never could” does not protect this base-context target event.

The service-account capabilities are quoted from current
origin/main:docs/VERIFY.md:1244:

```text
  `pineforge-promotion` service account (Cloud SQL client on the registry
  instance, accessor on the registry-db secret, read on the evidence bucket).
```

Registry database connectivity, access to its database secret and reads of
the evidence bucket are the GCP blast radius. Terraform comments at lines
41 and 42 describe registry_rw database writes; lines 49 through 51 say the
account has no Cloud Run role. Real IAM, OIDC exchange and metadata access
were not queried: **NOT CHECKED**.

### P3 publication record and template scope

The full corrected COMMIT-MSG-SEC846.new.md quotes all added/rewritten
engine documentation and the removed text with source revisions and lines.
Key engine omissions from the prior handoff, quoted from
0212cabe2d87e64b9656791ade118be0bdee01bc:docs/ci.md:

```text
successful on the verified PR head and a merge commit on `main` that carries
```

That removed line is at 771. Line 773 contains the removed `on main` clause,
and line 775 was:

```text
for the tree; every other case exits green without promoting.
```

Line 1059 was:

```text
Promotion stays hosted for both target and dispatch triggers.
```

The corrected record also identifies the intermediate all-hosted guard
sentence from the SEC846 branch, superseded by main's runner migration.
README.md gains four authorization sentences at line 361 and removes none.

Read-only cross-repository P3 inventory, source workflow revision
6d8a82791e87992b64ede831547f12fef66df76f:

| Removed source | Removed claim, quoted verbatim |
| --- | --- |
| `docs/VERIFY.md:1187` | `Fork PRs never could: a workflow run from a fork is not granted write` |
| `docs/CODEGEN-DOCS-GATE.md:144` | `The engine repo's own copy is an older revision; this` |
| `docs/CODEGEN-DOCS-GATE.md:145` | `change touches only the codegen branch, so the engine needs no update for it.` |
| `docs/CODEGEN-DOCS-GATE.md:153` | `The checker is builtins-only and uses the same rule. PR CI uses the trusted` |
| `docs/CODEGEN-DOCS-GATE.md:154` | ``default-branch workflow definition (`pull_request_target`), not the PR's`` |
| `docs/CODEGEN-DOCS-GATE.md:171` | `PR pytest runs in the trusted gate workflow. The separate test workflow` |
| `docs/CODEGEN-DOCS-GATE.md:194` | ``- A fork's code PR runs no code here: its `gate` fails, saying a maintainer`` |
| `docs/CODEGEN-DOCS-GATE.md:196` | `  full gate. A fork's accepted docs proof clears as a same-repository one does.` |
| `docs/CODEGEN-DOCS-GATE.md:208` | `Trust boundary, for whoever reviews the installation PR. The classifier and` |
| `docs/CODEGEN-DOCS-GATE.md:209` | `` the reporter run trusted code from `main`, for a fork's PR too. `differential` `` |
| `docs/CODEGEN-DOCS-GATE.md:210` | ``and `pytest` run the PR head's code under `pull_request_target` only for a`` |

The commit handoff includes the complete removed passages, including event
policy, timeout and trust-boundary text, rather than only these excerpts.
The replacements document the pull_request/authz port and promotion guard.
Codegen's original SEC846 CONTRIBUTING.md change is additive, with no
removed documentation quote to supply.

What the template got beyond the promotion guard/dependency insertion:

- `campaign/ci/promote-baseline.yml:21` replaces
  `# Otherwise it exits green without promoting.` with
  `# After promote-guard passes, unmet tree/gate conditions exit green without promoting.`
- Lines 32 and 33 add
  `# WIF admits main's workflow for fork PRs too. The credential-free`
  and `# promote-guard refuses their heads before promote can use secrets or OIDC.`
  These are accurate but outside item A's literal workflow edit scope.
  **Restore or split out those comments**, as READ-CODEX12 T2 requests.
- `campaign/ci/codegen-gate.yml:7`, line 17, lines 81/131/169/196 and
  lines 202/206 implement item B's trigger, authz, needs and guarded
  aggregate. That port is justified by item B.
- The classifier base-ref change at line 91, PR event tests at lines
  96/117/216/239 and AUTHZ_RESULT wiring at lines 211/221 preserve a trusted
  base checker and fail-closed status reporting under that port. They are
  justified by item B and explicitly included in its scope record.

These are observations of the other worktree, not engine edits. The
workflow lane must restore/split the unrelated comments and carry its own
final handoff and synchronization evidence.

## CONFLICT RESOLUTIONS

| File:line | Resolution |
| --- | --- |
| `scripts/ci_preflight.py:138` | Keep main's hosted macOS and isolated release maps; add only the shared hosted guard exception |
| `scripts/ci_preflight.py:389` | Keep SEC846's independent literal and author canon; retain main's PR/post gate constants at line 449 |
| `scripts/test_ci_preflight.py:735` | Keep all SEC846 guard mutation/decision tests before main's matrix, full-profile, corpus and event tests |
| `scripts/test_ci_preflight.py:379` | Exclude the explicitly hosted guard from the fleet-only mutation helper; its own hosted refusal rows remain |
| `scripts/test_ci_preflight.py:2838` | Use the correct hosted guard expectation alongside main's isolated credential-job expectations |
| `docs/ci.md:1137` | Keep main's new release/fleet description and legacy-metadata retirement; retain SEC846 authorization and test documentation |

The three conflict files have no conflict markers by the required grep.
Git still lists their index entries as unmerged because the supervisor
alone may stage and complete the merge. Main's superseded pool-fragment
prose and old gate constants are not reintroduced.

## NOT CHANGED

- No credential step, secret reference, OIDC scope/provider/account or
  promotion condition was changed beyond the already authorized guard/needs.
- Main's exact release runner and matrix pins, event fleets, authz/aggregate
  checks, PR smoke and full-event populations remain.
- The measured C++ sources, headers, ABI, runtime and corpus were not opened
  or edited for this task. Main's staged changes remain the supervisor's.
- No codegen, template, Terraform, instruction or root handoff file was edited.
- No file was deleted; no git mutation, network, test or repository script ran.

## DECISION

- Use the promotion-specific canon variant because CI authz refuses target
  events; retain Addendum 4's author case and Addendum 2's refusal order.
- Share one complete literal guard block without repository-specific bytes.
- Preserve main's isolated promotion runner outside the shared hosted guard.
- Adapt the retired pool-label request to main's current pins and keep
  flat-peak refused by default until a release-only caller proof exists.
- Quote removed docs as well as replacements; record and split/restore the
  template's unrelated promotion comments in the following workflow lane.
- Keep the existing PR-base behavior; no new main-base condition is inferred.

## COLLECT-ONLY TO RUN

- `scripts/test_ci_preflight.py`: **NOT RUN**. Added lines were reread as
  source only; collection and Python syntax validation still require CI/box.

## GUARD DECISIONS TO RUN

These literal expected exits are assertions in the retained real-script
decision tables, not execution results.

| Case | Expected exit | Status |
| --- | --- | --- |
| Foreign fork head, unmerged | 1 | NOT RUN |
| Foreign fork head, merged | 1 | NOT RUN |
| Same repository with fork=true | 1 | NOT RUN |
| NONE/unknown non-bot author without write | 1 | NOT RUN |
| CONTRIBUTOR writer author, writer merger | 0 | NOT RUN |
| CONTRIBUTOR reader author | 1 | NOT RUN |
| Failed or empty author lookup | 1 | NOT RUN |
| Writer author, reader merger | 1 | NOT RUN |
| Member author, writer merger | 0 | NOT RUN |
| Failed or empty merger lookup | 1 | NOT RUN |
| Empty merger login | 1 | NOT RUN |
| Authorized own unmerged close | 0 | NOT RUN |
| Dependabot author, writer merger | 0 | NOT RUN |
| Manual dispatch | 0 | NOT RUN |
| Push or any unsupported event | 1 | NOT RUN |
| Deleted author lookup, writer-author case | baseline 0 / mutant 1 | NOT RUN |
| Failed author lookup changed to write | baseline 1 / mutant 0 | NOT RUN |
| Disabled fail / removed fork check / early unmerged bypass | baseline 1 / mutant 0 | NOT RUN |

## NOT RUN

| Verification | Status |
| --- | --- |
| `scripts/ci_preflight.py` and all source guards | NOT RUN |
| `scripts/test_ci_preflight.py`, guard decisions and all mutations | NOT RUN |
| Test collection / syntax parser / actionlint / ShellCheck | NOT RUN |
| `scripts/ci_verify.py` release/debug/sanitizers/native profiles | NOT RUN |
| C++ unit suites / ABI matrix / verifier self-tests | NOT RUN |
| Full corpus verification / `scripts/run_corpus.sh` | NOT RUN |
| Org policy checker / TypeSafe diff guard / supervisor Codex read | NOT RUN |

No execution summary or validation log exists. Source-only marker grep,
read-only git diffs and awk/md5 measurements are the only checks performed.
`git diff --check origin/main` over the four edited tracked files returned
no whitespace diagnostics; it is not test evidence.

## NOT CHECKED

CI collection, runtime behavior, parity, required statuses on the merged
tree, real metadata-token lookup permissions, deployed IAM/OIDC, repository
fork settings, final codegen/template synchronization and the supervisor's
handoff installation remain unchecked. The supervisor must run required
verification and commit the merge after the remaining lanes finish.

## FINAL INTEGRATION RECORD (supervisor, 10-23 15:15; supersedes every earlier "pending merge", "HEAD remains", "unmerged index" and "remaining lane" passage above, which are checkpoint accounts)

- This file is part of the completed two-parent merge commit of branch `ci/sec846-engine`: first parent `dcbdc1f16fb9ddec6bb4e01b084ca6a057762b3f` (the earlier SEC846 branch), second parent `3a0d1504fd1b7b754ca08fb4fbdbaa7cecb2bbd6` (origin/main). The three conflict files (`docs/ci.md`, `scripts/ci_preflight.py`, `scripts/test_ci_preflight.py`) are resolved by union; no conflict marker remains.
- Cross-repository adoption is finished: the `promote-guard` block (job header through its blank separator) has the same MD5, `933cc1f869787b37f3b339978d3828a2`, in engine, codegen-oss and the workflow template, and each repo's pin compares the whole block to one independent literal. The pre-adoption hashes and "remaining copying work" above are history.
- Template scope as it stands: the template's cloud SQL proxy wait is IDENTICAL to the template's own origin/main; it was deliberately NOT synchronised with codegen's (the two files differ there on main), so the whole codegen and template promotion files are intentionally not byte-equal at that wait. The extra header comments of the template's promotion file are already restored to main's text; its diff against origin/main is the guard and `needs` insertion only. Any earlier statement here that the proxy loop was copied, or that whole-file MD5s are equal, is a superseded checkpoint observation.
- Nothing was run on the authoring machine (tests, collection, scripts, workflows: NOT RUN).
