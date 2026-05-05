// Canonical-indicator runner for PineTS.
//
// Reads the same OHLCV as PineForge / PyneCore, runs the canonical Pine
// indicator source through PineTS, and dumps per-bar values to
// canonical_pinets.csv with the schema:
//   bar_index, timestamp_ms,
//   ema21, sma21, rsi14, atr14,
//   macd_line, macd_signal, macd_hist,
//   bb_basis, bb_upper, bb_lower
//
// Usage:
//   node runners/run_pinets_canonical.mjs

import { PineTS } from 'pinets';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(__dirname, '..', '..');
const csvPath = resolve(REPO, 'corpus/data/ohlcv_ETH-USDT-USDT_15m.csv');

// Load OHLCV CSV into PineTS' candle objects.
const text = readFileSync(csvPath, 'utf8');
const rows = text.trim().split('\n').slice(1);
const candles = rows.map((line) => {
    const [ts, open, high, low, close, volume] = line.split(',');
    return {
        openTime: Number(ts),
        open: Number(open),
        high: Number(high),
        low: Number(low),
        close: Number(close),
        volume: Number(volume),
    };
});

console.log(`pinets: loaded ${candles.length} candles`);

const pineTS = new PineTS(candles);

const PINE_SOURCE = `
//@version=6
indicator("Canonical Indicators")
ema21 = ta.ema(close, 21)
sma21 = ta.sma(close, 21)
rsi14 = ta.rsi(close, 14)
atr14 = ta.atr(14)
[macd, signal, hist] = ta.macd(close, 12, 26, 9)
[bb_basis, bb_upper, bb_lower] = ta.bb(close, 20, 2.0)
plot(ema21,    "ema21")
plot(sma21,    "sma21")
plot(rsi14,    "rsi14")
plot(atr14,    "atr14")
plot(macd,     "macd_line")
plot(signal,   "macd_signal")
plot(hist,     "macd_hist")
plot(bb_basis, "bb_basis")
plot(bb_upper, "bb_upper")
plot(bb_lower, "bb_lower")
`;

const t0 = performance.now();
const { plots } = await pineTS.run(PINE_SOURCE);
const elapsed = performance.now() - t0;
console.log(`pinets: run elapsed ${elapsed.toFixed(1)}ms`);

const COLS = [
    'ema21', 'sma21', 'rsi14', 'atr14',
    'macd_line', 'macd_signal', 'macd_hist',
    'bb_basis', 'bb_upper', 'bb_lower',
];

// Validate every column is present.
for (const c of COLS) {
    if (!plots[c]) {
        console.error(`pinets: missing plot ${c}`);
        process.exit(1);
    }
}

// Each plot.data is an array of (number | { value, time }). Normalize.
function asValueArray(arr) {
    return arr.map(p => {
        if (p == null) return NaN;
        if (typeof p === 'number') return p;
        return p.value ?? NaN;
    });
}
const series = Object.fromEntries(COLS.map(c => [c, asValueArray(plots[c].data)]));

// Sanity: every column should have N=candles.length entries.
for (const c of COLS) {
    if (series[c].length !== candles.length) {
        console.warn(`pinets: ${c} has ${series[c].length} samples, expected ${candles.length}`);
    }
}

// Emit canonical CSV.
const outPath = resolve(REPO, 'benchmarks/strategies/_indicators/canonical_pinets.csv');
const header = ['bar_index', 'timestamp_ms', ...COLS].join(',');
const lines = [header];
for (let i = 0; i < candles.length; i++) {
    const row = [i, candles[i].openTime];
    for (const c of COLS) {
        const v = series[c][i];
        row.push(Number.isFinite(v) ? v.toFixed(10) : '');
    }
    lines.push(row.join(','));
}
writeFileSync(outPath, lines.join('\n') + '\n');
console.log(`pinets: wrote ${outPath} (${candles.length} bars × ${COLS.length} cols)`);
