# hl2 history scratch guard

The files `generated.cpp` and `strategy.pine` are verbatim copies of
`validation/input-source-subscript-hl2-01/` from corpus commit
`8425ca820c4b7272db24d659a64d88e8e05b6407`.

- `generated.cpp` (37,386 bytes)

  sha256: b55054feb5f9be30e65206783112385d074b0836738af1056717b1ed7a0235f8

- `strategy.pine` (1,295 bytes)

  sha256: 5479d3e6b662e0305cf8328efa92b0ab3ded180c62e4d42b3d50fc8b5452f770

They are committed here for the CI scratch syscall guard: the PR Release
leg checks out no submodules and has no LFS market feed. The test must
compile and execute this probe without a corpus checkout or a skip path.
The `strategy.pine` Apache-2.0 header is kept unchanged.

This is a synthetic probe script in CI: the original hl2 momentum-turn
strategy runs on 100,000 deterministic synthetic 15-minute bars, with
empty inputs and default strategy settings and budgets. The bar generator
uses fixed-seed SplitMix64 shocks paired with their opposites, mild drift,
and symmetric high/low wicks. This guarantees frequent hl2 momentum turns;
the test still requires at least 30,000 actual closed trades from the
public ABI report, excluding report-only range-end marks.

The forwarding ScratchFile control is measured separately. The zero-I/O
window covers the entire generated C ABI run call, including its report
fill. All syscall counters are printed on success and failure. The test
does not read this README or depend on its checksums.
