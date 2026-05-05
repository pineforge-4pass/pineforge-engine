# Indicator comparison

All three engines compute the canonical indicator script ([`canonical.pine`](../strategies/_indicators/canonical.pine)) on the same 36,361-bar OHLCV feed. This table reports per-bar absolute and relative deltas across every pair of engines.

**NA columns** count bars where one engine reported a number but the other was still in its warmup window (or vice versa). Engines disagree on warmup behaviour for some indicators (e.g. PineForge's EMA emits the first close at bar 0 while PyneCore/PineTS wait for length-1 bars of history). This is a documented semantic divergence, not a numerical defect.

**Both-num columns** are the bars where both engines emitted a value. The relative-delta percentiles are computed only over those bars.

### PineForge ↔ PyneCore

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 41287 | 5.743e+00 | 8.741e-09 | 1.895e-08 | 2.669e-08 | 2.546e-03 |
| sma21 |  20 |   0 |   0 | 41287 | 4.800e-05 | 8.794e-09 | 1.862e-08 | 2.630e-08 | 3.345e-08 |
| rsi14 |  14 |   0 |   0 | 41293 | 5.000e-07 | 4.985e-09 | 9.705e-09 | 1.577e-08 | 3.766e-08 |
| atr14 |  13 |   0 |   0 | 41294 | 3.400e-06 | 7.988e-09 | 2.815e-08 | 4.173e-08 | 4.989e-08 |
| macd_line |   0 |  25 |   0 | 41282 | 1.850e+00 | 6.527e-09 | 2.345e-08 | 4.246e-08 | 1.713e+00 |
| macd_signal |   0 |  33 |   0 | 41274 | 1.517e+00 | 6.624e-09 | 2.367e-08 | 4.286e-08 | 4.774e-01 |
| macd_hist |   0 |  33 |   0 | 41274 | 3.434e-01 | 6.820e-09 | 2.302e-08 | 4.219e-08 | 5.913e-01 |
| bb_basis |  19 |   0 |   0 | 41288 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 41288 | 5.000e-05 | 8.631e-09 | 1.854e-08 | 2.562e-08 | 3.346e-08 |
| bb_lower |  19 |   0 |   0 | 41288 | 5.000e-05 | 8.822e-09 | 1.897e-08 | 2.619e-08 | 3.466e-08 |

### PineForge ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 41287 | 5.743e+00 | 8.770e-11 | 1.888e-10 | 2.723e-10 | 2.546e-03 |
| sma21 |  20 |   0 |   0 | 41287 | 4.762e-07 | 8.725e-11 | 1.860e-10 | 2.618e-10 | 3.273e-10 |
| rsi14 |  14 |   0 |   0 | 41293 | 5.000e-09 | 4.943e-11 | 9.720e-11 | 1.594e-10 | 4.898e-10 |
| atr14 |  13 |   0 |   0 | 41294 | 3.610e-08 | 7.957e-11 | 2.782e-10 | 4.200e-10 | 4.924e-10 |
| macd_line |   0 |  25 |   0 | 41282 | 1.850e+00 | 6.677e-11 | 2.540e-10 | 1.022e-09 | 1.713e+00 |
| macd_signal |   0 |  33 |   0 | 41274 | 1.517e+00 | 6.383e-11 | 2.497e-10 | 7.961e-10 | 4.774e-01 |
| macd_hist |   0 |  33 |   0 | 41274 | 3.434e-01 | 7.163e-11 | 2.968e-10 | 2.970e-09 | 5.913e-01 |
| bb_basis |  19 |   0 |   0 | 41288 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 41288 | 5.000e-07 | 8.625e-11 | 1.854e-10 | 2.590e-10 | 3.338e-10 |
| bb_lower |  19 |   0 |   0 | 41288 | 4.999e-07 | 8.815e-11 | 1.896e-10 | 2.654e-10 | 3.441e-10 |

### PyneCore ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |  20 |   0 |   0 | 41287 | 5.000e-05 | 8.728e-09 | 1.882e-08 | 2.596e-08 | 3.338e-08 |
| sma21 |  20 |   0 |   0 | 41287 | 4.762e-05 | 8.829e-09 | 1.860e-08 | 2.610e-08 | 3.319e-08 |
| rsi14 |  14 |   0 |   0 | 41293 | 5.000e-07 | 4.982e-09 | 9.704e-09 | 1.577e-08 | 3.750e-08 |
| atr14 |  13 |   0 |   0 | 41294 | 3.364e-06 | 7.995e-09 | 2.812e-08 | 4.170e-08 | 4.972e-08 |
| macd_line |  25 |   0 |   0 | 41282 | 4.877e-06 | 6.471e-09 | 2.296e-08 | 3.976e-08 | 3.684e-07 |
| macd_signal |  33 |   0 |   0 | 41274 | 4.986e-06 | 6.569e-09 | 2.320e-08 | 4.019e-08 | 3.096e-07 |
| macd_hist |  33 |   0 |   0 | 41274 | 4.991e-07 | 6.780e-09 | 2.260e-08 | 3.974e-08 | 2.264e-07 |
| bb_basis |  19 |   0 |   0 | 41288 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 41288 | 5.000e-05 | 8.628e-09 | 1.855e-08 | 2.566e-08 | 3.328e-08 |
| bb_lower |  19 |   0 |   0 | 41288 | 5.000e-05 | 8.815e-09 | 1.895e-08 | 2.622e-08 | 3.447e-08 |
