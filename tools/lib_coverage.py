#!/usr/bin/env python3
"""A line-coverage figure for lib/, merged across the whole fixture suite.

Two things make this possible, and neither is obvious (docs/reviews/tasks.md F135).

A module reached through `use` is normally the copy bundled into the binary, and a bundled
declaration carries a NULL `source_file` -- which `src/coverage.c` uses to decide what to count, so
`lib/` was invisible to `--coverage` exactly as it was to `--lint` (F133). Pointing
ZIMAN_MODULE_PATH at the sources loads them from disk with real paths and they are counted.

And coverage is per process, while the suite is 945 separate ones. F13 added `--coverage-lcov` for
this: each fixture writes its own .info and they are merged here by taking, for every line, the
highest hit count any run gave it.

Usage: tools/lib_coverage.py [--lcov out.info]. Prints a per-module table and the total.
"""
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DIRS = ["lib/core", "lib/db", "lib/http", "lib/time"]


def fixtures():
    d = os.path.join(ROOT, "tests", "fixtures")
    for name in sorted(os.listdir(d)):
        if name.endswith(".zm"):
            yield os.path.join(d, name)


def args_for(path):
    side = path[:-3] + ".args"
    if os.path.exists(side):
        return open(side).read().split()
    return []


def merge(info_path, hits):
    current = None
    for line in open(info_path):
        line = line.strip()
        if line.startswith("SF:"):
            current = line[3:]
        elif line.startswith("DA:") and current:
            n, count = line[3:].split(",")
            key = (current, int(n))
            hits[key] = max(hits.get(key, 0), int(count))


def main():
    env = dict(os.environ, ZIMAN_MODULE_PATH=":".join(os.path.join(ROOT, d) for d in DIRS))
    hits = {}
    ran = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, fx in enumerate(fixtures()):
            out = os.path.join(tmp, "%d.info" % i)
            try:
                subprocess.run([os.path.join(ROOT, "ziman"), "--coverage",
                                "--coverage-lcov=" + out] + args_for(fx) + [fx],
                               env=env, capture_output=True, timeout=20)
            except subprocess.TimeoutExpired:
                continue
            if os.path.exists(out):
                merge(out, hits)
                ran += 1

    per_file = defaultdict(lambda: [0, 0])  # [hit, total]
    for (sf, _line), count in hits.items():
        rel = os.path.relpath(sf, ROOT) if os.path.isabs(sf) else sf
        if not rel.startswith("lib/"):
            continue
        per_file[rel][1] += 1
        if count > 0:
            per_file[rel][0] += 1

    if not per_file:
        print("lib_coverage.py: no lib/ lines were counted at all -- is ZIMAN_MODULE_PATH reaching "
              "the sources?", file=sys.stderr)
        sys.exit(1)

    hit = sum(v[0] for v in per_file.values())
    total = sum(v[1] for v in per_file.values())
    for rel in sorted(per_file, key=lambda r: per_file[r][0] / max(per_file[r][1], 1)):
        h, t = per_file[rel]
        print("  %-28s %5d/%-5d %3d%%" % (rel, h, t, round(100 * h / t) if t else 0))
    print("lib_coverage.py: %d/%d lines (%d%%) across %d modules, from %d fixture runs"
          % (hit, total, round(100 * hit / total), len(per_file), ran))
    # The two database drivers are not reachable from `tests/fixtures/` at all -- they need a live
    # server, and their suites are `tests/run_pg_tests.sh` and `run_mysql_tests.sh`. Reporting one
    # number that silently includes a thousand lines nothing here could ever execute would make the
    # figure look worse than the suite is, and would hide movement in everything else.
    drivers = {r: v for r, v in per_file.items() if r in ("lib/db/pg.zm", "lib/db/mysql.zm")}
    if drivers:
        dh = sum(v[0] for v in drivers.values())
        dt = sum(v[1] for v in drivers.values())
        print("  (of which %d/%d lines are the pg and mysql drivers, which this suite cannot reach: "
              "%d/%d = %d%% without them)"
              % (dh, dt, hit - dh, total - dt, round(100 * (hit - dh) / (total - dt))))


if __name__ == "__main__":
    main()
