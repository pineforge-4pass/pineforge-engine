# SEC846 engine lane handoff

HEAD/base: `0212cabe2d87e64b9656791ade118be0bdee01bc`.
The changes are an unstaged worktree diff for the supervisor to commit.
No repository code, parser, workflow, script, build or test was executed.
Canon read: `/Users/haoliangwen/.config/laneboss/orgs/pineforge/ci-labels.md`,
sections "The `authz` job" and "The required aggregate job".
Addendum 2 supersedes the first pass's unmerged-close ordering. Short
Addendum 4 supersedes the old association/bot-only author refusal with the
16:02 canon's author-permission fallback; its nested case, comment, lookup
and refusal wording are copied without modification. This lane edits only
the engine worktree; the codegen and workflow-template lanes must supply
their own repo-specific reports and edits.

Tracked worktree scope from read-only `git diff --stat` (includes the
existing first and second passes and these Addendum 4 edits):

```text
 .github/workflows/promote-baseline.yml |  59 ++++++
 README.md                              |   5 +
 docs/ci.md                             |  22 +-
 scripts/ci_preflight.py                |  65 +++++-
 scripts/test_ci_preflight.py           | 356 ++++++++++++++++++++++++++++++++-
 5 files changed, 501 insertions(+), 6 deletions(-)
```

The two additional root files, `COMMIT-MSG-SEC846.md` and this report,
remain untracked. HEAD is still the base; no commit or staged diff exists.

## PR BODY

`.github/workflows/promote-baseline.yml` runs in the base repository on
`pull_request_target` with `types: [closed]`
(`.github/workflows/promote-baseline.yml:69`, line 70) and on
`workflow_dispatch` (`.github/workflows/promote-baseline.yml:74`).
Its credential job previously had no head-repository and actor authorization
dependency. Add `promote-guard` as the first job and require
`needs: [promote-guard]` on `promote`.
The author case now copies the updated canon: a private organization member
reported as `CONTRIBUTOR` can proceed with GitHub-confirmed write access.

Credential locations in the edited workflow:

| Credential or permission | File:line | Job |
| --- | --- | --- |
| `id-token: write` | `.github/workflows/promote-baseline.yml:100` | Workflow grant retained for `promote`; guard overrides with `permissions: {}` at line 106 |
| `secrets.PINEFORGE_APP_ID` | `.github/workflows/promote-baseline.yml:252` | `promote`, org App token mint |
| `secrets.PINEFORGE_APP_PRIVATE_KEY` | `.github/workflows/promote-baseline.yml:253` | `promote`, org App token mint |
| GCP OIDC authentication | `.github/workflows/promote-baseline.yml:274` | `promote`, existing Google auth action |
| GCP provider and service account | `.github/workflows/promote-baseline.yml:276` and `.github/workflows/promote-baseline.yml:277` | Existing OIDC identity inputs |

The guard uses `ubuntu-latest`, matching `promote`, with `permissions: {}`,
no checkout, no `secrets.` access and no `id-token` use. Its only token
binding is `GH_TOKEN: ${{ github.token }}` for the metadata permission reads.
Its empty job-level permissions override the workflow's `contents: read`,
`statuses: read`, `checks: read` and `id-token: write`; those grants do not
apply to the guard. `promote` waits only on `[promote-guard]`
(`.github/workflows/promote-baseline.yml:162`) and its existing job condition.

On every PR close, merged or unmerged, the guard first refuses a different
head repository or a fork head, then checks the author. An author outside
`OWNER|MEMBER|COLLABORATOR` who is neither `dependabot[bot]` nor
`pineforge-app[bot]` must have `admin|maintain|write` permission from
`gh api "repos/$REPO/collaborators/$PR_AUTHOR/permission" --jq .permission`.
This admits private-member `CONTRIBUTOR` authors with write access and
refuses a non-writer, an empty permission or a failed call. The canon
comment and refusal wording are retained verbatim. Only after these checks
does an unmerged close pass without promotion. For a merged PR, an empty
merger login refuses before a merger lookup; the merger must be one of
those same bots or have
`admin|maintain|write` permission from
`gh api "repos/$REPO/collaborators/$MERGED_BY/permission" --jq .permission`.
Either lookup refuses on failure or an empty permission; failure after
printing any permission also refuses. The failed-call fallback appends
`none`, never a permission that can pass. Author and merger permissions are
independent: a writer author merged by a reader refuses.

Exact refusal message templates, as present in the workflow:

- `pull request from another repository (${HEAD_REPO:-unknown})`
- `pull request from a fork`
- `author ${PR_AUTHOR:-unknown} (${ASSOCIATION:-NONE}) has permission '$perm', not write, and is not an allowlisted bot`
- `pull_request_target merger is empty`
- `pull_request_target merger ${MERGED_BY:-unknown} has permission '$perm', not write`
- `event $EVENT does not run baseline promotion (only pull_request_target and workflow_dispatch are allowed)`

Each refusal emits `::error title=authz::<message>`, appends
`authz: REFUSED: <message>` to the step summary, and exits 1. These retain
the canon's error/summary prefixes even though the job is named
`promote-guard`. A failed guard prevents `promote` through its new `needs`
and the existing condition, which contains no status function.

Manual dispatches are accepted under the canon's write-access event rule.
An authorised same-repository unmerged close returns success without a
merger lookup, writes
`promote-guard: unmerged pull request; promotion is skipped`, and cannot
start `promote` because its existing merged-or-dispatch condition is retained.
An unmerged fork close fails red. All other events take the failing default arm.

### Existing merged and base-ref conditions: engine

The original `origin/main` workflow at
`.github/workflows/promote-baseline.yml:104` already required merged == true
for the PR path, with dispatch as the alternative. The line is unchanged
at `.github/workflows/promote-baseline.yml:163`:

```yaml
    if: ${{ github.event_name == 'workflow_dispatch' || github.event.pull_request.merged == true }}
```

Base ref `main` is NOT required today: the `pull_request_target` trigger
has no `branches:` filter, and neither this `if:` nor the guard checks
`github.event.pull_request.base.ref == 'main'`. Every executable base-ref
binding/use in the edited workflow is quoted below; `github.event.pull_request.base.ref`
has only the one binding.

| Edited file:line | Exact binding or use | Original `origin/main` line |
| --- | --- | --- |
| `.github/workflows/promote-baseline.yml:189` | `BASE_REF: ${{ github.event.pull_request.base.ref || 'main' }}` | 130 |
| `.github/workflows/promote-baseline.yml:193` | `git fetch --quiet origin "$BASE_REF" "+refs/pull/$PR_NUMBER/head:refs/remotes/origin/pr-head"` | 134 |
| `.github/workflows/promote-baseline.yml:194` | `if ! git merge-base --is-ancestor "$MERGE_SHA" "origin/$BASE_REF"; then` | 135 |
| `.github/workflows/promote-baseline.yml:195` | `echo "::notice::merge commit $MERGE_SHA is not on origin/$BASE_REF; skipping"` | 136 |

The workflow itself describes a merge commit "now on the base branch"
(`.github/workflows/promote-baseline.yml:9`) and compares against that
payload-selected branch. Its main-copy identity comments do not establish a
main-only PR base rule. No base-ref condition was added; a separate
main-only restriction needs a supervisor decision. The documentation now
states the existing base-branch check precisely.

### WIF second layer and promotion-account blast radius

Read-only sources: `git show origin/main:terraform/gcp/github-wif.tf` and
`git show origin/main:docs/VERIFY.md` in
`/Users/haoliangwen/code/pineforge-workflow`; that local `origin/main` is
`6d8a82791e87992b64ede831547f12fef66df76f`. No Terraform was edited.

The provider's `attribute_condition` at
`terraform/gcp/github-wif.tf:129` requires all five clauses:

- `assertion.repository_owner_id` equals `var.github_owner_id` (`terraform/gcp/github-wif.tf:130`).
- `assertion.repository_id` belongs to `values(var.github_promotion_repo_ids)` (`terraform/gcp/github-wif.tf:131`).
- `assertion.repository` belongs to `local.promotion_repo_names` (`terraform/gcp/github-wif.tf:132`).
- `assertion.ref == "refs/heads/main"` (`terraform/gcp/github-wif.tf:133`, resolving `local.promotion_ref` at line 81).
- `assertion.job_workflow_ref == assertion.repository + "/.github/workflows/promote-baseline.yml@refs/heads/main"` (`terraform/gcp/github-wif.tf:134`, resolving the path at line 69 and ref at line 81).

`docs/VERIFY.md:1180` and line 1181 state the exact ref/workflow pin;
lines 1182 and 1183 explain that `pull_request_target` runs the default
branch's workflow with `GITHUB_REF` set to that branch. Here that is main.
For ANY `pull_request_target` run of this workflow, including one for a fork
PR, the workflow runs main's own copy with `GITHUB_REF = refs/heads/main`;
its owner/repository claims identify the base repository. The provider
condition is therefore satisfied for these runs regardless of the PR head
repository. The guard is the only layer that distinguishes a fork PR.
The docs' sentence beginning "Fork PRs never could" at line 1187 concerns
workflow runs from a fork; it must not be treated as protection for a
base-repository `pull_request_target` run about a fork PR.

The promotion account's capabilities are quoted from
`docs/VERIFY.md:1178` and line 1179:

```text
`pineforge-promotion` service account (Cloud SQL client on the registry
instance, accessor on the registry-db secret, read on the evidence bucket).
```

Those privileges are the GCP blast radius: registry-instance connectivity,
access to the registry database credential, and evidence-bucket reads.
The GitHub App token remains scoped to reading the private
`pineforge-workflow` repo (`.github/workflows/promote-baseline.yml:255`,
line 256). The guard must succeed before either token minting or GCP
authentication is reachable.

### Tests written for pool execution: NOT RUN

`scripts/ci_preflight.py` now independently pins the guard's entire code
block, including all ten env bindings, empty permissions, runner, terminating
`fail`, author/head checks, author and merger lookups and default refusal. It pins the
two-job order, `promote` dependency and original job condition, and the exact
trigger set without weakening existing tree/status/runner checks.

`scripts/test_ci_preflight.py` adds must-fail mutations of the real workflow
text: whole guard script replaced by `exit 0`; no-op `fail`; unmerged exit
inserted before the head/author checks; each env binding removed, including `HEAD_IS_FORK`;
head/fork checks removed; author association bypassed; bot allowlist widened;
author lookup, empty-merger check or merger lookup removed; either
failed-lookup fallback changed to write; read permission accepted for either
actor; default refusal removed; extra allowed events; guard
permissions removed or OIDC granted; guard secret/checkout inserted; job/step
error suppression; guard skip; runner drift; `promote` dependency removed or
emptied or given an extra dependency; status-function bypass; missing, duplicate or reordered job contract;
and trigger drift. Each mutation must produce the corresponding preflight
finding. Existing hosted-route mutations now cover the guard as well.

The decision tests extract the `run: |` shell from the real workflow file
and execute it with bash. An executable fixture `gh` is first on PATH and
is the only fake: it logs/validates the API arguments and selects separate
permission and exit fixtures by username (`PR_AUTHOR` versus `MERGED_BY`).
Each decision row asserts the exact ordered API calls, including no merger
lookup after author refusal. No shell function or replacement guard driver
is prepended. They cover member/owner/collaborator authors, both bots,
`CONTRIBUTOR` authors with all three write-capable permissions, dispatch and
unmerged-close acceptance; foreign/fork/missing heads, authors without write
access, missing mergers, read/triage/none/empty permissions, failed author
and merger lookups (including partial output), a writer author merged by a
reader, and other events must refuse with the exact messages. These tests have been
written, not run. Missing bash fails explicitly with
`NOT RUN: bash is required to execute the promote-guard decision cases`;
it is never silently skipped. Tool availability here was NOT RUN.

Required decision rows implemented in the unittest tables (expected exits
are assertions, not execution results):

| Environment / PR case | Expected exit | Validation |
| --- | --- | --- |
| Fork head, unmerged | 1 | NOT RUN |
| Fork head, merged | 1 | NOT RUN |
| Same-repo head with `fork=true` | 1 | NOT RUN |
| Same-repo author `NONE`, not an allowlisted bot, author permission `none` | 1 | NOT RUN |
| Own merged PR, author `CONTRIBUTOR` with permission `write`, merger permission `write` | 0 | NOT RUN |
| Own merged PR, author `CONTRIBUTOR` with permission `read` | 1 | NOT RUN |
| Own merged PR, author `CONTRIBUTOR`, author lookup fails | 1 | NOT RUN |
| Own merged PR, author `CONTRIBUTOR`, author permission empty | 1 | NOT RUN |
| Own merged PR, author `CONTRIBUTOR` with permission `write`, merger permission `read` | 1 | NOT RUN |
| Own merged PR, member author, merger permission `write` | 0 | NOT RUN |
| Own merged PR, merger permission `read` | 1 | NOT RUN |
| Own merged PR, merger permission lookup fails | 1 | NOT RUN |
| Own merged PR, empty merger | 1 | NOT RUN |
| Own unmerged close, member author | 0 | NOT RUN |
| Own unmerged close, author `CONTRIBUTOR` with permission `write` | 0 | NOT RUN |
| Own PR by `dependabot[bot]`, merged by a write user | 0 | NOT RUN |
| `workflow_dispatch` | 0 | NOT RUN |
| `push` and other events | 1 | NOT RUN |

Additional execution-based mutation cases require the original script to
exit 1 and the changed script to exit 0 for a whole-script no-op, no-op
`fail`, missing fork check, missing head-repository check and an early
unmerged exit. A syntax error cannot count as the expected difference.
The structure pins separately refuse missing `needs`, guard OIDC,
secrets and checkout mutations, and every other listed contract mutation.
The two author-lookup mutations also have explicit decision differences:
deleting the author lookup changes the merged `CONTRIBUTOR`-with-write
case from exit 0 to exit 1, while changing only the failed author lookup's
fallback to `write` changes the failed-call case from exit 1 to exit 0.
Both mutations must also trigger the independent literal pin; NOT RUN.
No previous pin is loosened.

README.md and docs/ci.md document the authorization and hosted exception.
COMMIT-MSG-SEC846.md quotes every added or rewritten documentation sentence.

## NOT CHANGED

- The existing `pull_request_target: types: [closed]` and dispatch triggers, and the original `promote` merged-or-dispatch condition, are retained as required.
- Every existing `promote` step, secret reference, App token scope, OIDC grant/provider/account, environment and hosted runner is retained; only its guard dependency is added.
- Existing newest-status, merge-tree, campaign-verdict and promotion-defer behavior is retained; authorization now precedes it.
- No aggregate refuse-first step is added: `promote` has no status function that bypasses a failed dependency.
- Other engine workflows and all runtime, native, source-layer, ABI and corpus files are untouched; this brief assigns the engine promotion workflow only.
- Codegen, workflow-template and license cases B/C are outside this worktree and were not edited; the codegen-only `gate.yml` report item does not apply to this engine lane.
- Only the explicitly requested WIF Terraform and VERIFY sources were read from the workflow repo, via local `origin/main`; Terraform is untouched.
- The trigger still has no base-branch filter, and no base-ref condition was added; existing executable base-ref uses are quoted above.
- Existing CI runner fragments, hosted fallbacks and time limits remain pinned; changing the broader runner rollout is outside SEC846.
- Repository settings, real token metadata access, policy checks and hosted runs were not queried or executed in this offline edit-only lane.
- No file was deleted, no memory/instruction file was written, and no git staging, commit or push was performed.

## DECISION

- Use the explicitly authorized `promote-guard` canon variant because the canonical CI `authz` block deliberately rejects `pull_request_target`.
- Keep both jobs hosted on `ubuntu-latest`; the brief defers credential-workflow migration until `pf-ci-release`.
- Copy the 16:02 canon's nested PR-author case, private-membership comment, author lookup and refusal wording verbatim; `CONTRIBUTOR` authors with `admin|maintain|write` permission proceed, and failed or empty author lookups refuse.
- Apply this author rule to every closed PR before considering merged state, and independently require a write-capable or allowlisted `MERGED_BY` only for a merged PR.
- Preserve all other guard text, bot comments, error/summary prefixes, token binding, empty permissions and the `promote` dependency.
- Keep Addendum 2's order as retained by Addendum 4: head repository and fork flag, author including its permission fallback, authorised-unmerged exit, then nonempty and write-capable/allowlisted merger.
- Leave the base-ref behavior unchanged because the existing workflow uses the PR's base branch; flag a main-only restriction for the supervisor.
- Use an independent literal guard pin with actual-text mutations; no expected guard is derived from the workflow being checked.
- Use a PATH-first executable `gh` stub that answers by username with independent author and merger fixtures; assert exact ordered lookups and expected decisions for the real extracted script.
- Add both author-lookup must-differ mutations (lookup deletion and failed-call acceptance) without weakening existing pins or cases; all validation remains NOT RUN in this edit-only lane.

## NOT RUN

| Changed file | Validation |
| --- | --- |
| `.github/workflows/promote-baseline.yml` | NOT RUN |
| `scripts/ci_preflight.py` | NOT RUN |
| `scripts/test_ci_preflight.py` | NOT RUN |
| `README.md` | NOT RUN |
| `docs/ci.md` | NOT RUN |
| `COMMIT-MSG-SEC846.md` | NOT RUN |
| `REPORT-SEC846.md` | NOT RUN |

Preflight, the new mutation/decision tests, C++ unit tests, all verification
profiles, full corpus verification, `ci-policy-check.py` and the TypeSafe
diff guard and supervisor Codex review: NOT RUN. No PASS or runtime/parity verdict is claimed.
No execution summary or log path exists because no validation was run.
The supervisor must run the required verification and review before
committing or advancing the change.
