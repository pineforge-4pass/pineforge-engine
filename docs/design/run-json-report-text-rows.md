# Report-text allocation and memory verification on Linux x86-64

Run from the engine checkout on the verification box. These are commands
and acceptance criteria; the measured numbers it records are in the last
section.

The production change is confined to the resident JSON temporary in
`docker/run_json.py`. Use the same native strategy libraries, generated
settings, charts, Python executable and arena size for both producers.
The reference producer is `3ea5a0d6:docker/run_json.py`; engine, codegen and
ABI changes are outside this comparison.

## Python regression rows

The allocation rows were written before replacing the production buffer, but
their first version did not fail on the old code (it traced only the write and
`seek(0)`), and the ceilings tried after the first box runs were replaced by
the structural one below (the first version traced only the write and
`seek(0)` and passed on the old code; a ceiling built from an unexplained
read-chunk constant was replaced by the structural one).
The ceiling is `5 * S // 2`, 2.5 times the document's encoded size `S`,
derived from the two mechanisms: the old `StringIO` keeps the written text
(1 byte per ASCII character) and realizes a 4 byte-per-character buffer at
its first read, so its peak is at least 5 times `S` by construction; the new
byte buffer plus a bounded read-back working set measured about 1.5 times
`S` on a Linux box with Python 3.12.3. Trace the completed write and
`seek(0)` and the real `_write_report` read/write/close path (the old buffer
is realized at the first read, so the read-back is inside the window)
without retaining another report. A second row compares the peaks of an
8 MiB and a 16 MiB document: the slope per added byte is at most 2.5 (old 5
by construction, new about 1.0 to 1.125), so a fixed read-back term cancels.
There is no RSS assertion in the test.

Replay the private, frozen preparer to obtain the red result without
editing the production file:

```bash
python3 -B - <<'PY'
from pathlib import Path
import sys
import pytest

sys.path.insert(0, str(Path("docker").resolve()))
import run_json_stringio_test as rows

rows.run_json._prepare_report_json = rows._old_prepare_report_json
raise SystemExit(pytest.main([
    "-q", "-s", "-p", "no:cacheprovider",
    "docker/run_json_stringio_test.py::test_write_seek_peak_is_one_encoded_document",
    "docker/run_json_stringio_test.py::test_peak_growth_is_about_one_byte_per_added_byte",
]))
PY
```

Expected: exit 1, all six cases fail (four at `assert peak <= ceiling`, two at
`assert slope <= 2.5`). The old `StringIO` accumulator and its four-byte
character representation, realized at the first read, exceed the
document-derived bound; the printed `PEAKS` lines show about 1.1 times the
document after the writes and 5 times after the read-back, and the `SLOPE`
lines about 5 per added byte. Record the failure log; a passing or skipped
red row is not evidence. Run it on the Python of the production image
(Debian bookworm, 3.11.2) as well as on the build host's.

Run the edited implementation in a fresh process:

```bash
python3 -B -m pytest -q -rs -s -p no:cacheprovider docker/run_json_stringio_test.py
```

Expected: every row passes (17 tests), no skips; the `READ-CHUNK` line prints
the traced peak of one 1 MiB read-back; the `PEAKS` lines show
about 1.0 times the document after the writes and about 1.5 times after the
read-back, and the `SLOPE` lines about 1.0 to 1.125 per added byte. Small zero-trade and trading runs
must produce exactly the private reference's stdout bytes. Unicode inputs,
raw Unicode text and CRLF whitespace must survive unchanged. A late
serialization failure publishes zero report bytes; `main()` retains its
one failure JSON line and status 1. Mid-write `OSError` and short writes
retain the accepted report prefix, status 1 and the stderr reason, without
appending failure JSON to stdout. Both jailed and unjailed inherited-fd
runs must preserve the complete sink stream and stdout bytes, with the
32,768-record run buffering at most 32 KiB of summary text.

## Peak RSS and byte comparison of the sink-off jailed route

Set these environment values to the existing measurement artifacts of the
sink-off jailed route (a jailed run with no report sink):

| Variable | Value |
| --- | --- |
| `RJ_REPORT_TEXT_ZERO_SO` | Absolute path to the box-built zero-trade strategy library |
| `RJ_REPORT_TEXT_TRADE_SO` | Absolute path to the box-built trading strategy library |
| `RJ_REPORT_TEXT_CHART_2M` | Absolute path to the 2,000,000-bar `pf_bar_t/v1` chart, with its `.json` sidecar |
| `RJ_REPORT_TEXT_CHART_8M` | Absolute path to the 8,000,000-bar chart, with its `.json` sidecar |
| `RJ_REPORT_TEXT_ARENA_BYTES` | Decimal arena size admitted for every cell; keep it fixed for every cell |
| `RJ_REPORT_TEXT_RESULTS` | Absolute path to a new directory on disk with room for all stdout artifacts |

The command below runs two fresh processes per producer/module/bar-count
cell, sequentially. It uses the ordinary FULL report, jailed profile, no
report sink, binary chart and one-minute input/script timeframes. Use the
same modules and settings for every cell. It fixes only the report
clock, so elapsed time cannot change stdout bytes. The parent uses the
existing streaming report census after the measured process exits.
Peak RSS comes from the kernel's `wait4().ru_maxrss`, not sampling.
Do not overlap this command with `run_corpus.sh` or any build.

```bash
python3 -B - <<'PY'
import filecmp
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

root = Path.cwd().resolve()
assert sys.platform.startswith("linux") and platform.machine() == "x86_64"
sys.path.insert(0, str(root / "scripts"))
from measure_run_json_temporary_sink import _ReportCensus

modules = {
    "zero": Path(os.environ["RJ_REPORT_TEXT_ZERO_SO"]).resolve(),
    "trading": Path(os.environ["RJ_REPORT_TEXT_TRADE_SO"]).resolve(),
}
charts = {
    2_000_000: Path(os.environ["RJ_REPORT_TEXT_CHART_2M"]).resolve(),
    8_000_000: Path(os.environ["RJ_REPORT_TEXT_CHART_8M"]).resolve(),
}
arena_bytes = int(os.environ["RJ_REPORT_TEXT_ARENA_BYTES"])
assert arena_bytes > 0
output = Path(os.environ["RJ_REPORT_TEXT_RESULTS"]).resolve()
output.mkdir(parents=True, exist_ok=False)
before = output / "before_run_json.py"
before.write_bytes(subprocess.check_output(
    ["git", "show", "3ea5a0d6:docker/run_json.py"], cwd=root))
producers = {"before": before, "after": root / "docker/run_json.py"}

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()

for count, chart in charts.items():
    assert chart.stat().st_size == 48 * count
    assert json.loads(Path(str(chart) + ".json").read_text())["count"] == count
for module in modules.values():
    assert module.is_file()
manifest = {
    "base": "3ea5a0d6",
    "python": sys.version,
    "platform": platform.platform(),
    "arena_bytes": arena_bytes,
    "sha256": {str(path): sha256(path) for path in (
        *modules.values(), *charts.values(), *producers.values())},
}
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

worker = r'''
import ctypes
import fcntl
import importlib.util
import os
from pathlib import Path
import sys
import time
from types import SimpleNamespace

root, producer, module, chart, size = sys.argv[1:]
size = int(size)
# MFD_CLOEXEC | MFD_ALLOW_SEALING, including Python builds without os.memfd_create.
libc = ctypes.CDLL(None, use_errno=True)
fd = libc.memfd_create(b"report-text-arena", 3)
if fd < 0:
    number = ctypes.get_errno()
    raise OSError(number, os.strerror(number))
os.ftruncate(fd, size)
fcntl.fcntl(fd, 1033, 7)  # F_ADD_SEALS: GROW | SHRINK | SEAL
assert fcntl.fcntl(fd, 1034) == 7
if fd != 5:
    os.dup2(fd, 5)
    os.close(fd)
os.environ.update(PF_SCRATCH_ARENA_FD="5",
                  PF_SCRATCH_ARENA_BYTES=str(size),
                  PF_SCRATCH_ARENA_JAILED="1")
sys.path.insert(0, str(Path(root) / "docker"))
spec = importlib.util.spec_from_file_location("run_json", producer)
runner = importlib.util.module_from_spec(spec)
sys.modules["run_json"] = runner
spec.loader.exec_module(runner)
runner.time = SimpleNamespace(time=lambda: 10.0,
                              perf_counter_ns=time.perf_counter_ns)
raise SystemExit(runner.main([
    "--so", module, "--ohlcv-bin", chart, "--verify-bin", "full",
    "--profile", "jailed", "--input-tf", "1", "--script-tf", "1",
]))
'''

environment = dict(os.environ, PYTHONIOENCODING="utf-8",
                   PYTHONDONTWRITEBYTECODE="1")
rows = []
paths = {}
for kind, module in modules.items():
    for count, chart in charts.items():
        for repeat in (1, 2):
            for version, producer in producers.items():
                label = f"{version}-{kind}-{count}-{repeat}"
                report = output / f"{label}.stdout.json"
                errors = output / f"{label}.stderr"
                with report.open("wb") as stdout, errors.open("wb") as stderr:
                    with subprocess.Popen(
                        [sys.executable, "-B", "-c", worker, str(root),
                         str(producer), str(module), str(chart), str(arena_bytes)],
                        cwd=root, env=environment, stdout=stdout, stderr=stderr,
                    ) as process:
                        _, status, usage = os.wait4(process.pid, 0)
                        process.returncode = os.waitstatus_to_exitcode(status)
                assert process.returncode == 0, (label, errors)
                assert errors.stat().st_size == 0, (label, errors)
                with report.open("rb") as file:
                    census = _ReportCensus(file).read()
                assert census["input_bars"] == census["equity"] == count
                assert (census["trades"] == 0) == (kind == "zero")
                row = dict(version=version, module=kind, bars=count, repeat=repeat,
                           peak_rss_bytes=int(usage.ru_maxrss) * 1024, **census)
                rows.append(row)
                paths[version, kind, count, repeat] = report
                print(json.dumps(row, sort_keys=True), flush=True)
            assert filecmp.cmp(paths["before", kind, count, repeat],
                               paths["after", kind, count, repeat], shallow=False)
(output / "rows.json").write_text(json.dumps(rows, indent=2) + "\n")

for kind in modules:
    for version in producers:
        def mean(count, field):
            return statistics.mean(
                row[field] for row in rows
                if (row["module"], row["version"], row["bars"]) ==
                   (kind, version, count))
        rss_slope = (mean(8_000_000, "peak_rss_bytes") -
                     mean(2_000_000, "peak_rss_bytes")) / 6_000_000
        text_slope = (mean(8_000_000, "report_bytes") -
                      mean(2_000_000, "report_bytes")) / 6_000_000
        print(json.dumps({
            "module": kind, "version": version, "rss_bytes_per_bar": rss_slope,
            "rss_minus_chart_bytes_per_bar": rss_slope - 48,
            "stdout_bytes_per_bar": text_slope,
            "report_multiple": (rss_slope - 48) / text_slope,
        }, sort_keys=True))
PY
```

Retain `manifest.json`, `rows.json`, stdout/stderr artifacts and the command
log. Every paired stdout file must compare byte-for-byte, with the expected
bar and trade populations and successful exits. Compare the mean-of-two
slopes `(peak(8M) - peak(2M)) / 6M`, in bytes per bar; subtract 48 B/bar
only when comparing the report component. Measured on build hosts (kernel
`ru_maxrss`, stdout byte-equal in every base/head pair): the zero-trade
codegen module has 63 B/bar of stdout and slopes of 363.0 B/bar for the base
(five runs on two hosts, spread 0.007 percent) and 191.0 B/bar for the head
(four runs); one lower base draw of 344.0 was seen on a host that was later
lost, with no mechanism found, and every other base run, including the module
with no `ta` call, gave 363.0. The base slope is
exactly the chart plus five times the output: 48 + 5 x 63 = 363.0, and for the
hand-written trading module 48 + 5 x 143.88 = 767.4 (stdout 143.88 B/bar):
one byte per character of text plus the four-byte-per-character buffer. The
head measured 191.0 (zero-trade) and 289.7 (trading), a saving of 172 and
477.7 B/bar; what remains in the head is not decomposed here: it is not a
constant multiple of the output (2.27 times for zero-trade, 1.68 times for
trading) and the module with no `ta` call gives the same slopes (363.0 and
191.0), so the precalc vector is not part of this slope. An encoding failure,
altered stdout, missing records, partial report on serialization failure or an
unexplained change of these slopes fails the comparison.

## Full verification

The verification host runs preflight and the complete verification profile on
the box; the corpus sweep must run separately from the measurement command:

```bash
python3 scripts/ci_preflight.py
python3 scripts/ci_verify.py release --build-dir build --jobs 4
./scripts/run_corpus.sh
```
