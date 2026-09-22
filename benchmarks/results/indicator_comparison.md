# Indicator comparison

All three engines compute the canonical indicator script ([canonical.pine](../assets/strategies/_indicators/canonical.pine)) on the same 53,929-bar OHLCV feed. This table reports per-bar absolute and relative deltas across every pair of engines.

**NA columns** count bars where one engine reported a number but the other was still in its warmup window (or vice versa). Engines disagree on warmup behaviour for some indicators (e.g. PineForge's EMA emits the first close at bar 0 while PyneCore/PineTS wait for length-1 bars of history). This is a documented semantic divergence, not a numerical defect.

**Both-num columns** are the bars where both engines emitted a value. The relative-delta percentiles are computed only over those bars.

### PineForge ↔ PyneCore

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 53909 | 8.386e-01 | 8.572e-11 | 1.780e-10 | 2.603e-10 | 3.170e-04 |
| sma21 |  20 |   0 |   0 | 53909 | 4.762e-07 | 8.542e-11 | 1.769e-10 | 2.533e-10 | 3.273e-10 |
| rsi14 |  14 |   0 |   0 | 53915 | 5.000e-09 | 4.940e-11 | 9.719e-11 | 1.581e-10 | 4.936e-10 |
| atr14 |  13 |   0 |   0 | 53916 | 3.608e-08 | 8.678e-11 | 2.790e-10 | 4.193e-10 | 4.985e-10 |
| macd_line |   0 |  25 |   0 | 53904 | 1.333e+00 | 6.611e-11 | 2.380e-10 | 4.324e-10 | 1.694e+00 |
| macd_signal |   0 |  33 |   0 | 53896 | 1.277e+00 | 6.655e-11 | 2.385e-10 | 4.315e-10 | 1.793e+00 |
| macd_hist |   0 |  33 |   0 | 53896 | 5.726e-01 | 6.830e-11 | 2.324e-10 | 4.287e-10 | 7.748e-01 |
| bb_basis |  19 |   0 |   0 | 53910 | 2.137e-11 | 1.915e-15 | 4.561e-15 | 8.392e-15 | 9.999e-15 |
| bb_upper |  19 |   0 |   0 | 53910 | 5.000e-07 | 8.449e-11 | 1.755e-10 | 2.522e-10 | 3.322e-10 |
| bb_lower |  19 |   0 |   0 | 53910 | 5.000e-07 | 8.628e-11 | 1.793e-10 | 2.580e-10 | 3.435e-10 |

### PineForge ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |   0 |  20 |   0 | 53909 | 8.386e-01 | 8.573e-11 | 1.780e-10 | 2.603e-10 | 3.170e-04 |
| sma21 |  20 |   0 |   0 | 53909 | 4.762e-07 | 8.542e-11 | 1.769e-10 | 2.533e-10 | 3.273e-10 |
| rsi14 |  14 |   0 |   0 | 53915 | 5.000e-09 | 4.943e-11 | 9.721e-11 | 1.580e-10 | 4.898e-10 |
| atr14 |  13 |   0 |   0 | 53916 | 3.610e-08 | 8.699e-11 | 2.790e-10 | 4.192e-10 | 4.988e-10 |
| macd_line |   0 |  25 |   0 | 53904 | 1.333e+00 | 6.671e-11 | 2.540e-10 | 8.601e-10 | 1.694e+00 |
| macd_signal |   0 |  33 |   0 | 53896 | 1.277e+00 | 6.380e-11 | 2.493e-10 | 6.666e-10 | 1.793e+00 |
| macd_hist |   0 |  33 |   0 | 53896 | 5.726e-01 | 7.166e-11 | 2.927e-10 | 2.416e-09 | 7.748e-01 |
| bb_basis |  19 |   0 |   0 | 53910 | 0 | 0 | 0 | 0 | 0 |
| bb_upper |  19 |   0 |   0 | 53910 | 5.331e-07 | 8.456e-11 | 1.757e-10 | 2.527e-10 | 3.338e-10 |
| bb_lower |  19 |   0 |   0 | 53910 | 5.331e-07 | 8.632e-11 | 1.794e-10 | 2.580e-10 | 3.441e-10 |

### PyneCore ↔ PineTS

| Indicator | Both-NA | A-only | B-only | Both-num | max-abs | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ema21 |  20 |   0 |   0 | 53909 | 5.275e-11 | 8.521e-15 | 1.769e-14 | 2.538e-14 | 3.359e-14 |
| sma21 |  20 |   0 |   0 | 53909 | 8.777e-11 | 8.764e-15 | 2.143e-14 | 3.500e-14 | 4.221e-14 |
| rsi14 |  14 |   0 |   0 | 53915 | 5.000e-11 | 4.963e-13 | 9.719e-13 | 1.595e-12 | 4.139e-12 |
| atr14 |  13 |   0 |   0 | 53916 | 5.000e-11 | 1.721e-12 | 4.539e-12 | 9.190e-12 | 2.101e-11 |
| macd_line |  25 |   0 |   0 | 53904 | 9.994e-11 | 4.905e-12 | 3.727e-11 | 3.873e-10 | 3.450e-07 |
| macd_signal |  33 |   0 |   0 | 53896 | 1.025e-10 | 4.292e-12 | 3.177e-11 | 3.132e-10 | 2.869e-07 |
| macd_hist |  33 |   0 |   0 | 53896 | 1.454e-10 | 1.750e-11 | 1.263e-10 | 1.269e-09 | 3.040e-07 |
| bb_basis |  19 |   0 |   0 | 53910 | 2.137e-11 | 1.915e-15 | 4.561e-15 | 8.392e-15 | 9.999e-15 |
| bb_upper |  19 |   0 |   0 | 53910 | 7.445e-08 | 1.086e-12 | 3.946e-12 | 8.353e-12 | 1.737e-11 |
| bb_lower |  19 |   0 |   0 | 53910 | 7.457e-08 | 1.116e-12 | 4.002e-12 | 8.440e-12 | 1.750e-11 |
