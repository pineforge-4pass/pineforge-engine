# core-cr150-o1e2

## Fix round 2

Updated the citation in `docs/native-refactor-progress.md:183`.

- Old cite: `include/pineforge/native_order.hpp:2302`, claiming the removed `history_` member.
- New cite: `include/pineforge/native_order.hpp:2438`, the declaration of `journal_`.
- New header line, quoted: `    std::shared_ptr<JournalStore> journal_;`

The same sentence now names `journal_` and describes `history()` as returning a
journal view, matching the current declarations.

No other doc anchor changed.

Only files under `docs/` were edited. This report is under `docs/` to honor that
restriction. No local builds, tests, corpus runs, or measurements were run.
Source inspection and `git diff --check` were the only verification; the latter
passed. No staging or commits were performed.
