#!/usr/bin/env python3
"""n12_diff.py BASE_DIR EXP_DIR probe... -- row-level diff of engine_trades.csv (sha256 as the
identity judge hashes, universal newlines) the way the design doc states it: data rows, rows that
differ position by position, first differing row (1-based data row, CSV line = row + 1) and its
Date/time cell, extra rows, plus N12's own metric `diff base exp | grep -c '^[<>]'`."""
import csv, hashlib, io, subprocess, sys
from pathlib import Path
base, exp = Path(sys.argv[1]), Path(sys.argv[2])
for p in sys.argv[3:]:
    a, b = (base / f'{p}.csv').read_text(), (exp / f'{p}.csv').read_text()
    ra, rb = list(csv.reader(io.StringIO(a)))[1:], list(csv.reader(io.StringIO(b)))[1:]
    n = max(len(ra), len(rb))
    diff = [i for i in range(n) if i >= len(ra) or i >= len(rb) or ra[i] != rb[i]]
    first = diff[0] if diff else None
    when = ''
    if first is not None:
        row = rb[first] if first < len(rb) else ra[first]
        when = f' date={row[2]}' if len(row) > 2 else ''
    dl = subprocess.run(f"diff '{base}/{p}.csv' '{exp}/{p}.csv' | grep -c '^[<>]'", shell=True,
                        capture_output=True, text=True).stdout.strip()
    same = hashlib.sha256(a.encode()).hexdigest() == hashlib.sha256(b.encode()).hexdigest()
    # the file is newest-first; count chronologically (oldest data row = row 1) too
    ca, cb = ra[::-1], rb[::-1]
    cdiff = [i for i in range(n) if i >= len(ca) or i >= len(cb) or ca[i] != cb[i]]
    # the TradingView-recorded columns only (the tape header's width); the engine-only trailing
    # columns ('Engine entry incarnation', 'Engine range-end') renumber whenever the kernel
    # consumes an incarnation differently, without any trade moving
    width = int(__import__('os').environ.get('N12_WIDTH', '0'))
    if width:
        tdiff = [i for i in range(n) if i >= len(ca) or i >= len(cb) or ca[i][:width] != cb[i][:width]]
        tfirst = tdiff[0] if tdiff else None
        twhen = ''
        if tfirst is not None:
            row = ca[tfirst] if tfirst < len(ca) else cb[tfirst]
            twhen = f' ({row[1]} {row[2]})'
        print(f'  [{p}] first {width} columns: '
              + (f'differ={len(tdiff)} of {n} first_chronological_row={tfirst + 1}{twhen}' if tdiff
                 else f'identical on all {n} rows'))
    cfirst = cdiff[0] if cdiff else None
    cwhen = ''
    if cfirst is not None:
        row = ca[cfirst] if cfirst < len(ca) else cb[cfirst]
        cwhen = f' ({row[1]} {row[2]})' if len(row) > 2 else ''
    print(f'{p}: base_rows={len(ra)} exp_rows={len(rb)} '
          + ('IDENTICAL' if same else
             f'differ(file order)={len(diff)} of {n}; differ(chronological)={len(cdiff)} of {n} '
             f'first_chronological_row={cfirst + 1}{cwhen}')
          + f' diff_lines={dl}')
