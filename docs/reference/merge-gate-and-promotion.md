## Merge gate and parity campaign

The `PineForge strict CI base` ruleset requires `pineforge/verify` (full
`ci_verify.py` profiles on the PR head's exact tree) and `pineforge/parity`
(no parity regression, or no engine behaviour change). The maintainers'
private verification tooling posts both commit statuses; GitHub Actions does not post them.
Its PR jobs provide faster advisory feedback and a documentation-only change runs preflight alone among verification jobs; otherwise push-to-main and manual
dispatch run the full CI profiles. A campaign PASS verdict still binds the
exact engine and codegen HEADs for baseline promotion.

Merged single-axis PRs advance the campaign baseline automatically through
`.github/workflows/promote-baseline.yml`, installed from the private
pineforge-workflow repository's `campaign/ci/promote-baseline.yml`. `main`'s own copy runs
on a `push` to `main` (it resolves the merged PR from the pushed commit) and runs no PR code. It promotes only
when the merge commit on `main` carries the gated PR head's tree (PRs are
squash-merged, so the head itself never lands on `main`), both required
statuses are success on that head, and the maintainers' private campaign tool finds the merge gate's
parity verdict for that tree; it then adopts the pair that verdict measured.
Anything else exits green without promoting.

## Release admission and retained evidence

Release admission requires authenticated `pineforge/parity=success`.
`pineforge/verify` stays advisory: its existing producer still runs, and its
actual latest state and presence are printed and recorded. The profiles run
by ci-post are execution evidence, separate from that status. The future
main-proof context map supplies verify on that path; no entry means
`presence=false`, `state=missing`, never an inferred success.

Phase `bump` gates current main and pushes only the untagged VERSION/catalog
reset. After that bump's own full ci-post, phase `tag` names its sole checked
parent with `dispatch_sha` and tags the same bump SHA. Both PR and future
main-proof admission refuse a re-bump. The original push actor must be the
release App; a maintainer's rerun requires authenticated repository write
permission. Every attempt remains visible, including failed and cancelled
attempts and incomplete history, which refuses admission.

The complete default latest jobs response for the selected exact-SHA run
is the proving inventory; attempt-specific inventories are audit history.
Any red proving job, duplicate proving name or missing required success
refuses. Changed IDs/API attempt numbers do not establish fresh execution.
Docs-only releases walk back with the shared `ci_docs_only.is_documentation`
classifier to the actual built proof; each docs commit needs its own latest
successful run. Requested release and proof SHA/tree identities and the docs
chain stay distinct. VERSION bumps need their own full CI.

The optional gate receipt uses the decision's collected evidence, including
actual advisory status, App attribution, proving jobs, all attempts and limits.
Refusal remains nonzero and receipt-write failure fails the run.
The workflow always retains that receipt for both phases and dry runs;
it introduces no status producer or main-proof issuer. See
[`docs/ci.md`](../ci.md#release-guard-phases) for limits and artifact details.
