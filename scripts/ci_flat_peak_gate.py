#!/usr/bin/env python3
"""The flat-peak memory gate: CI holds the CR-150 flat-memory property.

One command per arm, run by .github/workflows/flat-peak.yml (ci.yml calls it on
a push to main, release.yml calls it before anything is published):

  mem     Release build with the flat-memory rows registered, the macd module,
          `ctest -L mem`, then the harness's own result files (RUN.COMPLETE and
          one case-*.json per case). ctest's exit status alone is never the
          verdict.
  census  the heap-census fixture and every corpus strategy module, then
          tests/heap_census.py over the battery and the corpus into a new
          directory, then its census.json. Only status PASS passes.

check-wiring is the static half: the workflow files and the CMake labels still
hold the gate's shape (scripts/ci_preflight.py runs it as a stage).

Every outcome that is not a proven pass exits non-zero: zero rows registered or
run, a skipped or not-run row, a row outside the expected set, an absent or
incomplete report, a RED or OPEN census, a missing tool, a stale result
directory. The gate never deletes anything, never retries and never skips. Each
arm prints how many rows or comparisons it judged.

Exit status: 0 proven pass; 1 the property or its proof failed; 2 the gate could
not run (a tool, a checkout or a build step failed before a verdict).
"""
from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ci_verify import ctest_list_count, ctest_list_names, ctest_rows  # noqa: E402
from ci_preflight import FLAT_PEAK_RUNNER, _job_values, pool_runner  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

MEM_LABEL = 'mem'
# The rows `ctest -L mem` must register and run, exactly. test_flat_memory_rss_macd
# is the macd-only part of test_flat_memory_rss_e2e (tests/CMakeLists.txt): its
# cases load no codegen output, so a plain build directory can run them.
MEM_ROWS = ('test_flat_memory_rss_macd',)
MEM_FLOOR = len(MEM_ROWS)
MEM_CASES = ('a', 'g', 'h', 'borrowed')
# The measured rungs each case's own ladder runs (tests/flat_memory_rss_e2e.py):
# fewer means the case measured less than its definition, and a short run is a
# failure here, never a pass.
MEM_RUNG_FLOORS = {'a': 4, 'g': 3, 'h': 3, 'borrowed': 0}
MEM_RESULTS = 'flat_memory_macd_results'
MEM_MODULE = Path('flat_memory_modules') / 'macd.so'
# run_json.py loads the selected-window planner from <its directory>/../lib/, the installed-image
# layout, and case (g) sends a selected-window request. The row therefore runs a staged copy of
# docker/ beside the planner library the build made (the flat_memory_prefix target); the gate
# refuses to run ctest when any of it is missing, so a bare setup error never stands for a verdict.
MEM_STAGED = (Path('flat_memory_prefix') / 'bin' / 'run_json.py',
              Path('flat_memory_prefix') / 'lib' / 'libpineforge_window_plan.so')
MEM_BUILD_TARGETS = ['pineforge', 'flat_memory_macd', 'flat_memory_prefix']
# The row that keeps every case and needs codegen outputs: lab only, and its
# label must never match `ctest -L mem` (a regular-expression search).
LAB_ONLY_ROW = 'test_flat_memory_rss_e2e'

CENSUS_SCHEMA = 'pineforge-heap-census/v1'
CENSUS_FEED = Path('corpus') / 'data' / 'ohlcv_ETH-USDT-USDT_1m.csv'
CENSUS_MIN_FEED_BYTES = 1_000_000
CENSUS_SITE_LINES = 20

WORKFLOW_FILE = './.github/workflows/flat-peak.yml'
GATE_COMMAND = 'python3 scripts/ci_flat_peak_gate.py'


def default_runner(argv: list[str], cwd: Path) -> tuple[int, str]:
    """Run argv under cwd, echo its output as it arrives, return (status, output)."""
    print('+ ' + ' '.join(shlex.quote(part) for part in argv), flush=True)
    chunks: list[str] = []
    try:
        child = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as error:
        message = f'{argv[0]}: {error}\n'
        sys.stdout.write(message)
        return 127, message
    with child:
        assert child.stdout is not None
        for raw in iter(child.stdout.readline, b''):
            text = raw.decode('utf-8', 'replace')
            chunks.append(text)
            sys.stdout.write(text)
            sys.stdout.flush()
        status = child.wait()
    return status, ''.join(chunks)


def cmake_definitions(*, corpus: bool, flat_memory: bool, launcher: str | None) -> dict[str, str]:
    """The cache entries of a gate build: Release, source layer, no sanitizer."""
    values = {
        'CMAKE_BUILD_TYPE': 'Release',
        'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON',
        # The interpreter that runs this script, as ci_verify.py passes it.
        'Python3_EXECUTABLE': sys.executable,
        'PINEFORGE_BUILD_TESTS': 'ON',
        'PINEFORGE_BUILD_TUTORIAL': 'OFF',
        'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF',
        'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
        'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'ON' if corpus else 'OFF',
        'PINEFORGE_BUILD_BENCH_STRATEGIES': 'OFF',
        'PINEFORGE_BUILD_SPEED_BENCH': 'OFF',
        'PINEFORGE_BUILD_EXAMPLES': 'OFF',
        'PINEFORGE_ENABLE_SANITIZERS': 'OFF',
        'PINEFORGE_ENABLE_COVERAGE': 'OFF',
        'PINEFORGE_STRICT_WARNINGS': 'OFF',
        'PINEFORGE_VERSION_SOURCE': 'FILE',
        'PINEFORGE_BUILD_FLAT_MEMORY_TESTS': 'ON' if flat_memory else 'OFF',
    }
    if launcher:
        values['CMAKE_C_COMPILER_LAUNCHER'] = launcher
        values['CMAKE_CXX_COMPILER_LAUNCHER'] = launcher
    return values


def configure_argv(root: Path, build: Path, definitions: dict[str, str]) -> list[str]:
    return (['cmake', '-S', str(root), '-B', str(build)]
            + [f'-D{key}={value}' for key, value in definitions.items()] + ['-Wno-dev'])


def build_argv(build: Path, jobs: int, targets: list[str]) -> list[str]:
    return ['cmake', '--build', str(build), '--parallel', str(jobs), '--target', *targets]


def load_json(path: Path):
    """The JSON document at path, or None when it is absent or unreadable."""
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def is_count(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def fail(arm: str, status: int, reasons: list[str]) -> int:
    print(f'flat-peak {arm}: FAIL')
    for reason in reasons:
        print(f'  - {reason}')
    return status


# ---- arm 1: the mem rows ---------------------------------------------------


def judge_mem_results(results: Path) -> tuple[list[str], int]:
    """The harness's own verdict files: (reasons the run is not a pass, measured rungs).

    RUN.COMPLETE is written only when every selected case ran, and each
    case-*.json carries that case's verdict. An absent file is a failure.
    """
    reasons: list[str] = []
    rungs = 0
    if (results / 'RUN.INCOMPLETE').exists():
        reasons.append(f'{results / "RUN.INCOMPLETE"} exists: the harness did not finish its cases')
    run = load_json(results / 'RUN.COMPLETE')
    if not isinstance(run, dict):
        reasons.append(f'no readable {results / "RUN.COMPLETE"}: the harness wrote no completed verdict')
    else:
        for key, want in (('verdict', 'PASS'), ('returncode', 0), ('red', 0), ('setup', 0),
                          ('errors', 0), ('passed', len(MEM_CASES)), ('judgement_deferred', False)):
            if run.get(key) != want:
                reasons.append(f'RUN.COMPLETE {key} is {run.get(key)!r}, expected {want!r}')
        for key in ('selected', 'executed'):
            if run.get(key) != list(MEM_CASES):
                reasons.append(f'RUN.COMPLETE {key} is {run.get(key)!r}, expected {list(MEM_CASES)!r}')
    for case in MEM_CASES:
        document = load_json(results / f'case-{case}.json')
        if not isinstance(document, dict):
            reasons.append(f'no readable case-{case}.json: case {case} left no verdict')
            continue
        if document.get('verdict') != 'PASS':
            reasons.append(f'case {case}: {document.get("verdict")}: {document.get("reason")}')
        measured = document.get('measured_rungs')
        if not is_count(measured) or measured < MEM_RUNG_FLOORS[case]:
            reasons.append(f'case {case} measured {measured!r} rungs, below its floor of '
                           f'{MEM_RUNG_FLOORS[case]}')
        elif measured:
            rungs += measured
    return reasons, rungs


def gate_mem(root: Path, build: Path, jobs: int, launcher: str | None, runner=default_runner) -> int:
    results = build / MEM_RESULTS
    if results.exists():
        return fail('mem', 2, [f'{results} already exists: the harness refuses to overwrite a result '
                               'file and this gate never deletes one; use a fresh --build-dir'])
    steps = (('configure', configure_argv(root, build, cmake_definitions(
                 corpus=False, flat_memory=True, launcher=launcher))),
             ('build', build_argv(build, jobs, MEM_BUILD_TARGETS)))
    for what, argv in steps:
        status, _ = runner(argv, root)
        if status != 0:
            return fail('mem', 2, [f'{what} exited {status}; no verdict was reached'])
    missing = [str(build / path) for path in (MEM_MODULE, *MEM_STAGED) if not (build / path).is_file()]
    if missing:
        return fail('mem', 2, [f'{path} was not built or staged; the rows have nothing to load'
                               for path in missing])

    # Registration first: a label that selects nothing, or something else, must
    # fail here, before a run that would pass over the wrong rows or none.
    status, listing = runner(['ctest', '--test-dir', str(build), '-N', '-L', MEM_LABEL], root)
    registered = ctest_list_names(listing.encode()) or frozenset()
    count = ctest_list_count(listing.encode())
    if status != 0 or count is None:
        return fail('mem', 2, [f'ctest -N -L {MEM_LABEL} exited {status} without a readable row count'])
    print(f'flat-peak mem: ctest -N -L {MEM_LABEL} registers {count} rows: '
          f'{", ".join(sorted(registered)) or "none"}')
    if count == 0 or registered != frozenset(MEM_ROWS) or count != len(MEM_ROWS):
        return fail('mem', 1, [
            f'label {MEM_LABEL} selects {count} rows {sorted(registered)}, expected exactly '
            f'{list(MEM_ROWS)}; zero rows means the label or PINEFORGE_BUILD_FLAT_MEMORY_TESTS is '
            'gone, an extra row is one this build directory cannot run'])

    status, output = runner(['ctest', '--test-dir', str(build), '-L', MEM_LABEL, '--no-tests=error',
                             '--output-on-failure', '--timeout', '0', '-V'], root)
    reasons: list[str] = []
    ran = 0
    try:
        rows = ctest_rows(output.encode())
    except ValueError as error:
        rows = None
        reasons.append(f'ctest output unreadable: {error}')
    if rows is None:
        reasons.append('ctest printed no closing summary: no row ran to a verdict')
    else:
        ran = rows.ran
        print(f'flat-peak mem: ctest ran {ran} rows (floor {MEM_FLOOR})')
        if ran < MEM_FLOOR:
            reasons.append(f'ctest ran {ran} rows, below the floor of {MEM_FLOOR}')
        for label, names in (('skipped', rows.skipped), ('not run', rows.not_run),
                             ('disabled', rows.disabled)):
            if names:
                reasons.append(f'{label} rows are not a pass: {", ".join(names)}')
    for name in MEM_ROWS:
        if not re.search(rf'Test\s+#\d+:\s+{re.escape(name)}\s+\.+\s*Passed\b', output):
            reasons.append(f'row {name} has no Passed result line')
    if status != 0:
        reasons.append(f'ctest exited {status}')
    found, rungs = judge_mem_results(build / MEM_RESULTS)
    reasons += found
    if reasons:
        return fail('mem', 1, reasons)
    print(f'flat-peak mem: PASS: ctest ran {ran} rows (floor {MEM_FLOOR}); harness cases '
          f'{len(MEM_CASES)}/{len(MEM_CASES)} passed over {rungs} measured rungs')
    return 0


# ---- arm 2: the heap census ------------------------------------------------


def judge_census(output: Path, status: int, battery_modes: int) -> tuple[list[str], str]:
    """census.json and the exit status of tests/heap_census.py: (reasons, summary line)."""
    reasons: list[str] = []
    report = load_json(output / 'census.json')
    summary = 'no census report'
    if not isinstance(report, dict):
        reasons.append(f'no readable {output / "census.json"}: an absent report is never a pass')
    else:
        if report.get('schema') != CENSUS_SCHEMA:
            reasons.append(f'census schema is {report.get("schema")!r}, expected {CENSUS_SCHEMA!r}')
        if report.get('status') != 'PASS':
            reasons.append(f'census status is {report.get("status")!r}; only PASS passes '
                           '(RED: a growing allocation site; OPEN: the proof is incomplete)')
        if report.get('memory_verdict') != 'FLAT':
            reasons.append(f'census memory_verdict is {report.get("memory_verdict")!r}, expected FLAT')
        for error in report.get('errors') or []:
            reasons.append(f'open proof: {error}')
        if report.get('corpus_requested') is not True:
            reasons.append('the census did not run over the corpus: a battery-only report cannot PASS')
        comparisons = report.get('comparisons')
        population = report.get('corpus_population')
        comparisons = comparisons if isinstance(comparisons, list) else []
        population = population if isinstance(population, list) else []
        floor = battery_modes + len(population)
        if not comparisons:
            reasons.append('the census judged zero comparisons')
        if not population:
            reasons.append('the census report names zero corpus probes')
        if len(comparisons) < floor:
            reasons.append(f'the census judged {len(comparisons)} comparisons, below the floor of '
                           f'{floor} ({battery_modes} battery modes + {len(population)} corpus probes)')
        grown = 0
        loose: list[str] = []
        for case in comparisons:
            if not isinstance(case, dict):
                continue
            for site in case.get('sites') or []:
                grown += 1
                if not site.get('admitted'):
                    first = str(site.get('site', '')).splitlines()[0:1] or ['']
                    loose.append(f'{case.get("name")}: {site.get("lane")} +{site.get("delta_bytes")} B '
                                 f'({site.get("bytes_per_unit")} B per {case.get("unit")}): {first[0]}')
        if loose:
            reasons.append(f'{len(loose)} growing allocation sites are not admitted:')
            reasons += [f'  {line}' for line in loose[:CENSUS_SITE_LINES]]
            if len(loose) > CENSUS_SITE_LINES:
                reasons.append(f'  ... and {len(loose) - CENSUS_SITE_LINES} more, all in census.json')
        summary = (f'{len(comparisons)} comparisons (floor {floor}), {len(population)} corpus probes, '
                   f'{grown} growing sites, {len(loose)} not admitted, status {report.get("status")}')
    if status != 0:
        reasons.append(f'tests/heap_census.py exited {status} (0 is PASS, 1 RED, 2 OPEN)')
    return reasons, summary


def gate_census(root: Path, build: Path, jobs: int, launcher: str | None, runner=default_runner,
                which=shutil.which, platform: str = sys.platform) -> int:
    if platform != 'linux':
        return fail('census', 2, [f'the heap census runs on Linux with Valgrind, not on {platform}'])
    for tool in ('valgrind', 'vgdb'):
        if which(tool) is None:
            return fail('census', 2, [f'{tool} is not installed: the census cannot run, and a missing '
                                      'tool is a failure, never a skip'])
    if not (root / 'corpus' / 'CMakeLists.txt').is_file():
        return fail('census', 2, ['the corpus submodule is not checked out (corpus/CMakeLists.txt)'])
    feed = root / CENSUS_FEED
    if not feed.is_file() or feed.stat().st_size < CENSUS_MIN_FEED_BYTES:
        return fail('census', 2, [f'{CENSUS_FEED} is absent or still a Git-LFS pointer'])
    battery = root / 'tests' / 'fixtures' / 'flat_memory'
    manifest = load_json(battery / 'heap_census_battery.json')
    modes = manifest.get('native_modes') if isinstance(manifest, dict) else None
    if not isinstance(modes, list) or not modes:
        return fail('census', 2, [f'{battery / "heap_census_battery.json"} lists no native modes'])
    output = build / 'heap-census'
    if output.exists():
        return fail('census', 2, [f'{output} already exists: tests/heap_census.py needs a new '
                                  'directory and this gate never deletes one; use a fresh --build-dir'])
    steps = (('derive the corpus feeds', [sys.executable, str(root / 'scripts' / 'derive_corpus_feeds.py')]),
             ('configure', configure_argv(root, build, cmake_definitions(
                 corpus=True, flat_memory=False, launcher=launcher))),
             ('build', build_argv(build, jobs, ['pineforge', 'test_heap_census_unique_ids',
                                                'corpus_strategies'])))
    for what, argv in steps:
        status, _ = runner(argv, root)
        if status != 0:
            return fail('census', 2, [f'{what} exited {status}; no verdict was reached'])
    status, _ = runner([sys.executable, str(root / 'tests' / 'heap_census.py'), '--build-dir', str(build),
                        '--battery', str(battery), '--corpus', str(root / 'corpus'),
                        '--output', str(output)], root)
    reasons, summary = judge_census(output, status, len(modes))
    if reasons:
        print(f'flat-peak census: {summary}')
        return fail('census', 1, reasons)
    print(f'flat-peak census: PASS: {summary}')
    return 0


# ---- the static half: the files still hold the gate's shape ----------------


def job_blocks(workflow: str) -> dict[str, list[str]]:
    """Job id -> its code lines, for the two-space job headers under the top-level jobs key."""
    lines = workflow.splitlines()
    if 'jobs:' not in lines:
        return {}
    blocks: dict[str, list[str]] = {}
    name = None
    for line in lines[lines.index('jobs:') + 1:]:
        if line and not line[0].isspace() and not line.startswith('#'):
            break
        header = re.match(r'^  ([A-Za-z_][A-Za-z0-9_-]*):[ \t]*(?:#.*)?$', line)
        if header:
            name = header.group(1)
            blocks[name] = []
        elif name is not None and line.strip() and not line.lstrip().startswith('#'):
            blocks[name].append(line)
    return blocks


def code_lines(text: str) -> list[str]:
    return [line for line in text.splitlines() if line.strip() and not line.lstrip().startswith('#')]


def step_blocks(lines: list[str]) -> list[list[str]]:
    """The steps of a job's code lines: each starts at a six-space dash."""
    steps: list[list[str]] = []
    for line in lines:
        if line.startswith('      - '):
            steps.append([line])
        elif steps:
            steps[-1].append(line)
    return steps


CONTINUE_ON_ERROR = re.compile(r'\s*(?:-\s+)?["\']?continue-on-error["\']?\s*:')


def tolerance_findings(name: str, text: str) -> list[str]:
    """A failure that a workflow absorbs: continue-on-error, `|| true`, a chain that hides a status."""
    found = []
    for line in code_lines(text):
        if CONTINUE_ON_ERROR.match(line):
            found.append(f'{name} must not continue on error: {line.strip()}')
        if '|| true' in line or '||true' in line or 'set +e' in line:
            found.append(f'{name} must not tolerate a failure: {line.strip()}')
    return found


def check_wiring(root: Path) -> list[str]:
    """Findings against the workflow files and the CMake labels; empty when the gate is wired."""
    workflows = root / '.github' / 'workflows'
    try:
        ci = (workflows / 'ci.yml').read_text()
        release = (workflows / 'release.yml').read_text()
        flat = (workflows / 'flat-peak.yml').read_text()
        cmake = (root / 'tests' / 'CMakeLists.txt').read_text()
    except OSError as error:
        return [f'cannot read {error.filename}']
    findings: list[str] = []

    # flat-peak.yml: the two arms, each one unconditional command whose status is the step's.
    if not re.search(r'^  workflow_call:', flat, re.MULTILINE):
        findings.append('flat-peak.yml must be callable (workflow_call)')
    findings += tolerance_findings('flat-peak.yml', flat)
    jobs = job_blocks(flat)
    runner_input = re.search(r'^      runner_labels:\n((?:^        .*\n)*)', flat, re.MULTILINE)
    defaults = _job_values(runner_input.group(1), 'default') if runner_input else []
    for arm, job in (('mem', 'flat-peak-mem'), ('census', 'flat-peak-census')):
        body = '\n'.join(jobs.get(job, []))
        runners = _job_values(body, 'runs-on')
        if runners == [FLAT_PEAK_RUNNER]:
            runners = defaults
        if len(runners) != 1 or not pool_runner(runners[0]):
            findings.append(f'flat-peak.yml job {job} must run on the pf-ci pool, '
                            'never a GitHub-hosted label')
        if any(re.match(r'\s+(?:-\s+)?["\']?timeout-minutes["\']?\s*:', line)
               for line in jobs.get(job, [])):
            findings.append(f'flat-peak.yml job {job}: the owner rule forbids time limits')
        command = f'{GATE_COMMAND} {arm} '
        steps = [step for step in step_blocks(jobs.get(job, []))
                 if any(command in line for line in step)]
        if len(steps) != 1:
            findings.append(f'flat-peak.yml job {job} must run `{GATE_COMMAND} {arm}` in exactly one step')
            continue
        step = steps[0]
        run = [line for line in step if command in line]
        if any(re.match(r'\s+(?:-\s+)?if\s*:', line) for line in step):
            findings.append(f'flat-peak.yml job {job}: the {arm} step must not carry an `if`')
        if any(token in run[0] for token in ('|', ';', '&&')):
            findings.append(f'flat-peak.yml job {job}: the {arm} command must stand alone '
                            '(a pipe or a chain can hide its status)')

    # ci.yml: the MAIN tier runs it, a pull request does not.
    block = job_blocks(ci).get('flat-peak')
    if block is None:
        findings.append('ci.yml must call the flat-peak gate (job flat-peak)')
    else:
        text = '\n'.join(block)
        if f'uses: {WORKFLOW_FILE}' not in text:
            findings.append(f'ci.yml job flat-peak must use {WORKFLOW_FILE}')
        conditions = [line for line in block if re.match(r'\s+if\s*:', line)]
        if len(conditions) != 1 or "github.event_name != 'pull_request'" not in conditions[0]:
            findings.append("ci.yml job flat-peak must carry one `if` that excludes pull_request: the "
                            'pull-request tier is unchanged')
    findings += tolerance_findings('ci.yml', ci)

    # release.yml: the release job needs the gate, so a regression publishes nothing.
    blocks = job_blocks(release)
    gate = blocks.get('flat-peak-gate')
    if gate is None or f'uses: {WORKFLOW_FILE}' not in '\n'.join(gate):
        findings.append(f'release.yml must call the flat-peak gate (job flat-peak-gate, uses {WORKFLOW_FILE})')
    # The gate reaches the tag push, the GitHub release, the tarballs and the hub
    # through these three edges; losing any one lets a release publish past it.
    for job, pattern, shown in (
            ('release', r'(?:flat-peak-gate|\[\s*flat-peak-gate\s*\])', 'flat-peak-gate'),
            ('prebuilt', r'release', 'release'),
            ('notify-hub', r'\[\s*release\s*,\s*prebuilt\s*\]', '[release, prebuilt]')):
        needs = [line for line in blocks.get(job, []) if re.match(r'\s{4}needs\s*:', line)]
        if len(needs) != 1 or not re.search(rf'needs:\s*{pattern}\s*$', needs[0]):
            findings.append(f'release.yml job {job} must need exactly {shown}')
    findings += tolerance_findings('release.yml', release)

    # tests/CMakeLists.txt: `mem` selects exactly the rows this build directory can run.
    labels = dict(re.findall(r'set_tests_properties\(\s*(\S+)\s+PROPERTIES\s+LABELS\s+"([^"]*)"\s*\)', cmake))
    for row in MEM_ROWS:
        registration = re.search(rf'add_test\(\s*NAME\s+{re.escape(row)}\b([^)]*)\)', cmake)
        if not registration:
            findings.append(f'tests/CMakeLists.txt registers no row {row}')
        elif '--run-json ${PINEFORGE_FLAT_MEMORY_PREFIX}/bin/run_json.py' not in registration.group(1):
            findings.append(f'tests/CMakeLists.txt row {row} must run the staged run_json.py '
                            '(--run-json ${PINEFORGE_FLAT_MEMORY_PREFIX}/bin/run_json.py): in the source '
                            'tree the selected-window planner library is not beside it')
        if MEM_LABEL not in labels.get(row, '').split(';'):
            findings.append(f'tests/CMakeLists.txt row {row} must carry the label {MEM_LABEL}')
    if LAB_ONLY_ROW not in labels:
        findings.append(f'tests/CMakeLists.txt gives {LAB_ONLY_ROW} no label set')
    for row, text in labels.items():
        for label in text.split(';'):
            if MEM_LABEL in label and not (label == MEM_LABEL and row in MEM_ROWS):
                findings.append(f'tests/CMakeLists.txt row {row} carries label {label!r}: `ctest -L '
                                f'{MEM_LABEL}` is a regular-expression search and would select it')
    return findings


# ---- command line ----------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('mem', 'census'):
        arm = commands.add_parser(name, help=f'run the {name} arm')
        arm.add_argument('--build-dir', type=Path, required=True,
                         help='a fresh build directory; an existing result directory is refused')
        arm.add_argument('--jobs', type=int, default=4, help='build parallelism')
        arm.add_argument('--ccache', action='store_true', help='route the compilers through ccache')
    commands.add_parser('check-wiring', help='check the workflow files and the CMake labels')
    args = parser.parse_args(argv)

    if args.command == 'check-wiring':
        findings = check_wiring(ROOT)
        for finding in findings:
            print(finding)
        if not findings:
            print('flat-peak wiring: ci.yml (push, no pull request), release.yml (release needs the '
                  'gate), flat-peak.yml (two arms, no tolerance) and the mem labels OK')
        return 1 if findings else 0

    if args.jobs < 1:
        parser.error('--jobs must be at least 1')
    launcher = None
    if args.ccache:
        launcher = shutil.which('ccache')
        if launcher is None:
            return fail(args.command, 2, ['--ccache was requested and ccache is not installed'])
    build = args.build_dir.resolve()
    if args.command == 'mem':
        return gate_mem(ROOT, build, args.jobs, launcher)
    return gate_census(ROOT, build, args.jobs, launcher)


if __name__ == '__main__':
    raise SystemExit(main())
