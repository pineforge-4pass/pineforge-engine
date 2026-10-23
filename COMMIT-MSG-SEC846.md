ci: pin the shared promotion guard and scope isolated release runners

Require a credential-free promote-guard before baseline promotion. Every
pull_request_target close checks the same-repository, non-fork head and the
canonical author association/bot/permission rule before the unmerged exit.
Private-member CONTRIBUTOR authors need admin/maintain/write permission;
failed or empty lookups refuse. Merged PRs independently require a nonempty,
allowlisted or write-capable merger. Dispatch is allowed; other events refuse.

Use the engine guard as the reference for all three repositories. Pin the
complete job literally, including its header, comments and separating blank
line, rather than discarding comments. Keep the canon author case verbatim.
Mutations alter the real workflow, including its header, comments, step name,
separator, authorization checks, dependencies and credential isolation.
The existing decision tables execute the extracted real shell with only gh
stubbed by username; both author-lookup mutations retain concrete exit flips.

Resolve the supervisor's merge by retaining main's event/call-graph routing,
release pins, PR/full-event split, aggregates, tests and other features, plus
SEC846's guard, dependencies, literal and decision tests. The shared guard
stays on ubuntu-latest. Engine's credential job retains main's pf-gcp-main
runner outside the shared block; codegen/template credential routing is
outside that block too.

Main retired POOL_CLASSES, JOB_RUNNERS and pf-ci-release. Extend its current
JOB_COMMANDS / RELEASE_JOB_RUNNERS / workflow_event_routes model: release
pool labels require an existing isolated job pin, and incoming PR events
cannot reach those jobs through reusable calls. Preserve main's named
promotion exception. The positive release case uses pf-gcp-main; restoring
pf-ci-release remains a failure. No flat-peak.yml exists, and the event-only
model has no release-only caller proof, so grant it no release exception.
New refusal rows mutate the real CI and reusable-workflow text.

Engine guard and independent literal MD5: 933cc1f869787b37f3b339978d3828a2.
Canon/workflow/literal author-case MD5: f684468222dd2d9915868e54fee8accc.
Codegen and workflow lanes must copy this finished engine reference; their
current differing hashes are recorded in REPORT-SEC846E.md, not called equal.

Validation: NOT RUN for every test, script, collection, workflow, C++ suite
and full corpus sweep. This is an offline edit-only lane. No staging, commit,
history change or push was performed. The supervisor installs this full
replacement as COMMIT-MSG-SEC846.md and installs REPORT-SEC846.new.md as
REPORT-SEC846.md; neither root handoff file was edited here.

Documentation added or rewritten, quoted verbatim:

README.md:361:

```text
`promote-guard` runs before promotion credentials are available and rejects foreign or fork heads and disallowed authors on every PR close, including unmerged closes.
Authors outside the association and bot allowlists must have `admin`, `maintain` or `write` permission from GitHub; failed or empty author lookups refuse promotion.
Merged PRs also require an allowlisted bot merger or a merger with write access; manual dispatches are allowed.
See [CI documentation](docs/ci.md) for author associations and permissions, merger permissions and unmerged-close behavior.
```

docs/ci.md:852:

```text
(`pull_request_target`) and runs no PR code. It requires both statuses to be
successful on the verified PR head and a merge commit on the PR's base branch that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on the base branch). It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; after the guard succeeds, ineligible baseline cases exit green
without promoting.
```

docs/ci.md:860:

```text
`promote-guard` must pass before the credential job runs.
Every `pull_request_target` close requires a non-fork head in this repository and an author whose association is `OWNER`, `MEMBER` or `COLLABORATOR`, whose login is an allowlisted bot, or whose GitHub permission is `admin`, `maintain` or `write`.
The permission fallback admits private organization members reported as `CONTRIBUTOR`; a failed or empty author lookup refuses promotion.
Only after those checks can an unmerged close pass without a merger lookup and skip `promote`.
For a merged PR, the merger login must be nonempty and must be an allowlisted bot or have `admin`, `maintain` or `write` permission; a failed or empty lookup refuses promotion.
Only `dependabot[bot]` and `pineforge-app[bot]` are allowlisted bots.
Manual `workflow_dispatch` events pass the guard because dispatch requires write access.
Every other event fails the guard before credentials are available.
The trigger has no `branches:` filter and the guard does not require a `main` base ref; the existing tree check reads the PR's base ref, defaulting to `main` for dispatch.
```

docs/ci.md:1155:

```text
The credential-free `promote-guard` stays hosted on `ubuntu-latest`; the credential job keeps main's isolated release runner.
The same stage pins the complete shared `promote-guard` block byte for byte, including comments, empty permissions, payload bindings, refusal script and separator, and the merged-or-dispatch credential job's dependency on it.
Decision tests execute the guard script extracted from the real workflow with bash and an executable `gh` stub first on PATH that returns separate author and merger permissions by username.
Mutation tests alter the real promotion workflow to replace the script with `exit 0`, move the unmerged exit before authorization, remove author or merger lookups and other checks, disable `fail`, bypass failed author or merger lookups, add guard credentials or a checkout, or disconnect the credential job.
The release pool is restricted to the existing pinned release and promotion jobs; reusable call edges cannot admit PR events to them.
Retired `pf-ci-release` labels remain refused, and an unpinned `flat-peak.yml` receives no release-pool exception.
```

Removed documentation quotations omitted from the prior handoff (P3 E2):

Source revision: engine 0212cabe2d87e64b9656791ade118be0bdee01bc,
the parent of the committed SEC846 pass. docs/ci.md:770:

```text
(`pull_request_target`) and runs no PR code. It requires both statuses to be
successful on the verified PR head and a merge commit on `main` that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on `main`). It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; every other case exits green without promoting.
```

The removed main-base assertion is replaced by the actual PR-base behavior,
and the unconditional green-exit wording is qualified by guard success.
The same original text is at origin/main:docs/ci.md:852 before this merge's
SEC846 resolution. Original docs/ci.md:1059:

```text
Promotion stays hosted for both target and dispatch triggers.
```

That historical sentence was changed by SEC846 to the following sentence at
dcbdc1f16fb9ddec6bb4e01b084ca6a057762b3f:docs/ci.md:1070:

```text
Promotion and its `promote-guard` stay hosted for both target and dispatch triggers.
```

Main's runner migration supersedes both all-hosted claims. The final
docs/ci.md:1155 explicitly distinguishes the hosted guard from the isolated
credential job. The obsolete pool-fragment narrative removed by main is not
restored during conflict resolution.

Cross-repository P3 T3 removed quotations (read-only inventory; no template
or codegen file was edited by the engine lane):

Source revision: workflow 6d8a82791e87992b64ede831547f12fef66df76f,
docs/VERIFY.md:1187:

```text
  Fork PRs never could: a workflow run from a fork is not granted write
  permissions — `id-token: write` included — unless an admin turns on "Send
  write tokens to workflows from pull requests"; keep it off. The template
  checks out and runs no PR code under `pull_request_target`.
```

This was replaced by the explanation that main-pinned target runs for fork
PRs satisfy WIF and need the promotion guard.

Same revision, docs/CODEGEN-DOCS-GATE.md:140:

```text
Install all five in the same codegen PR. `promote-baseline.yml` replaces the
live copy, which is byte-identical to the previous template: it now reads the
newest `gate` commit status on the PR head (the reporter's in practice; the
workflow does not check its creator) and still accepts the legacy check-run.
The engine repo's own copy is an older revision; this
change touches only the codegen branch, so the engine needs no update for it.
```

Same revision, docs/CODEGEN-DOCS-GATE.md:153:

```text
The checker is builtins-only and uses the same rule. PR CI uses the trusted
default-branch workflow definition (`pull_request_target`), not the PR's
editable workflow. Its classifier reads the exact head as data in a separate
checkout, executing only the trusted checker. Its preflight resolves Git commits/tree/diff; its docs check proves
```

Same revision, docs/CODEGEN-DOCS-GATE.md:167:

```text
policy when installing: it must permit this trusted workflow's
`pull_request_target` event. Do not weaken permissions or inject secrets into
```

Same revision, docs/CODEGEN-DOCS-GATE.md:171:

```text
PR pytest runs in the trusted gate workflow. The separate test workflow
```

Same revision, docs/CODEGEN-DOCS-GATE.md:175:

```text
The test workflow runs once per pushed commit (`concurrency` keyed by the
SHA), so a docs-only push never cancels a code push's tests. Every job is
capped: 10 minutes for the classifiers and the reporter, 30 for the
differential gate and each pytest job.
```

Same revision, docs/CODEGEN-DOCS-GATE.md:182:

```text
- The in-job gate-relevant path filter is gone: every PR that is not an
  accepted docs proof runs the full Pyodide differential gate. Before, a PR
  touching no gate path was trivially green.
```

Same revision, docs/CODEGEN-DOCS-GATE.md:194:

```text
- A fork's code PR runs no code here: its `gate` fails, saying a maintainer
  must push the branch to the repository and open a PR from it, which runs the
  full gate. A fork's accepted docs proof clears as a same-repository one does.
```

Same revision, docs/CODEGEN-DOCS-GATE.md:203:

```text
`PINEFORGE_EVIDENCE_BUCKET`). Actions must allow `pull_request_target` (the
default). The operator recording with `lab gate docs` needs the registry
```

Same revision, docs/CODEGEN-DOCS-GATE.md:208:

```text
Trust boundary, for whoever reviews the installation PR. The classifier and
the reporter run trusted code from `main`, for a fork's PR too. `differential`
and `pytest` run the PR head's code under `pull_request_target` only for a
same-repository head: both carry `github.event_name != 'pull_request_target'
|| github.event.pull_request.head.repo.full_name == github.repository`, so a
fork's code never runs in this base-context workflow (a deleted fork counts as
a fork), while nightly and manual runs stay full. The reporter clears the full
gate only for a same-repository head; whenever a fork's PR does not pass, the
description says a maintainer must push the branch here and open a PR from it.
A fork's run never overwrites a `gate` success that a run of this repository
(`github-actions[bot]`) posted on the same commit: it leaves it and exits 0, so
a fork PR sharing a same-repository PR's head shows that PR's pass instead of
turning it red. Even so, the two jobs hold only `contents: read` (no secrets,
no caches, no OIDC token, no persisted credentials) and the reporter alone
holds `statuses: write` without checking out PR code. None of codegen's
```

The template replaces these claims with the authorized pull_request/authz
port, base-checker checkout, guarded reporter, and promotion guard. These doc
corrections are justified by base brief items B and D, not a reason to port
unrelated workflow behavior. Codegen CONTRIBUTING.md gains authorization
prose; the original SEC846 change removed no codegen documentation.

Template scope omitted from the prior handoff (P3 T2), current template paths:

- campaign/ci/promote-baseline.yml:21 replaces
  `# Otherwise it exits green without promoting.` with
  `# After promote-guard passes, unmet tree/gate conditions exit green without promoting.`
- campaign/ci/promote-baseline.yml:32 adds
  `# WIF admits main's workflow for fork PRs too. The credential-free`
  and line 33 adds
  `# promote-guard refuses their heads before promote can use secrets or OIDC.`
- Those accurate comments are outside item A's literal guard/needs-only
  workflow scope. Restore them or split them out in the workflow lane, as
  READ-CODEX12 T2 requests; they are not justified as additional code edits
  by this item. The engine lane leaves the other worktree read-only.
- campaign/ci/codegen-gate.yml:7 changes the trigger; line 17 adds authz;
  lines 81, 131, 169 and 196 add dependencies; line 202 adds refusal first;
  line 206 guards the reporter. These are explicitly authorized by item B.
- campaign/ci/codegen-gate.yml:91 changes the trusted checker ref to
  `${{ github.event.pull_request.base.sha || github.sha }}`. Lines 96, 117,
  216 and 239 adapt event tests to pull_request; lines 211 and 221 bind and
  require AUTHZ_RESULT. These are necessary to preserve the trusted checker
  and fail-closed reporter during item B's event port; keep them with that
  port, rather than silently omitting them from its scope record.

FINAL INTEGRATION RECORD (supervisor, 10-23 15:15; supersedes every earlier "pending merge", "unmerged", "remaining copying" and "still requires restoration" passage above, which are checkpoint accounts):
- This is the completed merge commit of `ci/sec846-engine`: parents `dcbdc1f16fb9ddec6bb4e01b084ca6a057762b3f` and origin/main `3a0d1504fd1b7b754ca08fb4fbdbaa7cecb2bbd6`; conflicts in docs/ci.md, scripts/ci_preflight.py and scripts/test_ci_preflight.py resolved by union.
- The promote-guard block has MD5 933cc1f869787b37f3b339978d3828a2 in engine, codegen-oss and the workflow template; each pin compares the whole block to one independent literal.
- The template's extra header comments are already restored to main's text, and its cloud SQL proxy wait is identical to the template's origin/main (not synchronised with codegen, whose wait differs on main); the whole codegen and template promotion files are intentionally not byte-equal at that wait.
- Nothing was run on the authoring machine (NOT RUN).
