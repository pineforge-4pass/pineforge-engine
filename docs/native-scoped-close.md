# Scoped native settlement

The trusted native settlement seam can close the surviving exposure created
by one opening request in one position cycle. It uses the engine's existing
physical book, FIFO allocation and execution fee accounting.

This is a prerequisite for native owner-bound orders. It does not place an
order or implement a trigger, child lifecycle, group effect or broker fill
transport. The native host's current market request API remains unchanged.

## Selector and entry points

`<pineforge/execution_close_scope.hpp>` declares the additive
`pineforge::execution::close_scope_v1` types:

```cpp
struct Book {};
struct OpeningExposure {
    std::uint64_t incarnation = 0;
    std::int64_t cycle = 0;
};
using CloseScope = std::variant<Book, OpeningExposure>;
```

Trusted subclasses can use these protected methods:

```cpp
execution::SettlementInspection inspect_native_settlement_scoped(
    const execution::Action&, const execution::Fill&,
    execution::CloseScope) const;

execution::Result settle_native_execution_scoped_at(
    const execution::Action&, const execution::Fill&,
    const execution::PhysicalExecutionContext&, execution::CloseScope);
```

`Book` retains whole-book Flatten, Reduce and Transact behavior. Every
original settlement and inspection method keeps its signature and selects
Book. Existing C++ aggregate layouts and C ABI exports are unchanged.

`OpeningExposure` permits Flatten and Reduce only. It selects all surviving
physical fragments with the specified opening-request incarnation, in the
engine's exact current cycle. A partially filled opening request may create
several fragments with that incarnation. Labels and the closing fill's own
incarnation do not select exposure. The trusted caller validates run identity;
the selector itself is a run-local value.

## Physical behavior

Selected Reduce closes at most the selected exposure, in FIFO order within
that selection. Selected Flatten removes every selected fragment. Unrelated
lots retain their identity, order, quantities and historical costs. The cycle
ends only when the whole physical book becomes empty.

The caller supplies an already resolved fill price. Inspection quotes one
execution ticket and reports the surviving whole-book quantity, lot count
and notional. The caller may lock that ticket in `Fill::commission_account`
before settlement. Current costs are allocated across the selected close
rows; paid entry costs remain historical and are divided proportionally
between closed quantities and survivors.

Inspection does not authorize a later allocation. Settlement revalidates
the selector against the current book and recomputes allocation. No roster
index, callback, borrowed selector or saved plan is retained.

## Refusals and failure

Validation first checks finite price, explicit fee, quantity and book
integrity in the existing order. It then checks selected action and target
validity, before zero-effect handling. A zero/absent incarnation, stale or
nonpositive cycle, flat selected book, or selected Transact returns
`InvalidCloseTarget`. Invalid targets never fall back to Book.

A zero Reduce on a valid target returns NoEffect; a nonzero explicit charge
on that no-effect call returns InvalidAccounting. For example, a NaN price
with an invalid selector returns InvalidPrice, while an absent owner with
Reduce zero and a finite nonzero fee returns InvalidCloseTarget.

Checked refusals produce no rows or account effects. A selected settlement
cannot report Applied with zero closed units. Exceptions during commitment
retain the existing abort/discard-and-replay contract; this seam does not
promise rollback or in-place retry.

See [refactor progress](native-refactor-progress.md) for the remaining
native order and adapter work.
