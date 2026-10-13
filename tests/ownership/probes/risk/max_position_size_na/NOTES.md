# max_position_size_na

Pinned before execution from Appendix B's risk setter row and the EN-132
D1/D2 rulings. The sink is `set_pine_risk_max_position_size`; expected
completion with at least one closed trade.

The explicitly typed Pine `na` becomes a double NaN. D1 preserves it as a
 disabled/absent limit; this is a completion control, never a source-invalid
failure. The C++ NA rows test the raw NaN at host, replay and snapshot sinks.

Feed rule for i=0..39: timestamp=1736121600000+60000*i, O=100, H=101,
L=99, C=100, volume=1000+10*i. All bars are synthetic and finite. No engine,
compiler, transpiler, test or corpus was run while authoring this probe.
