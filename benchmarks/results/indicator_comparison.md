# Indicator comparison

All three engines compute the canonical indicator script ([`canonical.pine`](../strategies/_indicators/canonical.pine)) on the same 36,361-bar OHLCV feed. This table reports per-bar absolute and relative deltas across every pair of engines.

**NA columns** count bars where one engine reported a number but the other was still in its warmup window (or vice versa). Engines disagree on warmup behaviour for some indicators (e.g. PineForge's EMA emits the first close at bar 0 while PyneCore/PineTS wait for length-1 bars of history). This is a documented semantic divergence, not a numerical defect.

**Both-num columns** are the bars where both engines emitted a value. The relative-delta percentiles are computed only over those bars.

### PineForge ↔ PyneCore

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 36341 | 6.026e+00 | 8.321e-09 | 1.756e-08 | 2.479e-08 | 3.760e-03 |
| sma21 |  20 |   0 |   0 | 36341 | 4.800e-05 | 8.310e-09 | 1.738e-08 | 2.419e-08 | 3.048e-08 |
| rsi14 |  14 |   0 |   0 | 36347 | 5.000e-07 | 4.984e-09 | 9.654e-09 | 1.553e-08 | 3.766e-08 |
| atr14 |  13 |   0 |   0 | 36348 | 3.400e-06 | 8.528e-09 | 2.819e-08 | 4.172e-08 | 4.989e-08 |
| macd_line |   0 |  25 |   0 | 36336 | 6.990e+00 | 6.563e-09 | 2.371e-08 | 4.362e-08 | 7.159e-01 |
| macd_signal |   0 |  33 |   0 | 36328 | 5.897e+00 | 6.620e-09 | 2.392e-08 | 4.421e-08 | 1.805e+00 |
| macd_hist |   0 |  33 |   0 | 36328 | 2.116e+00 | 6.814e-09 | 2.310e-08 | 4.369e-08 | 1.046e+00 |
| bb_basis |  19 |   0 |   0 | 36342 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 36342 | 5.000e-05 | 8.287e-09 | 1.738e-08 | 2.391e-08 | 3.168e-08 |
| bb_lower |  19 |   0 |   0 | 36342 | 5.000e-05 | 8.452e-09 | 1.774e-08 | 2.447e-08 | 3.228e-08 |

### PineForge ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 36341 | 6.026e+00 | 8.343e-11 | 1.756e-10 | 2.498e-10 | 3.760e-03 |
| sma21 |  20 |   0 |   0 | 36341 | 4.762e-07 | 8.225e-11 | 1.735e-10 | 2.405e-10 | 3.021e-10 |
| rsi14 |  14 |   0 |   0 | 36347 | 5.000e-09 | 4.924e-11 | 9.679e-11 | 1.573e-10 | 4.898e-10 |
| atr14 |  13 |   0 |   0 | 36348 | 3.610e-08 | 8.473e-11 | 2.795e-10 | 4.190e-10 | 4.924e-10 |
| macd_line |   0 |  25 |   0 | 36336 | 6.990e+00 | 6.728e-11 | 2.555e-10 | 1.867e-09 | 7.159e-01 |
| macd_signal |   0 |  33 |   0 | 36328 | 5.897e+00 | 6.404e-11 | 2.520e-10 | 1.118e-09 | 1.805e+00 |
| macd_hist |   0 |  33 |   0 | 36328 | 2.116e+00 | 7.122e-11 | 2.974e-10 | 4.460e-09 | 1.046e+00 |
| bb_basis |  19 |   0 |   0 | 36342 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 36342 | 5.000e-07 | 8.244e-11 | 1.738e-10 | 2.401e-10 | 3.096e-10 |
| bb_lower |  19 |   0 |   0 | 36342 | 5.000e-07 | 8.408e-11 | 1.775e-10 | 2.448e-10 | 3.196e-10 |

### PyneCore ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |  20 |   0 |   0 | 36341 | 5.000e-05 | 8.307e-09 | 1.745e-08 | 2.422e-08 | 3.165e-08 |
| sma21 |  20 |   0 |   0 | 36341 | 4.762e-05 | 8.326e-09 | 1.736e-08 | 2.404e-08 | 3.024e-08 |
| rsi14 |  14 |   0 |   0 | 36347 | 5.000e-07 | 4.983e-09 | 9.663e-09 | 1.556e-08 | 3.750e-08 |
| atr14 |  13 |   0 |   0 | 36348 | 3.364e-06 | 8.538e-09 | 2.817e-08 | 4.170e-08 | 4.972e-08 |
| macd_line |  25 |   0 |   0 | 36336 | 4.877e-06 | 6.508e-09 | 2.311e-08 | 3.977e-08 | 1.792e-07 |
| macd_signal |  33 |   0 |   0 | 36328 | 4.986e-06 | 6.555e-09 | 2.334e-08 | 4.009e-08 | 3.096e-07 |
| macd_hist |  33 |   0 |   0 | 36328 | 4.991e-07 | 6.758e-09 | 2.252e-08 | 3.977e-08 | 2.264e-07 |
| bb_basis |  19 |   0 |   0 | 36342 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 36342 | 5.000e-05 | 8.298e-09 | 1.738e-08 | 2.393e-08 | 3.164e-08 |
| bb_lower |  19 |   0 |   0 | 36342 | 5.000e-05 | 8.467e-09 | 1.772e-08 | 2.443e-08 | 3.228e-08 |
