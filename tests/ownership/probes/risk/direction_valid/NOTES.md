# direction_valid

Pinned control for risk direction: completes with at least one closed trade.
The valid setter runs on bar 0, a one-unit long enters on bar 5, and it closes
on bar 10. These values admit both orders. The three int setter arguments
cannot represent NaN or infinity and receive no new admission or domain rule.
The native control file tests the six valid setters, full int range and sticky
percent flags. Main is expected to pass this preservation control.

Feed rule for i=0..39: timestamp=1736121600000+60000*i, O=100, H=101,
L=99, C=100, volume=1000+10*i. Synthetic finite feed, no market data.
Expectation written before any execution. No toolchain or engine was run.
