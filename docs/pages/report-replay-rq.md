# Settlement and equity replay records {#report_replay_rq}

This opt-in extension retains the original engine operands needed to replay
realized P&L and reported equity as binary64 operations. It supplements the
[report schema](report-schema.md) within the existing
[ABI guarantee](abi-stability.md).

`--report-replay` requests annotated
JSON; `--report-replay-sink PATH` explicitly negotiates a separate
`pf-report-replay/v1` companion. S carries `initial_capital`, `pointvalue` and
the raw `net_profit_sum`. Existing primary `pf-report-sink/v1` E/T records,
header, receipt, counts and bytes keep their current meanings. No existing
C report, trade or equity-point allocation grows; ABI version remains 5,
unchanged by this extension.

## R records and JSON trade members

There is exactly one R for each final primary T, including report-only
terminal trades. Positions include the tag at position 0.

| Position | Member | Meaning |
| --- | --- | --- |
| R1 | `n` | Existing one-based final T number. |
| R2 | `settlement_index` | Zero-based physical-close addition index; null only for a terminal row. Slices of one source share the index. |
| R3 | `parent_id` | Canonical decimal **string** of the full uint64 applied-event ordinal, scoped to this run. |
| R4 | `slice_index` | Zero-based replacement emission index, retained through later permutations. |
| R5 | `slice_count` | Total replacement rows for that parent, independent of the original close count. |
| R6 | `recut_fields` | Exactly `[]` for ANY; exactly `["qty","commission","pnl","pnl_pct","max_runup","max_drawdown"]` for FIFO. |
| R7 | `child_index` | Original child/transaction roster index. |
| R8 | `transaction_qty` | Whole child transaction quantity, before slicing. |
| R9 | `whole_qty` | Original kernel close quantity. |
| R10 | `whole_pnl` | Original net P&L booked once into the kernel accumulator. |
| R11 | `whole_commission` | Original combined entry and exit commission. |
| R12 | `whole_max_runup` | Original favorable excursion operand. |
| R13 | `whole_max_drawdown` | Original adverse excursion operand. |
| R14 | `whole_exit_fee` | Original exit-fee quote; null when fees are disabled or the branch is ANY. |
| R15 | `transaction_fee` | Rounded whole-child exit-fee quote; same applicability as R14. |

Under the option, each JSON trade has `settlement_index` and `recut`.
An unamended trade has `recut: null` and R3–R15 all null. A terminal row
additionally has R2 null. A replaced row has a `recut` object containing all
R3–R15 member names above.

**Money recut iff `recut_fields` is non-empty.** ANY still requires a complete
marker: it copies source quantities, money, percentages and excursions
bit for bit while changing row order. Empty fields do not mean an unamended
row. FIFO retains the six-field list even when recomputation happens to
preserve some numerical values.

Each parent's indices cover `0..slice_count-1` exactly once. All slices with
the same R2 agree on their original whole operands. The marker belongs to
the row: assign it before amendment publication, move it with the row through
`sort_same_bar_exit_trades`, and write it before the ledger seals or retires
its resident prefix. Later reads preserve the same identity and operands.

Indices and counts use checked exact JSON integers, at most `2**53-1`.
The parent uses a string even above that bound, up to `"18446744073709551615"`.
Applicable monetary operands are finite binary64 values with round-trip
representations; null means inapplicable.

For FIFO replay, walk children in slice emission order and consume source
lots in settlement order. Take `q=min(left,wanted)`, then apply the existing
ten-significant-digit money function R to `left-q` and `wanted-q`.
With `share=q/whole_qty`, enabled fees use
`a=R(max(0,whole_commission-whole_exit_fee)*share)`,
`b=R(transaction_fee*q/transaction_qty)` and `fee=R(a+b)`.
Disabled fees use the producer's explicit `+0.0`.
Then `pnl=R((whole_pnl+whole_commission)*share)-fee`,
`cost=(entry_price*q)*pointvalue`, and
`pnl_pct=(cost>0 ? (pnl/cost)*100 : 0)`. Multiply each whole excursion by
share. Preserve each operation and rounding boundary; the supplied operands
avoid guessing historical fees or recovering pre-rounding values.

## Q records and equity-point members

Q has positions `["Q",point_index,time_ms,settlement_count,net_profit_sum,
equity_basis,equity_term]`. Q1 is the zero-based final E ordinal and Q2 equals
that E's time. There is one Q per E, including a selected anchor. Join by
point index; the anchor and first observation can share a timestamp.

The four JSON point members are:

| Member | Meaning |
| --- | --- |
| `settlement_count` / Q3 | Physical-close prefix included in this snapshot; null for the synthetic anchor. |
| `net_profit_sum` / Q4 | Historical raw accumulator for that prefix; null only for the anchor. |
| `equity_basis` / Q5 | Closed set: `kernel_open`, `range_end`, `terminal_quote`, `anchor`. |
| `equity_term` / Q6 | Actual finite operand before adding capital, preserved even when displayed `open_profit` is zeroed. |

For effective capital C and the prefix N, the original operation order is:

| Basis | Exact equity expression |
| --- | --- |
| `kernel_open` | `(C + N) + equity_term`, where the term is the gross open mark. |
| `range_end` | `(C + N) + equity_term`, where the term is the separately folded report-only terminal P&L. |
| `terminal_quote` | `C + equity_term`, where the term is the presenter's report-row net P&L. |
| `anchor` | `C`; Q3 and Q4 are null and Q6 is explicit `+0.0`. |

The anchor uses the selected effective capital, rather than inventing a
historical engine observation. A zero-trade run still has genuine `+0.0` S3
and `+0.0` at every non-anchor Q4. An open terminal position can make displayed
equity differ from `C + N`; subtracting capital from that equity loses the
operand this extension is intended to retain.

## Exact replay and availability

Require the distinct non-null R2 values to be exactly `0..M-1`. For each j,
take P[j] from R10 for a marked row or from the original unamended row's P&L.
Validate agreement among duplicate source slices and use each source once.
Start N[0] at `+0.0`, then perform one binary64 addition in increasing j:
`N[j+1] = N[j] + P[j]`. Compare the 64 bits of N[M] to S3 and of N[Q3] to
every non-null Q4. There is no tolerance, regrouping, compensation,
presented-slice fold or subtraction from equity.

Compare parsed engine/companion values before any canonical re-encoding.
Signed zero is significant: S3 or non-null Q4 with `-0.0` is invalid, while
an individual P[j] may be `-0.0`. The zero seed is always positive.

The companion retains the S lane's header/trailer framing and adds fixed R/Q
descriptors plus R/Q counts. Hash header and data lines including LF,
excluding the trailer; verify counts, digest, trailer and EOF. Its
`base_sha256` must equal the verified primary's digest whenever a primary
sink is present. Standalone direct JSON/C capture may omit that binding.

**For sealed or streamed runs, the companion is mandatory for exact replay.**
Read R from that companion after the run. Missing companion, a missing R for
any T, unequal Q/E coverage, incomplete receipt or wrong primary binding
makes exact replay **UNAVAILABLE**. A validator reports that verdict and
does not fall back to a row fold, an after-run ledger read or a tolerance.

`pf_sink.ReplaySinkReader(path, primary_path=primary_path)` validates the
companion and its association with a verified primary. Exhaust the iterator
before trusting its `complete`, `trailer` or `sha256`; yielded records remain
provisional until then. An unbound reader checks format and receipt only.
`ReplaySinkFormatError.code` is
`r_record_invalid` or `q_record_invalid` for malformed unbound records.
Bound failures use `exact_replay_unavailable` with a structured
`code_args["reason"]`: missing companion/trailer, count/hash/binding failure,
trailing bytes, invalid R/Q/settlement/parent coverage, disagreeing source
operands, or a replay mismatch. S format errors use `s_record_invalid`,
`missing_s`, `duplicate_s` and `trailer_out_of_order`. The R6 descriptor type
is `str[]?`; the reader accepts only the two specified field lists.

Native collection uses fixed 64 KiB scratch windows over checked disk/arena
storage. Recording and emission take O(T+Q) work and disk for T final rows
and Q points, in addition to the existing recut/sort algorithm. Sealing writes
R with T before retiring its annotations; released E points consume their Q.
Selected capture retains only selected provenance and its anchor. Bound
Python validation uses temporary SQLite disk tables and bounded caches,
with O((T+Q) log(T+Q)) index work. It stores original engine lines and bit
blobs, comparing parsed doubles before any monetary re-encoding. The OFF path
allocates no replay owner and maintains no settlement counter or point history.

Host-owned custom curves without recorded operand provenance and summary
recording modes are refused. A presenter changing the equity expression must
retain its actual operands; an unmatched expression receives
`report_shape_unsupported`, never reconstructed operands.
For an fd primary, the producer checks the companion receipt and binding to
the native primary receipt. The receiver must materialize and verify both
streams with the bound reader before asserting exact replay.

The two-build byte check and fixture setup are described in
[CI and interface notes](../replay-ci-README.md).
