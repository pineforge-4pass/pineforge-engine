# Reservation expansion ownership

Reservation expansion and typed Pine instructions change the internal order
layout relative to ff54. This candidate's historical engine/PendingOrder
namespace is `engine_script_run_v7`, with broker/stream fingerprint epoch7.
In the current source-layer layout, `PendingOrder` is source-owned as
`pineforge::source::PendingOrder`, not an engine-namespace type. Standalone capture,
expansion and growth-source types establish `reservation_expansion_v1`.
Public C ABI4, stream API1 and pending-mirror schema1 remain unchanged. The
142-field shipped mirror prefix is preserved; seven reservation facts and six
Pine instruction facts append in the combined155-field mirror.

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

Native literal tests are in `tests/test_reservation_expansion.cpp`; old and new
C++ header/link boundaries are checked by `scripts/check_script_cpp_abi.py`.
They establish bounded ownership and pairing contracts. Compatibility outcomes
require a separate assessment with unchanged reference evidence.
