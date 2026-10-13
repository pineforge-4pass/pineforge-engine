# control_valid

Contract: revision 3 Appendix B, `entry.id/is_long/qty` and `close.id`;
Appendix D's defined finite result control. Label: control.

Submit a finite, one-unit long Market entry at script bar 5 and close it at
bar 10. The remaining feed leaves time for next-bar fills. There is no invalid
value in this probe. Completion with at least one trade is pinned from the
contract's preserved valid-command behavior before execution.

All three seeds contain the same 40 synthetic minute bars, written from these
rules for integer i = 0..39: timestamp = 1704067200000 + 60000*i; open = 100+i;
high = open+2; low = open-1; close = open+1; volume = 1000+10*i.
These are artificial prices, with no market-data or private-fixture input.

This is a preservation control: it should pass on main and after door admission
lands. It checks real execution and trades; it does not pin a parity vector or
every price/metric field of the report.
