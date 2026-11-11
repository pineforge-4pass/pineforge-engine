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
