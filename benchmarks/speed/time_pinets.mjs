#!/usr/bin/env node
// benchmarks/speed/time_pinets.mjs
//
// PineTS does not have a strategy backtester yet; we time the canonical
// indicator script (run_pinets_canonical.mjs) across N subprocess runs as an
// apples-to-apples indicator-cost measurement.
//
// run_pinets_canonical.mjs always runs the same hardcoded canonical Pine
// source regardless of CLI arguments, so a single timing loop covers the
// representative cost for any strategy.
//
// Usage:
//   N=20 node speed/time_pinets.mjs
//
// Output: JSON {"canonical": {median_ms, p95_ms, n}} to stdout.
// Includes Node.js startup + PineTS import time (realistic wall-time cost).

import { spawnSync } from "node:child_process";
import { performance } from "node:perf_hooks";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const BENCH = join(__dirname, "..");
const RUNNER = join(BENCH, "runners", "run_pinets_canonical.mjs");

const N = Number(process.env.N ?? 20);

function quantile(arr, q) {
  const a = [...arr].sort((x, y) => x - y);
  const pos = (a.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  if (lo === hi) return a[lo];
  return a[lo] + (a[hi] - a[lo]) * (pos - lo);
}

function timeCanonical() {
  const samples = [];
  for (let i = 0; i < N; i++) {
    const t0 = performance.now();
    const r = spawnSync("node", [RUNNER], {
      encoding: "utf8",
      cwd: BENCH,
    });
    const elapsed = performance.now() - t0;
    if (r.status !== 0) {
      process.stderr.write(`run ${i + 1}: FAILED (rc=${r.status})\n`);
      process.stderr.write(r.stderr ?? "");
      process.exit(1);
    }
    samples.push(elapsed);
    process.stderr.write(`run ${i + 1}/${N}: ${elapsed.toFixed(1)}ms\n`);
  }
  return {
    median_ms: quantile(samples, 0.5),
    p95_ms: quantile(samples, 0.95),
    n: N,
  };
}

process.stderr.write(`time_pinets: N=${N}, runner=${RUNNER}\n`);
const result = timeCanonical();
process.stderr.write(
  `canonical: median=${result.median_ms.toFixed(1)}ms  p95=${result.p95_ms.toFixed(1)}ms\n`
);
process.stdout.write(JSON.stringify({ canonical: result }, null, 2) + "\n");
