#!/usr/bin/env python3
"""Make suprove's per-justice result stream atomic for SymbiYosys.

SymbiYosys 0.66 consumes only the first status in the AIGER solver stream.
Suprove emits one status for each justice property, so a later failure can be
lost. For a multi-justice AIG this wrapper runs the real solver, checks that it
reported every justice property, and emits one aggregate status. Single-justice
inputs are executed directly so their witness stream remains unchanged.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def justice_count(aiger_path: Path) -> int:
    with aiger_path.open("rb") as stream:
        fields = stream.readline().split()
    if not fields or fields[0] not in (b"aig", b"aag"):
        raise ValueError(f"{aiger_path}: not an AIGER file")
    # AIGER 1.9: M I L O A [B C J F]
    return int(fields[8]) if len(fields) >= 9 else 0


def result_statuses(output: bytes) -> list[int]:
    statuses: list[int] = []
    expecting_status = True
    for line in output.splitlines():
        token = line.strip()
        if expecting_status and token in (b"0", b"1", b"2"):
            statuses.append(int(token))
            expecting_status = False
        elif token == b".":
            expecting_status = True
    return statuses


def main() -> int:
    real_solver = os.environ.get("YOSYS_SLANG_REAL_SUPROVE")
    if not real_solver:
        print(
            "YOSYS_SLANG_REAL_SUPROVE must name the pinned suprove binary",
            file=sys.stderr,
        )
        return 2
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [SUPROVE_ARGS] AIGER", file=sys.stderr)
        return 2

    aiger_path = Path(sys.argv[-1])
    try:
        justice = justice_count(aiger_path)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2

    command = [real_solver, *sys.argv[1:]]
    if justice <= 1:
        os.execv(real_solver, command)

    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    statuses = result_statuses(completed.stdout)
    if completed.returncode != 0 or len(statuses) != justice:
        sys.stdout.buffer.write(completed.stdout)
        sys.stderr.buffer.write(completed.stderr)
        print(
            f"expected {justice} per-justice results, got {len(statuses)}",
            file=sys.stderr,
        )
        return completed.returncode or 2

    if any(status == 1 for status in statuses):
        sys.stdout.write("1\n.\n")
    elif all(status == 0 for status in statuses):
        sys.stdout.write("0\n")
    else:
        sys.stdout.write("2\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
