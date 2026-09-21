# PyneCore speed — not measured: the host was never quiet

The PyneCore timing (`speed/time_pynecore.py`, N=20 subprocess runs per strategy, median and
p95, 8 strategies timed concurrently) may only start on a quiet host: 1-minute load average
below 6.0 and no `cmake --build`, `ctest` or `ci_verify` process running, re-checked before
every chunk. The gate was polled every ~5 minutes for four hours, 2026-09-21 15:49:00Z →
19:49:13Z (100 probes; the full log, with 20 earlier probes from 14:19Z, is
`quiet_host_polls.tsv`). No probe passed:

| | 1-min load | build/test processes |
|---|---:|---:|
| minimum | 7.74 (17:49:26Z, with 8 processes) | 5 |
| median | 97.0 | — |
| maximum | 233.2 | 16 |

The processes were other lanes' `ci_verify` debug / release / kernel runs, their `cmake
--build` and `ctest` children (E1, E2, E4–E7, E10, int12-docs). No PyneCore timing was taken,
so no `pynecore_speed.json` is staged; lane BENCH2 times PyneCore together with PineForge (see
`README.md`, BENCH2 step 6).

Probe (by executable name — a full-command `grep` also matches agent processes whose
arguments merely quote those commands):

```bash
read -r l1 _ <<<"$(sysctl -n vm.loadavg | tr -d '{}')"
ps -axo comm=,args= | awk '{ c = $1; sub(".*/", "", c)
    if (c == "ctest" || (c == "cmake" && / --build /) || (c ~ /^[Pp]ython/ && /ci_verify\.py/)) n++ }
    END { print n + 0 }'
```
