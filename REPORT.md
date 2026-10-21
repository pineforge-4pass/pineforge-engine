# core-cr150-o1v — lane V step 1 (CR-221 scope A): emit and read the scratch vocabulary certificate

> This path held lane F's report at commit `85f1d0118d` (the base of this
> branch); it is preserved there. The lane V brief names this root `REPORT.md`
> as this lane's handoff, so the file is replaced, not appended.

Status: AUTHORING ONLY. Nothing was compiled, linked, tested, run or staged on
this machine. Worktree `wt` (branch `cx/o1-v`, lane F `85f1d0118d` plus contract
stage 2.1 `f2c2fbb370`), codegen worktree `cg` (branch `cx/o1-v-cg`, main
`dbf50b89`). Deliverable is uncommitted.

This step classifies a compiled program before a run. It does NOT bind the
slab, does NOT route a run, changes no default, no refusal text and no hash
leaf, and adds no public ABI/export. `UNKNOWN = OUT`: any expression the
classifier does not recognise makes its roster incomplete.

## 1. Files changed in this worktree

| File:line | Change |
| --- | --- |
| `include/pineforge/detail/scratch_receipt.hpp:1` | NEW. Additive detail header. `scratch_receipt_detail::hex_digit:29` and `decode_hex:37` (strict lowercase hex, even length, no partial output); `scratch_facts_from_receipt:74` — `noexcept` pure function taking `(uint32 version, string_view classification, string_view certificate_hex)` and returning lane F's `ScratchFactsResult<ScratchVocabularyFacts>`. No public export, no new enum member, no allocation outside `std::string` block storage. |
| `tests/test_scratch_receipt.cpp:1` | NEW unit test (assert/throw style, `main` returns nonzero on failure). Hex strictness, every decoder error branch through the receipt, the in_subset-over-incomplete rejection, out_of_subset acceptance, three hand-computed cross-language vectors, hostile-input no-throw. |
| `tests/data/scratch_receipt_golden.json:1` | NEW. Three wire-v1 vectors typed by hand from `docs/design/scratch-facts.md:50-75` (including the doc's own worked vector). Each vector carries BOTH a hand-typed `layout` (per-byte blocks) and a separately retyped `certificate` hex; the tests assert they are the same string, so a transcription typo in either is caught. No implementation produced any byte. |
| `tests/cr150.d/core-cr150-o1v.cmake:1` | NEW. One-line-per-item registration of `test_scratch_receipt`. |

Nothing else in the engine was touched: `scratch_facts.hpp`, `scratch_tables.hpp`,
the sources, the defaults and every frozen pin are untouched. No consumer
includes the new header yet; lane H owns reading it and binding the plan.

## 2. What the receipt reader does

`scratch_facts_from_receipt` performs exactly three steps and publishes
nothing on failure:

1. `version != kScratchFactsReceiptVersion (1)` -> `unsupported_version`.
2. Classification must be exactly `"in_subset"` or `"out_of_subset"`; any
   other label -> `invalid_complete_flag` (the label is the receipt's own
   completeness flag).
3. Strict hex decode: even length and `[0-9a-f]` only (uppercase is refused so
   one byte string never has two spellings); a violation -> `invalid_length`.
   Then `decode_scratch_facts` (`scratch_facts.hpp:385`) runs unchanged.
   `"out_of_subset"` is accepted over any certificate, complete or not.
   `"in_subset"` is accepted only if the DECODED facts prove all five rosters
   complete; otherwise -> `incomplete_vocabulary`. The label is never trusted.

Error mapping uses lane F's closed enum and does not extend it (the mapping is
documented in the header at `:66-72`). Hex defects have no dedicated F member;
they map to `invalid_length`, the wire's no-partial-output length contract.
`allocation_failure` is only reachable from a real allocation failure, not
from hostile input: the count minimum-length check rejects a huge count before
any reserve (`scratch_facts.hpp:326-331`). No exception escapes: the function is
`noexcept` and catches `bad_alloc` / `length_error`, matching lane F.

## 3. Codegen emission (see `cg/REPORT-V.md` for the codegen detail)

The codegen worktree adds one additive key `scratch_facts` to the existing
`strategy_order_shapes_receipt` JSON document
(`pineforge_codegen/codegen/order_shapes.py:350-359`). Its fields are
`{"version":1,"classification":"in_subset"|"out_of_subset","certificate":"<lowercase hex>","reasons":[...]}`.
The certificate is lane F's wire v1 (`scratch_facts.hpp:286-385`): version byte
1, then S,F,E,H,J, each as a complete byte, a u64 LE count, then per entry a u64
LE length and the raw UTF-8 bytes of the Pine string, sorted by unsigned byte
value and deduplicated. Nothing about `LiveBounds` is emitted: roots stay on the
exact history until lane R proves live bounds.

### api_version finding

`docs/PUBLIC_CONTRACT.md:67-95` and `CHANGELOG.md:544-575` fix
`strategy_order_shapes_api_version()` at 1 and enumerate the document's keys.
Neither says that an additive field requires a version bump, and the one
"no new fields are added" sentence (`docs/PUBLIC_CONTRACT.md:63`) is about the
capabilities receipt, not order shapes. `strategy_order_shapes_api_version()`
therefore stays 1; the change is additive and a reader that does not know the
field ignores it. The codegen test `tests/test_order_shapes.py:25-26` pinned
the exact key set, so that single assertion was updated to include
`scratch_facts`; no other pinned document was touched (`documents[:2]` in
`test_legacy_receipts_are_byte_identical` are the legacy receipts and are
unchanged). `docs/PUBLIC_CONTRACT.md:96-107` now documents the field.

### Closure rule as implemented (codegen, `scratch_facts.py`)

Rosters, from the analyzed AST (`emitter.ctx.ast`):

* **S** — source IDs: `strategy.entry`, `strategy.order`, `strategy.exit`,
  `strategy.close` and `strategy.cancel` `id` values (openings carry the entry
  ID and exit/close placements carry their own ID, `pine_adapter.cpp:10413`,
  `:12002`), plus the engine-created label families: the constants
  `__margin_call__`, `__close__`, `__pine_close_all`, `__intraday_loss__`,
  `__intraday_cap_close__`, and `__margin_preopen__` + entry id and
  `__close__` + (entry or exit id).
* **F** — `from_entry` of every `strategy.exit` (an omitted argument is the
  emitter's literal `""`), plus, whenever any placement is possible, the empty
  string and every known entry and exit ID: a close placement copies its own
  ID into `from_entry` on the non-default-FIFO path (`pine_adapter.cpp:10413-10415`)
  and a preopen margin slice copies the opening source ID (`:5894-5906`), so
  F's completeness needs both the entry and the exit rosters.
* **E** — `id` of every `strategy.exit`, `strategy.close`, `strategy.cancel`.
* **H** — `S ∪ F`: the only values the adapter passes to `cohort_for`
  (`src/source/pine_adapter.cpp:5035`, `:5899`, `:6660`, `:6675`, `:12478`).
* **J** — the entry/order IDs: `note_exit_rank_key` records only opening
  Entry/Order source IDs (`pine_adapter.cpp:24568`).

Expressions handled, per node kind (`classify`, `scratch_facts.py:257`):

| Node | Verdict |
| --- | --- |
| `StringLiteral` | complete, its value (byte-faithful UTF-8) |
| `Ternary` | union of both branches; complete iff both are |
| `SwitchStmt` used as a value | union of every arm's last expression and the default; complete iff all are |
| `BinOp` `+` with two complete operands | complete, the concatenated values |
| `BinOp` with an incomplete operand or another operator | OUT (`string_concat` / `non_string_expression`) |
| `Identifier` | scope-matched program-wide definitions unioned, including non-`var` and `var` reassignments in if/else branches; a loop variable, a self-reference, a series name or no definition is OUT |
| `input.string` (and `input.text_area`, `input.session`, `input.symbol`, `input.timeframe`, `input.color`, generic `input`) with a `StringLiteral` default | complete, the declared default recorded (see the open question in §7) |
| user function with a single-expression body | evaluated with the union of every call site's arguments; a multi-statement body, a recursive call or a call deeper than 6 levels is OUT |
| `NumberLiteral`/`BoolLiteral`/`ColorLiteral`/`NaLiteral` | OUT (`non_string_literal`) |
| `Subscript` | OUT (`subscript`) |
| `MemberAccess` | OUT (`member_access`) |
| `str.tostring(...)` | OUT (`str_tostring` plus the argument's reasons) |
| `array.get(...)` | OUT (`array_read` plus argument reasons) |
| `request.security` / `request.security_lower_tf` | OUT (`request_security`) |
| any other namespace/method call | OUT (`method_call`) |
| any other name-bound call | OUT (`unrecognized_call`) |
| any other node kind | OUT (`unrecognized_expression`) — UNKNOWN IS OUT |

Reason codes (short, stable, pinned by tests): `series_value`, `loop_variable`,
`unbound_identifier`, `self_reference`, `unresolved_definition`,
`str_tostring`, `string_concat`, `non_string_expression`, `non_string_literal`,
`subscript`, `member_access`, `array_read`, `request_security`, `method_call`,
`user_function_nonconstant`, `recursive_call`, `call_depth`,
`missing_argument`, `input_default`, `order_call_in_method`,
`order_call_in_uncalled_function`, `unrecognized_call`,
`unrecognized_expression`.

An order call inside a `MethodDef` is OUT (`order_call_in_method`); inside a
`FuncDef` with no recognised call site it is OUT
(`order_call_in_uncalled_function`), because an unseen call (method syntax or a
library boundary) must not be mistaken for dead code. Definitions are collected
program-wide but filtered to the evaluating scope, so a local from another
function can never supply a value.

### Deliberate over-approximations (safe direction, reported)

1. The engine creates more internal source labels than the two the V brief
   names. Evidence: `__close__` + id (`pine_adapter.cpp:9431`, `:9483`,
   `:10247`, `:10320`, `:10645`, `:14048`), `__pine_close_all` (`:9783`,
   `:9791`), `__intraday_loss__` (`:21007`, `:25935`),
   `__intraday_cap_close__` (`:21047`, `:21067`). S includes all of them plus
   the two named families, so S is a superset of every
   `PlacementSnapshot.source_id`. Over-approximation can only enlarge the slab.
2. F contains `""` and every known entry/exit ID whenever any placement is
   possible, even where a particular configuration cannot reach it.
3. `E` includes `strategy.cancel` IDs, which create no placement.
4. S includes every exit/close ID and every internal label family, though some
   of them cannot flow to a given table.

## 4. Tests and why each fails before

Engine (`tests/cr150.d/core-cr150-o1v.cmake`):

| Test | Why it fails before this change |
| --- | --- |
| `test_scratch_receipt` | `include/pineforge/detail/scratch_receipt.hpp` does not exist on the frozen base, so the TU fails at the first include (missing-header diagnostic); the file records that as its first-line note. `tests/data/scratch_receipt_golden.json` also does not exist. |

Bodies: `strict_hex_and_labels` (version, label set, odd/uppercase/non-hex,
even-length hex of a truncated wire); `every_decoder_error_through_the_receipt`
(unsupported wire version, invalid complete flag, trailing bytes, arithmetic
overflow, short count, over-long entry, duplicate, unsorted, every truncated
prefix); `the_label_is_never_trusted` (in_subset over an incomplete roster ->
`incomplete_vocabulary`; out_of_subset accepted and keeps the incomplete
facts); `golden_vectors` (layout == certificate == decoded sets for three
hand-typed certificates); `hostile_input_never_throws` (4096-byte garbage, odd
and even, embedded NUL, non-ASCII; `static_assert` on `noexcept`).

Codegen (pytest, the existing `tests/` pattern, no CMake registration there):

| Test | Why it fails before this change |
| --- | --- |
| `tests/test_scratch_facts.py` truth table (34 Pine programs) | `scratch_facts` is absent from the receipt document -> `KeyError`, and the classifier module does not exist -> import error. Every expected classification and reason is hand-written in the table with a one-line judgement; nothing is copied from emitted output. |
| `tests/test_scratch_facts.py` mutants | The classifier module does not exist. Each mutant replaces the id node of an IN program with one OUT node kind (or a new unknown `ASTNode` subclass) and requires OUT, plus loop-variable, unknown-node-in-`from_entry` and recursive-function mutants. |
| `tests/test_scratch_facts.py` determinism + encoder order | Module absent. Same program twice, reversed input declaration order, shuffled/duplicated roster order and Python `set` iteration all yield one certificate. |
| `tests/test_scratch_facts.py` golden | `tests/data/scratch_receipt_golden.json` is new; the encoder must reproduce the hand-typed hex. |
| `tests/test_scratch_facts_corpus.py` corpus census | Module and field absent. Runs the real emitter over every `corpus/validation/*/strategy.pine`, requires every probe the static census lists as literal vocabulary to be `in_subset`, requires a reason code on every OUT, and prints the OUT probe names. It asserts no hard-coded count; the population is the corpus tree. The corpus is a sibling checkout, so the module skips without it (same resolver as `tests/test_compile_corpus.py`). |

## 5. Assumptions on the contract (re-check after merge)

| Assumed | Where |
| --- | --- |
| `ScratchFactsResult`, `ScratchFactsError`, `ScratchVocabularyFacts::complete()`, `decode_scratch_facts`, `kScratchFactsWireVersion` are exactly as read | `include/pineforge/detail/scratch_facts.hpp:20-42`, `:71-97`, `:292`, `:385` |
| The wire layout is version byte then S,F,E,H,J with complete/count/length bytes | `docs/design/scratch-facts.md:50-62`, `scratch_facts.hpp:286-291` |
| `emitter.ctx.ast` is the analyzed AST at receipt-emission time and the receipt is emitted once per generated strategy | `pineforge_codegen/codegen/order_shapes.py:304`, `capabilities.py:310-318` |
| The codegen's `_expression` and signature registry are unchanged | `capabilities.py:65-80`, `signatures.py` |

## 6. Compile audit (read, not run)

| File | Access/const | Signature/includes | Inline-ODR | -Werror risk |
| --- | --- | --- | --- | --- |
| `include/pineforge/detail/scratch_receipt.hpp` | free inline functions in `pineforge::detail`; no globals, no statics | includes `scratch_facts.hpp`, `<cstddef>`, `<cstdint>`, `<new>`, `<optional>`, `<stdexcept>`, `<string>`, `<string_view>`; all used | all functions `inline` (header-safe, one definition) | no narrowing, no unused parameters; `int` from a `char` comparison; `static_cast<char>` on a 0-255 value |
| `tests/test_scratch_receipt.cpp` | anonymous namespace; no external symbols | includes the new header, `../runner/json.hpp` (self-contained, std only), standard headers | one TU | assert-style; helper names (`append_word`, `require`) are in the anonymous namespace and cannot clash with `scratch_facts_detail::append_word` |
| `tests/cr150.d/core-cr150-o1v.cmake` | one `list(APPEND)` + one `pineforge_add_unit_test` | — | — | matches `tests/cr150.d/README.md` plain-registration form |
| codegen `scratch_facts.py` | module-level constants only; frozen dataclass results | imports `ast_nodes`, `limits.syntax_children`, `signatures`, `capabilities._expression`; no cycle (capabilities imports order_shapes lazily) | n/a | no unused imports (checked by reading) |

Debug/Release/historical-header configurations: the header uses C++17 only, no
`<bit>`, no `bit_cast`, no exceptions in its own body (`noexcept`, two catches),
and the test includes only public/detail headers. The lab still has to compile
both configurations and every historical ABI provider.

## 7. Open questions / deviations (reported, not decided here)

1. **`input.string` and trial overrides.** The V brief lists `input.string` as
   an IN form and requires recording the default. The generated strategy can
   still be overridden at run time (`strategy_set_input`,
   `pineforge_codegen/codegen/emit_top.py:1994-1996`), and the emitted
   certificate is static. As implemented, the declared default is recorded and
   the roster is marked complete; the label is therefore valid only when the
   trial's effective inputs equal their defaults. This step does not bind or
   check effective inputs (that is lane H, which already owns effective-trial
   setup). **Lane H must refuse to honour `in_subset` when an effective input
   differs from the declared default**, or this becomes the reverse error the
   V brief calls impossible. Flagged for the supervisor.
2. Extra internal labels in S (§3, deliberate over-approximation).
3. `E` includes cancel IDs and F always carries `""`; both are conservative.
4. The classifier is stricter than the closure rule for multi-statement user
   functions (always OUT) and for `str.tostring` of a numeric constant (OUT).
   No Pine program in the corpus relies on either.
5. Lane F has no defect found. The decoder's strictness, formulas and wire
   bytes were read and used unchanged.

## 8. UNVERIFIED (nothing was run)

No compile, link, ctest, pytest, corpus sweep, preflight, ci_verify, sanitizer,
Debug/Release build, heap census, ABI receipt preparation, or golden
reproduction was performed on this machine (the lane forbids it). Specifically
unverified: the codegen classifier's behaviour on the real corpus (predicted
all IN from a static read of every order call in the 312 probes — all ids and
from_entry values are string literals); the Python emitter still passing
`tests/test_order_shapes.py` and `tests/test_strategy_capabilities.py`; the
engine test compiling and every expected error code; both hand-typed golden
transcriptions (layout vs certificate, Python vs C++). These are the first
things the lab four-step and the paired codegen job must report.
