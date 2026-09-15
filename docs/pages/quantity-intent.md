The adapter placement snapshot separates an exit's original requested amount
from the native working reservation made for that request. It replaces the
retired compatibility booleans that mixed those lifetimes.

`QuantityIntent` contains exactly one of `Units(amount)`,
`Fraction(numerator, denominator)` or `All`. Fractional requests retain their
original representation; the Pine boundary supplies percentages as `P / 100`
without a floating-point divide/multiply round trip. An absent request belongs
to pending commands outside this exit-request contract. These are request
descriptors, not an additional numerical-admission policy.

`QuantityReservation` records admitted units and the exposure basis used when
reserving them. A new request clears the old reservation; a reservation cannot
exist without an original request. Deferred exits acquire this receipt when a
live owner is bound. Additional per-entry bindings copy the original request
and record their own admitted amount and basis. Copying is by value.

`qty` and `qty_percent` retain their current executable/reserved meaning in the
existing order paths. OCA can reduce executable quantity without changing
the original intent or its prior reservation receipt. Whether that receipt
covered less than its basis is a derived numerical comparison, using the
existing caller-supplied tolerances. A deferred fraction can be classified
before resolution; an unbound Units request cannot infer coverage without a
numeric basis.

Consequently, a half-position request rounded to a minimum one-unit slot may
have `Fraction(50, 100)` intent and a full `1 / 1` reservation. Conversely an
`All` request clipped behind another reservation may hold only `3 / 4` of its
basis. Neither case can be represented correctly by one “partial” label.

Deferred market-close instructions are another producer: `strategy.close`
has already resolved its source amount to a placement target before
`queue_deferred_close_order` runs. The request records that resolved source
target as `Units(qty_to_close)`, without an exposure reservation until a later
layered binding occurs. This is not a fixed executable-quantity promise: the
preserved Pine ANY-relative rule can turn target 1 on E2 into reservation 2
with basis E4 after reversal. The initial target remains 1 while executable
`qty` and the later reservation are 2. It does not claim to retain the original
Pine percentage expression; that conversion already occurred. This preserves
the old initial nonpartial classification without fabricating a basis.

The Pine percentage rounding, minimum-slot, reservation retention, one-shot
exit-ID and deferred/replacement policies remain explicit compatibility debt
in their existing callers. This change does not generalize those policies or
change their thresholds, financial assertions or execution order.

The public `pf_pending_order_v1_t` retains its complete existing field prefix.
Its `requested_partial` and `full_percent_exit_request` fields are deprecated
read-only projections derived from the new authoritative values; native
decisions never read those output fields. New fields append:

- `quantity_intent_kind`: 0 absent, 1 Units, 2 Fraction, 3 All.
- The relevant units or fraction numerator/denominator; inactive values are 0.
- Reservation presence, admitted units and basis units.

All new facts and optional-presence discriminators participate in broker
hashing and mirror output. They are not waived. C ABI version 4 and stream
API version 1 remain unchanged, and size-limited mirror reads preserve older
callers. The internal C++ layout changes, so all consumers require a matching
rebuild and the integrated representation change requires a new internal
namespace/fingerprint epoch before publication.
