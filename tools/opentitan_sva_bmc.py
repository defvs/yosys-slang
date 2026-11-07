#!/usr/bin/env python3
# Copyright (c) 2026 The sv-elab contributors
# SPDX-License-Identifier: ISC
"""Bounded model checking of the targets an OpenTitan SVA census lowered.

Each target the census reported PASS or PARTIAL is read again with the same
sources, its checks are lowered with `chformal -lower`, and ABC's `bmc3` runs
on the AIGER model. Resets are held active on the first step and free after
it; `--jasper-env` also sets clocks, input rates and resets up as OpenTitan's
JasperGold flow does. A canary assertion, which fails once all resets are
released, tells a run whose assumptions leave no such trace from a real one.

This finds counterexamples to the lowered properties; it proves nothing past
the depth it reaches, and it is no replacement for the commercial flow, whose
stopats and blackboxes it doesn't apply.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

RESET_RE = re.compile(r"rst\w*_ni|rst_n\w*|\w*_rst_ni")
CLOCK_RE = re.compile(r"clk\w*_i|clk\w*")
ABC_HIT_RE = re.compile(r"Output\s+(\d+) was asserted in frame\s+(\d+)")
ABC_ITER_HIT_RE = re.compile(r"Output (\d+) of miter .* was asserted in frame (\d+)")
ABC_FRAMES_RE = re.compile(r"No output (?:asserted|failed) in\s+(\d+) frames")
ABC_FRAME_LINE_RE = re.compile(r"^\s*(\d+) \+ :", re.MULTILINE)


# The clock and reset setup of OpenTitan's JasperGold flow


def parse_fpv_tcl(tcl: Path, dut: str) -> tuple[dict[str, int], dict[str, str], str | None,
                                                list[tuple[str, bool]]]:
    """The clocks with their factors, the input rates, the default rate and the
    resets with whether each is active low, as hw/formal/tools/jaspergold/fpv.tcl
    sets them up for `dut`."""
    lines = tcl.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if "specify clock(s) and reset(s)" in line)
    branches: list[tuple[str, list[str]]] = []
    current: list[str] | None = None
    for line in lines[start + 1:]:
        if line.startswith("#---") and branches:
            break
        match = re.match(r"\s*\}?\s*(?:if|elseif)\s*\{(.*)\}\s*\{\s*$", line)
        if match:
            current = []
            branches.append((match.group(1), current))
            continue
        if re.match(r"\s*\}\s*else\s*\{\s*$", line):
            current = []
            branches.append(("", current))
            continue
        if current is not None:
            current.append(line)
    chosen = next((body for cond, body in branches if cond and f'"{dut}"' in cond), None)
    if chosen is None:
        chosen = next((body for cond, body in branches if not cond), [])
    clocks: dict[str, int] = {}
    rates: dict[str, str] = {}
    default_rate = None
    resets: list[tuple[str, bool]] = []
    for line in chosen:
        line = line.strip()
        match = re.match(r"clock\s+-rate\s+-default\s+(\S+)", line)
        if match:
            default_rate = match.group(1)
            continue
        match = re.match(r"clock\s+-rate\s+\{([^}]*)\}\s+(\S+)", line)
        if match:
            for name in re.split(r"[,\s]+", match.group(1)):
                if name:
                    rates[name] = match.group(2)
            continue
        match = re.match(r"clock\s+(\S+)(.*)", line)
        if match and not match.group(1).startswith("-"):
            factor = re.search(r"-factor\s+(\d+)", match.group(2))
            clocks[match.group(1)] = int(factor.group(1)) if factor else 1
            continue
        match = re.match(r"reset\s+-expr\s+\{([^}]*)\}", line)
        if match:
            for name in match.group(1).split():
                resets.append((name.lstrip("!"), name.startswith("!")))
    return clocks, rates, default_rate, resets


def yosys_quote(value: str | Path) -> str:
    text = str(value)
    if re.fullmatch(r"[-+./:=A-Za-z0-9_$]+", text):
        return text
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def escaped(name: str) -> str:
    return "\\" + name + " "


def jasper_wrapper(top: str, ports: dict[str, tuple[str, int]], clocks: dict[str, int],
                   rates: dict[str, str], default_rate: str | None,
                   resets: list[tuple[str, bool]], clk2fflogic: bool) -> str:
    """A wrapper `fv_wrap` around `top`, in Verilog for read_verilog -formal.

    Each reset stays released once released, as JasperGold's `reset -expr`
    does once the reset phase is over. With clk2fflogic the clocks toggle on the
    global clock with their factors, and each input changes on the rising edge
    of the clock of its rate only, the resets on the falling edge of the primary
    clock."""
    clocks = {c: f for c, f in clocks.items() if c in ports}
    resets = [(n, low) for n, low in resets if n in ports]
    primary = "clk_i" if "clk_i" in clocks else next(iter(clocks), None)
    out = [f"module fv_wrap ({', '.join(escaped(n) for n in ports)});"]
    for name, (direction, width) in ports.items():
        out.append(f"\t{'input' if direction == 'input' else 'output'} [{width - 1}:0] {escaped(name)};")
    out.append(f"\t{escaped(top)} dut ({', '.join(f'.{escaped(n)}({escaped(n)})' for n in ports)});")
    for k, (name, low) in enumerate(resets):
        released = f"(({escaped(name)}) == 1'b{1 if low else 0})"
        out.append(f"\treg fv_released{k} = 1'b0;")
        out.append(f"\talways @($global_clock) if ({released}) fv_released{k} <= 1'b1;")
        out.append(f"\talways @* if (fv_released{k}) assume ({released});")
    if clk2fflogic and primary:
        out.append("\treg [15:0] fv_step = 16'd0;")
        out.append("\talways @($global_clock) fv_step <= fv_step + 16'd1;")
        out.append("\treg [15:0] fv_step_q = 16'hffff;")
        out.append("\talways @($global_clock) fv_step_q <= fv_step;")
        for clock, factor in clocks.items():
            out.append(f"\talways @* assume ({escaped(clock)} == ((fv_step / 16'd{factor}) & 16'd1));")

        def level(step: str, clock: str) -> str:
            return f"(((({step} / 16'd{clocks.get(clock, 1)}) & 16'd1)) == 16'd1)"

        def rise(clock: str) -> str:
            return f"({level('fv_step', clock)} && !{level('fv_step_q', clock)})"

        def fall(clock: str) -> str:
            return f"(!{level('fv_step', clock)} && {level('fv_step_q', clock)})"

        reset_names = {n for n, _ in resets}
        for k, (name, (direction, width)) in enumerate(ports.items()):
            if direction != "input" or name in clocks:
                continue
            if name in reset_names:
                change = fall(primary)
            else:
                rate = rates.get(name, default_rate or primary)
                change = rise(rate if rate in clocks else primary)
            out.append(f"\treg [{width - 1}:0] fv_input{k};")
            out.append(f"\talways @($global_clock) fv_input{k} <= {escaped(name)};")
            out.append(f"\talways @* if (fv_step != 16'd0 && !{change}) "
                       f"assume ({escaped(name)} == fv_input{k});")
    out.append("endmodule")
    return "\n".join(out) + "\n"


# The model of a target


def top_ports(netlist: Path, top: str) -> dict[str, tuple[str, int]]:
    modules = json.loads(netlist.read_text(encoding="utf-8"))["modules"]
    module = modules.get(top) or next(m for name, m in modules.items() if name.endswith(top))
    return {name: (port["direction"], len(port["bits"])) for name, port in module["ports"].items()}


def reset_monitor(uid: str, top: str, clock: str, resets: list[str], active_low: dict[str, bool],
                  sync: bool) -> str:
    """Resets active on the first step, and the canary that fails once all of
    them are released, bound into the top."""
    def active(name: str) -> str:
        return f"!{name}" if active_low.get(name, True) else name

    held = "\n".join(f"\t\tif (fv_first) assume ({active(r)});" for r in resets)
    released = " && ".join(f"!({active(r)})" for r in resets)
    syncs = "".join(
        f"\tfv_sync_{uid} u_sync_{r} (.clk_i({clock}), .rst_ni({r}));\n" for r in resets
    ) if sync else ""
    sync_decl = (
        f"(* blackbox *) module fv_sync_{uid} (input logic clk_i, input logic rst_ni); endmodule\n"
        if sync else ""
    )
    ports = ", ".join(f"input logic {n}" for n in [clock, *resets])
    return (
        f"module fv_reset_{uid} ({ports});\n"
        "\tlogic fv_first = 1'b1;\n"
        f"\talways_ff @(posedge {clock}) fv_first <= 1'b0;\n"
        f"\talways_comb begin\n{held}\n\tend\n"
        "\t// Must fail once all resets are released, else the run is vacuous\n"
        f"\tfv_canary: assert property (@(posedge {clock}) fv_first || !({released}));\n"
        f"{syncs}endmodule\n{sync_decl}"
        f"bind {top} fv_reset_{uid} u_fv_reset (.*);\n"
    )


def reset_sync(uid: str, clock: str) -> str:
    """For clk2fflogic: resets change only where the clock falls, so they are
    stable around each rising edge, as on a register clocked by it."""
    return (
        f"// Resets change only where {clock} falls\n"
        f"module fv_sync_{uid} (input clk_i, input rst_ni);\n"
        "\treg previous_clk, previous_rst;\n"
        "\talways @($global_clock) begin\n"
        "\t\tprevious_clk <= clk_i;\n"
        "\t\tprevious_rst <= rst_ni;\n"
        "\tend\n"
        "\talways @* if (previous_rst !== rst_ni) assume (previous_clk && !clk_i);\n"
        "endmodule\n"
    )


def model_script(read: str, top: str, args: argparse.Namespace, task: str) -> str:
    """The Yosys script up to the AIGER model, which a replay reuses."""
    steps = [
        read,
        f"prep -flatten -top {yosys_quote(top)}",
        "chformal -live -remove",
        "chformal -fair -remove",
        "chformal -cover -remove",
    ]
    if args.clk2fflogic:
        # Memories and edge-triggered checks get their clocks modelled first.
        # x bits stay x until setundef makes them free, not don't-cares.
        steps += ["memory_map", "opt -fast -keepdc", "clk2fflogic"]
    else:
        steps.append("async2sync")
    steps.append("chformal -lower")
    if args.task_only and task:
        # JasperGold proves the properties of the task only
        steps += [f"delete t:$assert c:*{task}* %d c:*fv_canary* %d", "opt_clean"]
    if args.clk2fflogic:
        steps.append("setundef -zero -init")
    steps += ["setundef -undriven -anyseq", "setundef -anyseq"]
    if not args.clk2fflogic:
        steps.append("formalff -clk2ff")
    steps += ["dffunmap", "techmap", "setundef -anyseq", "aigmap", "opt_clean",
              "delete -output", "opt_clean", "stat"]
    return "\n".join(steps) + "\n"


def run_tool(command: list[str], log: Path, timeout: int, cwd: Path) -> tuple[int, bool]:
    try:
        with log.open("w", encoding="utf-8") as handle:
            completed = subprocess.run(command, cwd=cwd, stdout=handle,
                                       stderr=subprocess.STDOUT, timeout=timeout, check=False)
        return completed.returncode, False
    except subprocess.TimeoutExpired:
        return 124, True


def run_bmc(aig: Path, outputs: int, work: Path, args: argparse.Namespace) -> dict[str, Any]:
    """Failing outputs with their frames, frames explored, and how ABC did."""
    log = work / "bmc.log"
    rc, timed_out = run_tool(
        [args.abc, "-c", f"read_aiger {aig}; fold; strash; "
         f"bmc3 -a -F {args.depth} -T {args.timeout} -v"],
        log, args.timeout + 300, work)
    text = log.read_text(encoding="utf-8", errors="replace")
    hits = {int(m.group(1)): int(m.group(2)) for m in ABC_HIT_RE.finditer(text)}
    finished = rc == 0 and ("Runtime:" in text or "No output asserted" in text)
    fallback = False
    if not finished and not timed_out:
        # bmc3 -a has been seen to crash after its first failing output. Plain
        # bmc3 stops on each failure, which is then tied off for the next run.
        fallback = True
        log = work / "bmc-iter.log"
        for _ in range(outputs + 1):
            tied = "".join(f"zeropo -N {i}; " for i in sorted(hits))
            rc, timed_out = run_tool(
                [args.abc, "-c", f"read_aiger {aig}; fold; strash; {tied}"
                 f"bmc3 -F {args.depth} -T {args.timeout} -v"],
                log, args.timeout + 300, work)
            text = log.read_text(encoding="utf-8", errors="replace")
            if rc != 0 or timed_out:
                break
            match = ABC_ITER_HIT_RE.search(text)
            if match:
                hits[int(match.group(1))] = int(match.group(2))
                continue
            finished = True
            break
    match = ABC_FRAMES_RE.search(text)
    if match:
        frames = int(match.group(1))
    else:
        explored = [int(x) for x in ABC_FRAME_LINE_RE.findall(text)]
        frames = max(explored) + 1 if explored else 0
    return {"hits": hits, "frames": frames, "finished": finished,
            "timed_out": timed_out, "abc_rc": rc, "abc_fallback": fallback}


def replay(census_dir: Path, work: Path, aig: Path, ywa: Path, outputs: list[str],
           index: int, args: argparse.Namespace) -> str:
    """A VCD of a trace failing output `index`: every other output is tied off,
    ABC writes the counterexample, which Yosys simulates on the model."""
    tied = "".join(f"zeropo -N {i}; " for i in range(len(outputs)) if i != index)
    rc, _ = run_tool(
        [args.abc, "-c", f"read_aiger {aig}; fold; strash; {tied}"
         f"bmc3 -F {args.depth} -T {args.timeout}; write_cex -a {work / 'cex.aiw'}"],
        work / "trace-abc.log", args.timeout + 300, work)
    if rc != 0 or not (work / "cex.aiw").exists():
        return "no counterexample written"
    rc, _ = run_tool([args.witness, "aiw2yw", str(work / "cex.aiw"), str(ywa),
                      str(work / "cex.yw")], work / "trace-witness.log", 600, work)
    if rc != 0:
        return "witness conversion failed"
    sim = work / "trace.ys"
    sim.write_text((work / "model.ys").read_text(encoding="utf-8")
                   + f"sim -r {yosys_quote(work / 'cex.yw')} "
                   f"-vcd {yosys_quote(work / 'cex.vcd')} -hdlname\n", encoding="utf-8")
    rc, _ = run_tool([args.yosys, "-m", str(args.plugin), "-l", str(work / "trace.log"), str(sim)],
                     work / "trace.out", args.timeout, census_dir)
    return f"{outputs[index]}: cex.vcd" if rc == 0 else "simulation of the counterexample failed"


def vcd_table(vcd: Path, wanted: list[str]) -> str:
    """The values of the signals named in `wanted`, one column per time step
    of the VCD. A name is a path from the top, or with a dot in it, the end
    of a path."""
    lines = vcd.read_text(encoding="utf-8", errors="replace").split("\n")
    codes: dict[str, list[str]] = {}
    scope: list[str] = []
    for line in lines:
        if line.startswith("$scope"):
            scope.append(line.split()[2])
        elif line.startswith("$upscope"):
            scope.pop()
        elif line.startswith("$var"):
            parts = line.split()
            code, name = parts[3], parts[4]
            full = ".".join(scope[1:] + [name])
            if any(full == w or ("." in w and full.endswith("." + w)) for w in wanted):
                codes.setdefault(code, []).append(full)
    rows: dict[str, dict[int, str]] = {}
    times = []
    time = 0
    for line in lines:
        if line.startswith("#"):
            time = int(line[1:])
            times.append(time)
            continue
        if not line or line[0] == "$":
            continue
        if line[0] == "b":
            value, code = line[1:].split()
        else:
            value, code = line[0], line[1:]
        for name in codes.get(code, []):
            rows.setdefault(name, {})[time] = value
    out = [f"{'time':45} " + " ".join(f"{t:>3}" for t in times)]
    for name in sorted(rows):
        current = None
        cells = []
        for t in times:
            current = rows[name].get(t, current)
            cells.append(str(int(current, 2)) if current and set(current) <= set("01") else str(current))
        out.append(f"{name:45} " + " ".join(f"{c:>3}" for c in cells))
    return "\n".join(out)


def check_target(target: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    tid = target["id"]
    work = (args.out / tid).resolve()
    work.mkdir(parents=True, exist_ok=True)
    census_dir = args.census_build / "targets" / tid
    read = (census_dir / "lower.ys").read_text(encoding="utf-8").splitlines()[0]
    top = target["main_top"]
    ports = top_ports(census_dir / "lower.json", top)
    inputs = [n for n, (d, _) in ports.items() if d == "input"]
    resets = [n for n in inputs if RESET_RE.fullmatch(n)]
    clocks = [n for n in inputs if CLOCK_RE.fullmatch(n)]
    active_low = {n: True for n in resets}
    task = target.get("task") or ""
    info: dict[str, Any] = {"id": tid, "top": top, "task": task}
    jasper = None
    if args.jasper_env:
        jclocks, rates, default_rate, jresets = parse_fpv_tcl(args.fpv_tcl, target.get("dut") or top)
        jresets = [(n, low) for n, low in jresets if n in ports]
        if jresets:
            resets = [n for n, _ in jresets]
            active_low = dict(jresets)
        jclocks = {c: f for c, f in jclocks.items() if c in ports}
        if jclocks:
            clocks = list(jclocks)
        jasper = (jclocks, rates, default_rate, jresets)
    info["resets"] = resets
    info["clocks"] = clocks
    if resets and clocks:
        clock = "clk_i" if "clk_i" in clocks else clocks[0]
        uid = re.sub(r"\W", "_", tid)
        sync = args.clk2fflogic and not args.jasper_env
        (work / "fv_reset.sv").write_text(
            reset_monitor(uid, top, clock, resets, active_low, sync), encoding="utf-8")
        read += " " + yosys_quote(work / "fv_reset.sv")
        if sync:
            (work / "fv_sync.v").write_text(reset_sync(uid, clock), encoding="utf-8")
            read += "\nread_verilog -formal -overwrite " + yosys_quote(work / "fv_sync.v")
    else:
        info["note"] = "no reset or clock port found, no reset assumption and no canary"
    model_top = top
    if jasper is not None:
        (work / "fv_wrap.v").write_text(
            jasper_wrapper(top, ports, *jasper, args.clk2fflogic), encoding="utf-8")
        read += "\nread_verilog -formal -sv " + yosys_quote(work / "fv_wrap.v")
        model_top = "fv_wrap"
    aig = work / "model.aig"
    ywa = work / "model.ywa"
    (work / "model.ys").write_text(model_script(read, model_top, args, task), encoding="utf-8")
    script = work / "bmc.ys"
    script.write_text((work / "model.ys").read_text(encoding="utf-8")
                      + f"write_aiger -I -B -zinit -ywmap {yosys_quote(ywa)} "
                      f"{yosys_quote(aig)}\n", encoding="utf-8")
    for stale in (aig, ywa, work / "cex.aiw", work / "cex.vcd"):
        stale.unlink(missing_ok=True)
    rc, timed_out = run_tool([args.yosys, "-m", str(args.plugin), "-l", str(work / "prep.log"),
                              str(script)], work / "prep.out", args.timeout, census_dir)
    info["prep_rc"] = rc
    if rc != 0 or not aig.exists():
        info["status"] = "PREP_TIMEOUT" if timed_out else "PREP_FAIL"
        errors = [line for line in (work / "prep.out").read_text(errors="replace").splitlines()
                  if line.startswith("ERROR:")]
        if errors:
            info["reason"] = errors[-1].removeprefix("ERROR:").strip()
        return info
    witness = json.loads(ywa.read_text(encoding="utf-8"))
    outputs = [".".join(part.lstrip("\\") for part in path) for path in witness["asserts"]]
    info["asserts"] = len(outputs)
    info["assumes"] = len(witness["assumes"])
    if not outputs:
        info["status"] = "NO_CHECKS"
        return info
    bmc = run_bmc(aig, len(outputs), work, args)
    hits = {i: f for i, f in bmc["hits"].items() if i < len(outputs)}
    info.update(frames=bmc["frames"], abc_rc=bmc["abc_rc"])
    if bmc["abc_fallback"]:
        info["abc_fallback"] = True
    canary = [i for i in hits if "fv_canary" in outputs[i]]
    failed = sorted((i for i in hits if i not in canary), key=lambda i: (hits[i], outputs[i]))
    info["failed_asserts"] = [f"{outputs[i]} @{hits[i]}" for i in failed]
    info["canary_failed"] = bool(canary)
    if not bmc["finished"] and not bmc["timed_out"]:
        info["status"] = "ABC_CRASH"
        return info
    complete = bmc["frames"] >= args.depth or len(hits) == len(outputs)
    if bmc["timed_out"] and not failed:
        status = f"BMC_TIMEOUT (to depth {bmc['frames']})"
    elif failed:
        status = "BMC_FAIL" + ("" if complete else f" (to depth {bmc['frames']})")
    elif not complete:
        status = f"BMC_TIMEOUT (to depth {bmc['frames']})"
    else:
        status = "BMC_PASS"
    if resets and clocks and not canary and not bmc["timed_out"]:
        # Every trace the assumptions leave keeps a reset active: the checks
        # can't fail, whatever they say
        status += " VACUOUS"
    info["status"] = status
    if args.trace and failed:
        info["trace"] = replay(census_dir, work, aig, ywa, outputs, failed[0], args)
        if args.trace_signals and (work / "cex.vcd").exists():
            (work / "cex.txt").write_text(
                vcd_table(work / "cex.vcd", args.trace_signals.split(",")) + "\n",
                encoding="utf-8")
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", type=Path, required=True,
                        help="results.json of the census run")
    parser.add_argument("--census-build", type=Path,
                        help="build root of the census run (default: the one results.json "
                        "records)")
    parser.add_argument("--plugin", type=Path, default=os.environ.get("PLUGIN"),
                        help="built slang.so (default: $PLUGIN, else the census plugin)")
    parser.add_argument("--yosys", default=os.environ.get("YOSYS", "yosys"))
    parser.add_argument("--abc", default=os.environ.get("YOSYS_ABC", "yosys-abc"))
    parser.add_argument("--witness", default=os.environ.get("YOSYS_WITNESS", "yosys-witness"))
    parser.add_argument("--out", type=Path, help="(default: bmc/ under the census build root)")
    parser.add_argument("--depth", type=int, default=20, help="frames for bmc3")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=1800, help="seconds for each tool run")
    parser.add_argument("--target", action="append", default=[],
                        help="configuration name or census target id")
    parser.add_argument("--clk2fflogic", action="store_true",
                        help="model clocks and asynchronous flops on the global clock")
    parser.add_argument("--jasper-env", action="store_true",
                        help="clock factors, input rates and reset expressions from fpv.tcl, "
                        "each reset held released once released")
    parser.add_argument("--task-only", action="store_true",
                        help="for a configuration with a task, check its assertions only, "
                        "as JasperGold does")
    parser.add_argument("--fpv-tcl", type=Path,
                        help="fpv.tcl to read with --jasper-env (default: the one of the "
                        "census OpenTitan tree)")
    parser.add_argument("--trace", action="store_true",
                        help="write a VCD of a counterexample to the first failing assertion")
    parser.add_argument("--trace-signals", default="",
                        help="comma-separated signal names to tabulate from the VCD")
    args = parser.parse_args()

    census = json.loads(args.results.read_text(encoding="utf-8"))
    metadata = census["metadata"]
    args.census_build = (args.census_build or Path(metadata["campaign_build_root"])).resolve()
    args.out = (args.out or args.census_build / "bmc").resolve()
    args.plugin = Path(args.plugin or metadata["plugin"]).resolve()
    if not args.plugin.is_file():
        parser.error(f"plugin not found: {args.plugin}")
    if args.fpv_tcl is None:
        args.fpv_tcl = args.census_build / "opentitan/hw/formal/tools/jaspergold/fpv.tcl"

    todo = [t for t in census["targets"] if t.get("status") in ("PASS", "PARTIAL")
            and (not args.target or t["id"] in args.target or t["name"] in args.target)]
    if not todo:
        parser.error("no lowered targets selected")
    args.out.mkdir(parents=True, exist_ok=True)
    print(f"OpenTitan SVA BMC: {len(todo)} targets, depth {args.depth}, {args.jobs} jobs",
          flush=True)
    report = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
        futures = {executor.submit(check_target, t, args): t for t in todo}
        for future in concurrent.futures.as_completed(futures):
            target = futures[future]
            try:
                info = future.result()
            except Exception as exc:  # A harness bug on one target leaves the others
                info = {"id": target["id"], "status": "HARNESS_FAIL",
                        "reason": f"{type(exc).__name__}: {exc}"}
            report.append(info)
            print(f"[{len(report):3}/{len(todo)}] {info['id']}: {info['status']} "
                  f"asserts={info.get('asserts')} assumes={info.get('assumes')} "
                  f"fails={len(info.get('failed_asserts', []))}", flush=True)
    report.sort(key=lambda info: info["id"])
    options = {k: getattr(args, k) for k in ("depth", "clk2fflogic", "jasper_env", "task_only")}
    (args.out / "bmc.json").write_text(
        json.dumps({"census": str(args.results.resolve()), "plugin": str(args.plugin),
                    "options": options, "targets": report}, indent=1) + "\n",
        encoding="utf-8")
    print(f"Results: {args.out / 'bmc.json'}", flush=True)
    return 1 if any(info["status"] == "HARNESS_FAIL" for info in report) else 0


if __name__ == "__main__":
    sys.exit(main())
