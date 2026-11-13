#!/usr/bin/env python3
"""Turn a failed build's log into GitHub Actions error annotations.

A failed run's log lives on storage a sandbox may not reach, but annotations come back through the API, so a CI run
reports why it broke without anyone opening the run page:

    ./gradlew ... 2>&1 | tee /tmp/build.log; st=${PIPESTATUS[0]}
    [ $st -eq 0 ] || python3 tools/android/ci_report.py /tmp/build.log
"""

import re
import sys

INTERESTING = re.compile(
    r"error|failed|cannot|can't|could not|no such file|not found|undefined reference|"
    r"unresolved|exception|fatal|denied|FAILURE",
    re.I,
)
BEFORE, AFTER = 2, 6
MAX_MESSAGES, MAX_CHARS = 8, 4000


def escape(text):
    """The annotation syntax: a newline ends the command, so it goes through as %0A."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def blocks(lines):
    """Runs of interesting lines, widened with the lines around them, merged when they touch."""
    spans = []
    for i, line in enumerate(lines):
        if INTERESTING.search(line):
            lo, hi = max(0, i - BEFORE), min(len(lines), i + AFTER + 1)
            if spans and lo <= spans[-1][1]:
                spans[-1][1] = hi
            else:
                spans.append([lo, hi])
    return ["\n".join(lines[lo:hi]) for lo, hi in spans]


def report(path, limit=MAX_MESSAGES):
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError as e:
        print(f"::error::no log to read ({e})")
        return 1

    found = [b for b in blocks(lines) if b.strip()]
    # Gradle, CMake and ninja all end with the same handful of lines: keep those as well as the first errors
    tail = "\n".join(lines[-25:])
    seen, out = set(), []
    for body in found[:limit]:
        key = body[-400:]
        if key in seen:
            continue
        seen.add(key)
        out.append(body[-MAX_CHARS:])
    if tail.strip() and tail[-400:] not in seen:
        out.append("(last lines)\n" + tail[-MAX_CHARS:])
    if not out:
        out = ["(nothing in the log matched, last lines)\n" + tail[-MAX_CHARS:]]

    for i, body in enumerate(out, 1):
        print(f"::error title=failed {i}/{len(out)}::{escape(body)}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(report(sys.argv[1]))
