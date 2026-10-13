# control_na_limit

Contract: revision 3 Appendix B, `entry.limit_price` after `na`/default selection;
Appendix D's legitimate `na` control; owner ruling D1. Label: control.

The explicit `limit=na` is legitimate absence. The stop is also absent by
default, so the existing entry semantics select Market. Submit one unit at
script bar 5 and close at bar 10. Pin completion and at least one trade before
execution. A blanket finite-value guard must not reject this case.

The same synthetic 40-bar rule as `control_valid` is repeated in this directory:
for i = 0..39, timestamp = 1704067200000 + 60000*i; open = 100+i; high = open+2;
low = open-1; close = open+1; volume = 1000+10*i. No market data is used.

This preservation control should pass on main and after admission lands.
Trade production checks that absence did not strand the entry; the generic
driver does not expose an order-kind trace. A computed raw NaN is not a
distinct source-invalid probe at a double door under D1.
