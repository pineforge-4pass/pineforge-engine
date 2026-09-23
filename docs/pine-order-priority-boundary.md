# Explicit Pine execution attachment and order priority

`BacktestEngine::attach_pine_execution_adapter()` explicitly selects two
compatibility components: the existing intraday cap and retained-parent order
priority. It is idempotent, preserves configuration and outstanding cap state,
and does not imply that all Pine behavior has been extracted from the engine.
`enable_pine_intraday_cap()`, the cap constructor argument and legacy integer
cap assignments remain cap-only operations.

Bare construction leaves order priority detached. Syminfo metadata, including
`flat_retained_child_fresh_parent_order=1`, cannot attach it. The Pine component
owns that key's finite-positive interpretation and retains a supplied value
while detached; explicit attachment preserves a prior off override. Under Pine
attachment, an absent key enables the exact historical exception. Zero, negative
and nonfinite supplied values disable it. Copy/reset preserve attachment and
configuration without retaining a cached scheduling decision.

The full predicate lives in `src/compat/pine/order_priority.cpp`: exact two-object
book, ordinary POOC context, flat placement, named-cancel and exact child-reissue
receipts, adjacent incarnations, default quantities, full absolute stop+limit
child, pure-stop parent, prior-bar age and all OCA/callback/magnifier/stream
exclusions. Three/four unrelated orders and an intervening accepted-and-canceled
order remain exclusions. This is one active rule transferred to Pine ownership,
not a claim that its source-shape policy disappeared globally.

The comparator consumes an immutable `broker::OrderPriorityDecision` bound to
exact incarnations. It substitutes two sequence tie-breaks at their former
location, after fill phase. It does not change eligibility, fill prices, OCA
mutation, callbacks, admission or financial accounting. Shared incarnation
receipts and their other compatibility consumers remain unchanged.

## Pairing and regeneration

New codegen calls the fully qualified attachment method in the constructor,
before `strategy_create` returns and host metadata arrives:

```cpp
#if defined(PINEFORGE_HAS_EXPLICIT_PINE_EXECUTION_ADAPTER_V1)
    pineforge::BacktestEngine::attach_pine_execution_adapter();
#elif defined(PINEFORGE_HAS_EXPLICIT_PINE_CAP_V1)
    pineforge::BacktestEngine::enable_pine_intraday_cap();
#endif
```

The fallback preserves existing cap-only engines' native-to-Pine cap selection
and their old default retained-parent rule. Engines lacking both capabilities
use their established defaults. This is generated-source portability with
matching headers/runtime, not binary compatibility across engine versions.

Old cap-only generated source can compile with current headers but does **not**
select the priority exception. Regenerate it with the paired codegen, or
explicitly update a maintained C++ Pine frontend to call the attachment method
before metadata. Neither metadata nor an implicit constructor default repairs
old source. Existing checked-in corpus/tutorial C++ must likewise be regenerated
before using it for Pine behavior measurements; compilation alone is insufficient.

Rebuild every C++ consumer with matching current headers and archive. This lane
moved the class namespace to `engine_script_run_v4` (since advanced to <!-- verified HEAD -->
`engine_script_run_v19`); frozen v2/base38 and v3/f864 callers were its
compile/link rejection controls. The broker fingerprint domain and the stream
fingerprint version moved to 4 then, including priority attachment and
configuration. Prior hashes are not comparable. Public C functions, PODs and ABI/API versions are unchanged;
an erased handle still belongs to its creating module.

## Remaining native gap and evidence limits

Bare native execution retains the ordinary phase/sequence scan. An older child
visited while flat can still be skipped until a later pass, exposing a transient
position to the intervening close-time observer. No identity-bound child
activation/event queue has been added. That causal scheduler is a separate next
slice, not an unused reducer in this extraction.

Literal C++ controls preserve the old Pine price, quantity, timing and financial
assertions with explicit fixture attachment; they separately pin native metadata
inertness, exclusions, OCA, copy/reset/hash and current lifetime safety. Codegen
checks compile source only. None establishes TV truth, measured campaign
neutrality, actualZERO, absence of population regressions or readiness to publish.
