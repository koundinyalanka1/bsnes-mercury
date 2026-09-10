#!/usr/bin/env python3
"""Compare executables built against their respective core/header revisions.

Usage: compare_ppu_benchmarks.py BASELINE_EXECUTABLE OPTIMIZED_EXECUTABLE
Run with other builds/benchmarks stopped. Emits raw timings and per-mode medians.
"""
import json
import re
import statistics
import subprocess
import sys

runs = {'baseline': [], 'optimized': []}
executables = dict(zip(runs, sys.argv[1:]))
if len(executables) != 2:
    sys.exit(__doc__)
for repeat in range(3):
    order = ('baseline', 'optimized') if repeat % 2 == 0 else ('optimized', 'baseline')
    for kind in order:
        output = subprocess.check_output([executables[kind], '300'], text=True)
        rows = [dict(mode=int(m[0]), threaded=int(m[1]), ms=float(m[2]), hash=m[3])
                for m in re.findall(r'mode=(\d+) threaded=(\d+) ms=([\d.]+) hash=(\w+)', output)]
        assert len(rows) == 16, output
        runs[kind].append(rows)
for i in range(16):
    assert len({r[i]['hash'] for group in runs.values() for r in group}) == 1, i
summary = []
for mode in range(8):
    before = statistics.median(r[2 * mode + 1]['ms'] for r in runs['baseline'])
    after = statistics.median(r[2 * mode + 1]['ms'] for r in runs['optimized'])
    summary.append(dict(mode=mode, baseline_ms=before, optimized_ms=after,
                        reduction_percent=round((1 - after / before) * 100, 1)))
print(json.dumps(dict(summary=summary, runs=runs), indent=2))
