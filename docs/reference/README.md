# Reference text moved out of CLAUDE.md and AGENTS.md

`CLAUDE.md` and `AGENTS.md` are short on purpose: how to build, how to test, and the rules that matter. The longer text that is not testing guidance lives here. Each file below is the exact text of the line range shown, copied unchanged (no header added) from the two files as they stood at commit `4c9e7633`.

| File | Moved from | What it holds |
| --- | --- | --- |
| [`c-abi-export-sop.md`](c-abi-export-sop.md) | `CLAUDE.md` lines 76-99 and `AGENTS.md` lines 73-96 (identical) | checklist for adding or removing a runtime `PF_API` export (the `source-guard-c-abi` gate) |
| [`refactor-workers.md`](refactor-workers.md) | `CLAUDE.md` lines 108-143 and `AGENTS.md` lines 105-140 (identical) | rules for an agent implementing a slice of the native-engine refactor |
| [`ci-runner-routing.md`](ci-runner-routing.md) | `AGENTS.md` lines 9-19 | merge ruleset summary and CI runner routing as written then |
| [`merge-gate-and-promotion.md`](merge-gate-and-promotion.md) | `AGENTS.md` lines 142-160 | merge gate, parity campaign and baseline promotion as written then |

These files are a record, not a second source of truth. Where one disagrees with `CLAUDE.md`, the workflow files under `.github/workflows/` or [`docs/ci.md`](../ci.md), those win. For example `ci-runner-routing.md` still describes the PR build as `ci.yml` and a macOS post-merge cell, while the pull-request proof jobs now live in `gke-pr.yml` and `ci-post.yml` has no macOS cell.
