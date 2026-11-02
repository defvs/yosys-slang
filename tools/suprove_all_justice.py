#!/usr/bin/env python3
"""Prove every justice property of an AIGER model with suprove for SymbiYosys.

SymbiYosys takes only the first status in the AIGER solver stream.
Suprove emits one status for each justice property of a model without fairness
constraints, so a later failure can be lost, and for a model with several
justice properties and a fairness constraint it emits no status at all. For a
multi-justice model this wrapper runs the real solver once per justice
property, on a copy of the model that keeps that property and every fairness
constraint, and emits one aggregate status. A failure is reported with the
witness of the property that failed, renumbered to its index in the model.
Single-justice inputs are executed directly so their witness stream remains
unchanged.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


class AigerError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes, path: Path):
        self.data = data
        self.path = path
        self.pos = 0

    def line(self) -> bytes:
        end = self.data.find(b"\n", self.pos)
        if end < 0:
            raise AigerError(f"{self.path}: truncated AIGER file")
        line = self.data[self.pos : end + 1]
        self.pos = end + 1
        return line

    def varint(self) -> None:
        while True:
            if self.pos >= len(self.data):
                raise AigerError(f"{self.path}: truncated AIGER file")
            byte = self.data[self.pos]
            self.pos += 1
            if not byte & 0x80:
                return


def header_fields(header: bytes, path: Path) -> list[bytes]:
    fields = header.split()
    if not fields or fields[0] not in (b"aig", b"aag") or len(fields) < 6:
        raise AigerError(f"{path}: not an AIGER file")
    # AIGER 1.9: M I L O A [B C J F]
    return fields + [b"0"] * (10 - len(fields))


def justice_count(aiger_path: Path) -> int:
    with aiger_path.open("rb") as stream:
        return int(header_fields(stream.readline(), aiger_path)[8])


def split_justice(aiger_path: Path) -> list[bytes]:
    """Return one copy of the model for each of its justice properties."""
    data = aiger_path.read_bytes()
    reader = Reader(data, aiger_path)
    fields = header_fields(reader.line(), aiger_path)
    binary = fields[0] == b"aig"
    inputs, latches, outputs, ands, bad, constraints, justice, fairness = (
        int(field) for field in fields[2:10]
    )

    head = [reader.line() for _ in range(0 if binary else inputs)]
    head += [reader.line() for _ in range(latches + outputs + bad + constraints)]
    sizes = [reader.line() for _ in range(justice)]
    literals = [[reader.line() for _ in range(int(size))] for size in sizes]
    fair = [reader.line() for _ in range(fairness)]
    gates_start = reader.pos
    if binary:
        for _ in range(2 * ands):
            reader.varint()
    else:
        for _ in range(ands):
            reader.line()
    gates = data[gates_start : reader.pos]

    # The symbol table runs up to the line that starts the comment section
    symbols: list[bytes] = []
    while reader.pos < len(data) and data[reader.pos :].split(b"\n", 1)[0] != b"c":
        symbols.append(reader.line())
    comments = data[reader.pos :]

    fields[8] = b"1"
    header = b" ".join(fields) + b"\n"
    models: list[bytes] = []
    for index in range(justice):
        kept: list[bytes] = []
        for symbol in symbols:
            name = symbol.split(b" ", 1)[0]
            if name[:1] != b"j":
                kept.append(symbol)
            elif name == b"j%d" % index:
                kept.append(b"j0" + symbol[len(name) :])
        models.append(
            header
            + b"".join(head)
            + sizes[index]
            + b"".join(literals[index])
            + b"".join(fair)
            + gates
            + b"".join(kept)
            + comments
        )
    return models


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


def renumbered_witness(output: bytes, index: int) -> bytes:
    """The first witness in the output, naming justice property `index`."""
    lines = output.splitlines(keepends=True)
    start = next(n for n, line in enumerate(lines) if line.strip() == b"1")
    witness: list[bytes] = []
    for line in lines[start:]:
        if len(witness) == 1 and line.strip() == b"j0":
            line = b"j%d\n" % index
        witness.append(line)
        if line.strip() == b".":
            break
    return b"".join(witness)


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
        models = split_justice(aiger_path) if justice > 1 else []
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2

    if justice <= 1:
        os.execv(real_solver, [real_solver, *sys.argv[1:]])

    unknown = False
    with tempfile.TemporaryDirectory(prefix="sv-elab-justice.") as work_dir:
        for index, model in enumerate(models):
            model_path = Path(work_dir) / f"justice{index}.aig"
            model_path.write_bytes(model)
            completed = subprocess.run(
                [real_solver, *sys.argv[1:-1], str(model_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            statuses = result_statuses(completed.stdout)
            if completed.returncode != 0 or len(statuses) != 1:
                sys.stdout.buffer.write(completed.stdout)
                sys.stderr.buffer.write(completed.stderr)
                print(
                    f"justice property {index} of {justice}: expected one "
                    f"result, got {len(statuses)}",
                    file=sys.stderr,
                )
                return completed.returncode or 2
            if statuses[0] == 1:
                print(f"justice property {index} of {justice} fails", file=sys.stderr)
                sys.stdout.buffer.write(renumbered_witness(completed.stdout, index))
                return 0
            unknown |= statuses[0] == 2

    sys.stdout.write("2\n" if unknown else "0\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
