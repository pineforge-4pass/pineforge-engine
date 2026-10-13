# max_position_size_finite

Pinned before execution from Appendix B's risk setter row and the EN-132
D1/D2 rulings. The sink is `set_pine_risk_max_position_size`; expected
completion with at least one closed trade.

The raw finite value stays valid. A one-unit long entry on bar 5 and close
on bar 10 both fit under this limit. The native FINITE rows are the paired
control; main is expected to pass these preservation rows.

Feed rule for i=0..39: timestamp=1736121600000+60000*i, O=100, H=101,
L=99, C=100, volume=1000+10*i. All bars are synthetic and finite. No engine,
compiler, transpiler, test or corpus was run while authoring this probe.
