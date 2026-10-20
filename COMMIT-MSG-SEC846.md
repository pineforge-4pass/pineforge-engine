SEC846: require head and actor authorization before baseline promotion

PARKED 10-20 17:35 (owner #859): CI security work resumes 10-23 10:00;
committed as-is, no PR. Review read 12 returned CHANGES NEEDED (P2 guard
text differs across engine, codegen and the template; P3 handoff quotes
omitted, template edits beyond scope) — open, not fixed.

Put a credential-free, hosted promote-guard before promote with empty job
permissions. On every pull_request_target close, check this repository's
non-fork head and the canonical author association/bot/permission rule before
the unmerged exit. Copy the canon's author fallback and comment verbatim:
private-member CONTRIBUTOR authors pass with admin/maintain/write permission
from GitHub; failed or empty author lookups refuse. Fork closes refuse even
when unmerged. Merged own PRs also
require a nonempty merger and an allowlisted bot merger or admin/maintain/write
permission from GitHub; failed or empty lookups refuse. Authorised own
unmerged closes skip promotion through the existing job condition.
Manual dispatches remain allowed; every other event refuses.

Pin the complete guard independently in ci_preflight and keep every existing
promotion pin. Add mutations of the real workflow for a no-op fail, missing
head/fork bindings and checks, bypassed author checks, a widened bot list,
missing/failed author and merger lookups, missing needs, status-function bypasses,
guard checkout/secrets/OIDC, permissions and runner drift. Add whole-script
no-op and early-unmerged-exit mutations. Decision tables execute the actual
workflow shell with bash and an executable gh stub first on PATH; the stub
answers by username with separate author and merger permission/exit fixtures.
The table requires a merged CONTRIBUTOR-with-write PR to pass, read/none/empty
or failed author lookups to refuse, and a writer author merged by a reader
to refuse. Deleting the author lookup changes the writer-author case from
0 to 1; making a failed author lookup pass changes its refusal from 1 to 0.
Disabled refusal mutations must differ from the original execution.

Document that merged == true is already required for PR promotion but the
trigger and guard have no main-base condition; leave the base-ref behavior
unchanged. Record the WIF source conditions and promotion-account blast
radius in REPORT-SEC846.md: WIF admits base-repo pull_request_target runs
for fork PRs, so promote-guard supplies that distinction.

Validation: NOT RUN for every changed file; this is an edit-only lane.
The supervisor must execute verification before any passing claim.
No staging, commit or push was performed.

Documentation sentences added or rewritten, quoted verbatim (each six or
more words):

README.md:

```text
`promote-guard` runs before promotion credentials are available and rejects foreign or fork heads and disallowed authors on every PR close, including unmerged closes.
Authors outside the association and bot allowlists must have `admin`, `maintain` or `write` permission from GitHub; failed or empty author lookups refuse promotion.
Merged PRs also require an allowlisted bot merger or a merger with write access; manual dispatches are allowed.
See [CI documentation](docs/ci.md) for author associations and permissions, merger permissions and unmerged-close behavior.
```

docs/ci.md, baseline eligibility sentences:

```text
It requires both statuses to be
successful on the verified PR head and a merge commit on the PR's base branch that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on the base branch).

It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; after the guard succeeds, ineligible baseline cases exit green
without promoting.
```

docs/ci.md, promotion authorization:

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

docs/ci.md, hosted routing:

```text
Promotion and its `promote-guard` stay hosted for both target and dispatch triggers.
```

docs/ci.md, preflight pins:

```text
The same stage pins `promote-guard` independently, its empty permissions, its payload bindings and refusal script, and the merged-or-dispatch credential job's dependency on it.
Decision tests execute the guard script extracted from the real workflow with bash and an executable `gh` stub first on PATH that returns separate author and merger permissions by username.
Mutation tests alter the real promotion workflow to replace the script with `exit 0`, move the unmerged exit before authorization, remove author or merger lookups and other checks, disable `fail`, bypass failed author or merger lookups, add guard credentials or a checkout, or disconnect the credential job.
```
