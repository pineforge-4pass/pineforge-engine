# Reservation expansion ownership

This historical candidate described reservation expansion and typed Pine
instructions before adapter lowering. L3b removes the compatibility-order
representation; equivalent live-request and adapter-placement facts provide
the public projection. Public C ABI 4, stream API 1 and pending-mirror schema 1
remain unchanged. The frozen shipped mirror prefix is preserved; reservation
and frozen-instruction facts are appended fields of that mirror
(`include/pineforge/pending_order_mirror.hpp`, 406 POD fields today).

The EXIT owns a capture of cycle and side plus its first later admitted
incarnation. Its selected MARKET sources own exact receiver receipts. A new
successful capture can explicitly reassign a pending source; erasure or
replacement alone never transfers it. Executable `qty` is the only mutable
finite capacity. Original QuantityRequest intent and reservation basis remain
historical. Open capture permits live-All only for its captured exposure.

Pine still owns selection, timing, OCA ordering, risk behavior and the separate
margin-revival direct-close path. Exact multiple-tracker routing and captured
cycle validity are source corrections, not proven behavior-neutral changes.
The existing dispatch retirement ledger prevents replay and excludes retired
receivers before physical compaction; no second consumed bit or owner map exists.

Native literal tests are in `tests/test_reservation_expansion_l4c.cpp` (the
registered twin of the retired `tests/test_reservation_expansion.cpp`); old and new
C++ header/link boundaries are checked by `scripts/check_script_cpp_abi.py`.
They establish bounded ownership and pairing contracts. Compatibility outcomes
require a separate assessment with unchanged reference evidence.
