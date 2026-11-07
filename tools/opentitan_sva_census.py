#!/usr/bin/env python3
# Copyright (c) 2026 The sv-elab contributors
# SPDX-License-Identifier: ISC
"""Run every OpenTitan formal configuration through sv-elab.

This is a compatibility census, not a replacement for the commercial formal
flow. It uses OpenTitan's FuseSoC file lists and elaborates the real SVA
sources with FPV_ON. The frontend rejects a property it can't lower with an
error. The census then drops that property from a filtered copy of its source
file and elaborates again, until the design lowers or only errors outside of
properties are left. Every dropped property is reported with the diagnostic
that rejected it, and none is counted as lowered.
"""

from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import csv
import dataclasses
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any

try:
    import hjson
    import yaml
except ImportError as exc:
    raise SystemExit(
        "opentitan_sva_census.py requires hjson and PyYAML; run it from the "
        "OpenTitan Python environment or install those two packages"
    ) from exc


FORMAL_CONFIGS = (
    "hw/top_earlgrey/formal/top_earlgrey_fpv_ip_cfgs.hjson",
    "hw/top_earlgrey/formal/top_earlgrey_fpv_prim_cfgs.hjson",
    "hw/top_earlgrey/formal/top_earlgrey_fpv_sec_cm_cfgs.hjson",
    "hw/top_darjeeling/formal/top_darjeeling_fpv_ip_cfgs.hjson",
    "hw/top_darjeeling/formal/top_darjeeling_fpv_prim_cfgs.hjson",
    "hw/top_darjeeling/formal/top_darjeeling_fpv_sec_cm_cfgs.hjson",
    "hw/ip/rv_timer/fpv/rv_timer_formal_cfg.hjson",
    "hw/ip/rom_ctrl/dv/formal/rom_ctrl_formal_cfg.hjson",
)

# The diagnostics of SVA the frontend has no lowering for. One of these on a
# property gets the property dropped. Anywhere else, like a frontend error that
# isn't one of these, it stops the target from lowering.
SVA_DIAGNOSTIC_TEXT = (
    "unsupported assertion statement",
    "encountered unsupported SVA feature",
    "repetition",
    "SVA clocking event",
    "temporal delay requires",
    "liveness expression needs an acceptance condition",
    "SVA match items",
    "covering an unbounded liveness property",
    "$past gating",
    "is unsupported",
    "weak sequence",
    "sequence `and`",
    "followed-by",
    "property waits on ticks",
    "`reject_on`",
    "acceptance condition",
    "assertion action block",
    "assertions in final procedures",
    "checker instances",
    "function called in a concurrent assertion",
    "automatic variable or const cast",
    "clocking block signals",
    "@(edge)",
)
# A system function with no lowering, such as `$isunknown`, rejects the property
# it's in. In other code it's a frontend error.
SYSTEM_TASK_TEXT = "unsupported system task"


@dataclasses.dataclass(frozen=True)
class SourceFilter:
    """Rewrites of an OpenTitan source in the census copy of the tree. Each
    `(old, new)` pair must match once and keep the line count, so diagnostics
    and source attributes point at the lines of the original file."""

    path: str
    replacements: tuple[tuple[str, str], ...]
    reason: str


# OpenTitan RTL the frontend rejects, rewritten into forms it reads, with the
# same behavior. They are needed until sv-elab reads the original forms, see
# docs/opentitan_sva_census/README.md.
SOURCE_FILTERS = (
    SourceFilter(
        "hw/ip/prim/rtl/prim_assert.sv",
        (
            ("bit __name``_cond;",
             "bit __name``_cond; logic __name``_rst; assign __name``_rst = (__rst);"),
            ("always_ff @(posedge __clk or posedge __rst) begin",
             "always_ff @(posedge __clk or posedge __name``_rst) begin"),
            ("if (__rst) begin", "if (__name``_rst) begin"),
        ),
        "`ASSERT_FPV_LINEAR_FSM` clocks a flop on `posedge __rst`, an expression "
        "(`!rst_ni` by default); the flop is reset from a signal assigned the "
        "expression instead",
    ),
    SourceFilter(
        "hw/ip/tlul/rtl/tlul_assert.sv",
        (("        disable_sva <= 0;\n      end\n",
          "        disable_sva <= 0;\n      end else disable_sva <= disable_sva;\n"),),
        "the flop of `disable_sva` has an asynchronous reset and no `else`; it "
        "gets one that keeps the value",
    ),
    *(
        SourceFilter(
            f"hw/top_{top}/ip_autogen/pwrmgr/dv/sva/pwrmgr_sec_cm_checker_assert.sv",
            (("    if (!rst_ni || !io_clk_en || clk_esc_edge)\n",
              "    if (!rst_ni) clk_esc_dead_cnt <= 0; "
              "else if (!io_clk_en || clk_esc_edge)\n"),),
            "the asynchronous reset of `clk_esc_dead_cnt` is tested together with "
            "two synchronous conditions; it gets a branch of its own",
        )
        for top in ("earlgrey", "darjeeling")
    ),
)

PROPERTY_MACRO_RE = re.compile(
    r"^\s*`(?P<kind>ASSERT|ASSUME|COVER)(?:_[A-Z0-9_]+)?"
    r"\s*\(\s*(?P<name>[A-Za-z_][A-Za-z0-9_$]*)"
)
PROPERTY_STMT_RE = re.compile(
    r"^\s*(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*:\s*"
    r"(?P<kind>assert|assume|cover)\s+property\b",
    re.IGNORECASE,
)
# The assertion statements the census can drop, matched where a line starts.
# They include what the inventory of property sites leaves out as it always
# has: labels on a line of their own, immediate assertions and statements
# without a label.
LABELED_SITE_RE = re.compile(
    r"(?P<name>(?!(?:default|else|begin|end)\b)[A-Za-z_][A-Za-z0-9_$]*)\s*:\s*"
    r"(?P<kind>assert|assume|cover|restrict)\b"
)
UNLABELED_SITE_RE = re.compile(
    r"(?P<kind>assert|assume|cover|restrict)\b\s*"
    r"(?:property\b|sequence\b|final\b|#\s*0\b|\()"
)
STATEMENT_RE = re.compile(
    r"(?:[A-Za-z_][A-Za-z0-9_$]*\s*:\s*)?(?:assert|assume|cover|restrict)\b\s*"
    r"(?:property\b|sequence\b|final\b|#\s*0\b)?"
)
MACRO_CALL_RE = re.compile(r"^\s*`(?P<name>[A-Za-z_][A-Za-z0-9_$]*)")
MACRO_NAME_RE = re.compile(r"`[A-Za-z_][A-Za-z0-9_$]*")
DEFINE_RE = re.compile(
    r"^\s*`define\s+(?P<name>[A-Za-z_][A-Za-z0-9_$]*)(?P<args>\([^)]*\))?(?P<body>.*)$"
)
PROPERTY_BODY_RE = re.compile(
    r"`(?:ASSERT|ASSUME|COVER)\w*\b|\b(?:assert|assume|cover)\s+property\b"
)
SKIPPED_RE = re.compile(r"/\*|//|\"|^[ \t]*`define\b", re.MULTILINE)
DECL_RE = re.compile(
    r"^[ \t]*(?P<kind>sequence|property)\s+(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*[;(]",
    re.MULTILINE,
)
FUNCTION_RE = re.compile(
    r"^[ \t]*function\b[^;(]*?(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*[;(]", re.MULTILINE
)
DECL_END = {
    kind: re.compile(rf"\bend{kind}\b") for kind in ("sequence", "property", "function")
}
BLOCK_WORD_RE = re.compile(r"\b(?:begin|end)\b|;|\"|//|/\*")
# What a site never spans: the end of an enclosing block or declaration, or a
# macro definition
CLOSING_WORD_RE = re.compile(
    r"\b(?:end(?:module|generate|case|sequence|property|function|task|interface|"
    r"package|class|checker|program|clocking|group|specify|primitive|config)?|"
    r"join(?:_any|_none)?)\b|`define\b"
)
END_LABEL_RE = re.compile(r"\s*:\s*[A-Za-z_][A-Za-z0-9_$]*")
ELSE_RE = re.compile(r"else\b")
BEGIN_RE = re.compile(r"begin\b")
WORD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")
DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<severity>warning|error|fatal): (?P<message>.*)$"
)
MODULE_RE = re.compile(r"^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)\b", re.MULTILINE)
CORE_NAME_RE = re.compile(
    r"""^name:\s*["']?([^"' \t\r\n]+)""", re.MULTILINE
)


def git_output(repo: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(repo), *args], text=True
    ).strip()


def read_hjson(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        return hjson.load(handle)


def link_or_copy(src: str, dst: str) -> None:
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def prepare_opentitan(checkout: Path, tree: Path) -> list[dict[str, str]]:
    """The OpenTitan tree the census reads: hard links to the checkout, with
    copies of the files SOURCE_FILTERS rewrites."""
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(
        checkout, tree, symlinks=True, copy_function=link_or_copy,
        ignore=shutil.ignore_patterns(".git"),
    )
    applied = []
    for source_filter in SOURCE_FILTERS:
        path = tree / source_filter.path
        text = path.read_text(encoding="utf-8")
        for old, new in source_filter.replacements:
            if text.count(old) != 1 or old.count("\n") != new.count("\n"):
                raise RuntimeError(
                    f"source filter for {source_filter.path} does not match "
                    "this OpenTitan revision"
                )
            text = text.replace(old, new)
        # A new file in place of the link, which leaves the checkout as it is
        tmp = path.with_name(path.name + ".census-tmp")
        tmp.write_text(text, encoding="utf-8")
        os.replace(tmp, path)
        applied.append({"path": source_filter.path, "reason": source_filter.reason})
    return applied


def discover_configs(opentitan: Path) -> list[dict[str, Any]]:
    configs: list[dict[str, Any]] = []
    for relative in FORMAL_CONFIGS:
        source = opentitan / relative
        data = read_hjson(source)
        entries = data.get("use_cfgs")
        if entries is None:
            entries = [data]
        for entry in entries:
            cfg = dict(entry)
            cfg["config_source"] = relative
            if "top_earlgrey" in relative:
                cfg["top_variant"] = "earlgrey"
            elif "top_darjeeling" in relative:
                cfg["top_variant"] = "darjeeling"
            else:
                cfg["top_variant"] = "standalone"
            configs.append(cfg)

    if len(configs) != 125:
        raise RuntimeError(
            f"expected 125 OpenTitan formal configurations, found {len(configs)}"
        )
    return configs


def core_registry(opentitan: Path) -> list[str]:
    cores: list[str] = []
    for path in (opentitan / "hw").rglob("*.core"):
        try:
            match = CORE_NAME_RE.search(
                path.read_text(encoding="utf-8", errors="replace")
            )
        except OSError:
            continue
        if match:
            cores.append(match.group(1))
    return sorted(set(cores))


def unversioned_core(core: str) -> str:
    parts = core.split(":")
    if len(parts) >= 4 and re.fullmatch(r"\d+(?:\.\d+)*", parts[-1]):
        return ":".join(parts[:-1])
    return core


def resolve_core_name(
    requested: str, variant: str, registry: list[str]
) -> tuple[str, str]:
    requested_unversioned = unversioned_core(requested)
    for registered in registry:
        if unversioned_core(registered) == requested_unversioned:
            return requested, ""

    component = requested_unversioned.split(":")[-1]
    component = re.sub(r"^top_(?:earlgrey|darjeeling)_", "", component)
    candidates = [
        core for core in registry if unversioned_core(core).split(":")[-1] == component
    ]
    if not candidates:
        return requested, ""

    variant_candidates = [
        core for core in candidates if variant != "standalone" and variant in core
    ]
    if len(variant_candidates) == 1:
        resolved = variant_candidates[0]
    else:
        generic = [
            core
            for core in candidates
            if "earlgrey" not in core
            and "darjeeling" not in core
            and "englishbreakfast" not in core
        ]
        if len(generic) == 1:
            resolved = generic[0]
        elif len(candidates) == 1:
            resolved = candidates[0]
        else:
            return requested, ""
    return resolved, (
        f"pinned config core {requested!r} was not registered; "
        f"resolved by component and top variant to {resolved!r}"
    )


def split_defines(value: Any) -> list[str]:
    if not value:
        return []
    if isinstance(value, list):
        words = [str(item) for item in value]
    else:
        words = shlex.split(str(value))
    return [word.removeprefix("+define+") for word in words]


def parse_parameters(cfg: dict[str, Any]) -> list[str]:
    params: list[str] = []
    value = cfg.get("params")
    if value:
        text = str(value).strip().removeprefix("{").removesuffix("}")
        words = shlex.split(text)
        if len(words) % 2:
            raise ValueError(f"odd parameter list for {cfg['name']}: {value}")
        params.extend(f"{words[i]}={words[i + 1]}" for i in range(0, len(words), 2))
    for option in cfg.get("build_opts", []):
        match = re.fullmatch(r"-define\s+(\S+)\s+(.+)", str(option))
        if match:
            params.append(f"{match.group(1)}={match.group(2)}")
    return params


def target_id(cfg: dict[str, Any], index: int) -> str:
    variant = cfg["top_variant"]
    safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", str(cfg["name"]))
    return f"{index:03d}-{variant}-{safe_name}"


def fusesoc_command(opentitan: Path, work: Path, cfg: dict[str, Any]) -> list[str]:
    cmd = [
        "fusesoc",
        "--cores-root",
        str(opentitan / "hw"),
        "run",
        "--mapping=lowrisc:prim_generic:all:0.1",
    ]
    if cfg["top_variant"] == "earlgrey":
        cmd.append("--mapping=lowrisc:systems:top_earlgrey:0.1")
    elif cfg["top_variant"] == "darjeeling":
        cmd.append("--mapping=lowrisc:systems:top_darjeeling:0.1")
    else:
        # Standalone configurations still have dependencies on a top package.
        # Pinning Earl Grey makes FuseSoC's virtual-core choice deterministic.
        cmd.append("--mapping=lowrisc:systems:top_earlgrey:0.1")
    cmd.extend(
        [
            "--target=formal",
            "--tool=icarus",
            "--no-export",
            "--setup",
            "--work-root",
            str(work),
            str(cfg.get("resolved_core", cfg["fusesoc_core"])),
        ]
    )
    return cmd


def run_process(
    command: list[str],
    cwd: Path,
    timeout: int,
    log_path: Path,
    env: dict[str, str] | None = None,
) -> tuple[int, bool]:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
            env=env,
        )
        output = completed.stdout
        returncode = completed.returncode
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        output += f"\nTIMEOUT after {timeout} seconds\n"
        returncode = 124
        timed_out = True
    log_path.write_text(output, encoding="utf-8")
    return returncode, timed_out


def edam_sources(
    edam: dict[str, Any], edam_dir: Path
) -> tuple[list[Path], list[Path]]:
    sources: list[Path] = []
    includes: set[Path] = set()
    for item in edam.get("files", []):
        name = item.get("name")
        if not name:
            continue
        path = Path(name)
        if not path.is_absolute():
            path = edam_dir / path
        path = path.resolve()
        file_type = str(item.get("file_type", ""))
        if file_type not in ("systemVerilogSource", "verilogSource"):
            continue
        if item.get("is_include_file"):
            includes.add(path.parent)
        else:
            sources.append(path)
    return sources, sorted(includes)


def find_bind_tops(sources: list[Path]) -> list[str]:
    tops: set[str] = set()
    for source in sources:
        if "bind" not in source.name.lower() or not source.is_file():
            continue
        try:
            text = source.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for module in MODULE_RE.findall(text):
            if "bind" in module.lower():
                tops.add(module)
    return sorted(tops)


def yosys_quote(value: str | Path) -> str:
    text = str(value)
    if re.fullmatch(r"[-+./:=A-Za-z0-9_$]+", text):
        return text
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def lowering_setup(cfg: dict[str, Any], edam: dict[str, Any],
                   sources: list[Path], includes: list[Path]) -> dict[str, Any]:
    main_top = str(edam.get("toplevel") or cfg["dut"])
    defines = ["FPV_ON"]
    defines.extend(split_defines(cfg.get("defines")))
    if cfg.get("task"):
        defines.extend(["FPV_SEC_CM_ON", "FPV_ALERT_NO_SIGINT_ERR"])
    return {
        "main_top": main_top,
        "bind_tops": find_bind_tops(sources),
        "defines": list(dict.fromkeys(defines)),
        "parameters": parse_parameters(cfg),
        "official_blackboxes": (
            [
                "prim_count",
                "prim_double_lfsr",
                "prim_onehot_check",
                "prim_ram_1p",
                "prim_ram_1p_scr",
            ]
            if cfg.get("task")
            else []
        ),
        "blackboxes_applied": [],
        "source_count": len(sources),
        "include_dir_count": len(includes),
    }


def write_yosys_script(
    path: Path,
    setup: dict[str, Any],
    sources: list[Path],
    includes: list[Path],
    output_json: Path,
) -> None:
    args = [
        "read_slang",
        "--no-synthesis-define",
        "--ignore-timing",
        "--ignore-initial",
        "--single-unit",
        "--allow-use-before-declare",
        "--relax-enum-conversions",
        "--allow-hierarchical-const",
        # Each run reports all the properties it rejects
        "--error-limit=0",
    ]
    for define in setup["defines"]:
        args.extend(["-D", define])
    for top in [setup["main_top"], *setup["bind_tops"]]:
        args.extend(["--top", top])
    for include in includes:
        args.extend(["-I", str(include)])
    for param in setup["parameters"]:
        args.extend(["-G", param])
    args.extend(str(source) for source in sources)

    script = " ".join(yosys_quote(arg) for arg in args)
    script += f"\nhierarchy -check -top {yosys_quote(setup['main_top'])}\n"
    script += f"write_json {yosys_quote(output_json)}\n"
    path.write_text(script, encoding="utf-8")


def relative_source(path: str | Path, opentitan: Path) -> str:
    candidate = Path(path)
    try:
        resolved = candidate.resolve()
    except OSError:
        return str(candidate)
    try:
        return str(resolved.relative_to(opentitan.resolve()))
    except (OSError, ValueError):
        resolved_text = resolved.as_posix()
        marker = "/fusesoc-work/"
        if marker in resolved_text:
            return "generated/" + resolved_text.split(marker, 1)[1]
        return str(candidate)


# Scanning sources for the assertions the census can drop


def skip_string(text: str, i: int) -> int:
    """`i` is on the quote that opens a string, returns the index after it."""
    j = i + 1
    while j < len(text):
        if text[j] == "\\":
            j += 2
            continue
        if text[j] in '"\n':
            return j + 1
        j += 1
    return j


def skip_space(text: str, i: int) -> int:
    """The index of the first character from `i` on that isn't white space or
    in a comment."""
    while i < len(text):
        if text[i].isspace():
            i += 1
        elif text.startswith("//", i):
            nl = text.find("\n", i)
            i = len(text) if nl < 0 else nl + 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
        else:
            break
    return i


def balanced_end(text: str, i: int) -> int:
    """`i` is on an opening bracket, returns the index after its match."""
    depth = 0
    while i < len(text):
        c = text[i]
        if c == '"':
            i = skip_string(text, i)
            continue
        if text.startswith("//", i) or text.startswith("/*", i):
            i = skip_space(text, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def macro_call_end(text: str, i: int) -> int:
    """The end of the macro invocation at `i`, with its arguments."""
    match = MACRO_NAME_RE.match(text, i)
    j = skip_space(text, match.end())
    if j < len(text) and text[j] == "(":
        return balanced_end(text, j)
    return match.end()


def macro_arguments(text: str, start: int, end: int) -> list[str]:
    i = text.find("(", start, end)
    if i < 0:
        return []
    args: list[str] = []
    current: list[str] = []
    depth = 0
    j = i + 1
    while j < end - 1:
        c = text[j]
        if c == '"':
            k = skip_string(text, j)
            current.append(text[j:k])
            j = k
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(current))
            current = []
            j += 1
            continue
        current.append(c)
        j += 1
    args.append("".join(current))
    return args


def expand_macro(formals: list[str], body: str, actuals: list[str]) -> str:
    """The body of a macro with its arguments put in, `` pasting tokens."""
    body = body.replace("``", "\0")
    for k, formal in enumerate(formals):
        name, _, default = formal.partition("=")
        name = name.strip()
        value = actuals[k].strip() if k < len(actuals) else default.strip()
        if name:
            body = re.sub(
                r"(?<![A-Za-z0-9_$])" + re.escape(name) + r"(?![A-Za-z0-9_$])",
                lambda _: value, body,
            )
    return body.replace("\0", "")


def balanced_block(text: str) -> bool:
    """Whether `text`, a site, opens and closes its blocks and doesn't reach
    past the end of the block or declaration around it."""
    depth = 0
    j = 0
    while True:
        match = BLOCK_WORD_RE.search(text, j)
        if not match:
            break
        token = match.group(0)
        if token == '"':
            j = skip_string(text, match.start())
            continue
        if token in ("//", "/*"):
            j = skip_space(text, match.start())
            continue
        if token == "begin":
            depth += 1
        elif token == "end":
            depth -= 1
            if depth < 0:
                return False
        j = match.end()
    if depth:
        return False
    stripped = re.sub(r"\bend\b", "", strip_comments(text))
    return not CLOSING_WORD_RE.search(stripped)


def strip_comments(text: str) -> str:
    """`text` with its comments and strings blanked."""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = skip_string(text, i)
        elif text.startswith("//", i) or text.startswith("/*", i):
            j = skip_space(text, i)
            # skip_space also passes the white space after the comment
        else:
            out.append(c)
            i += 1
            continue
        out.append(" ")
        i = j
    return "".join(out)


def statement_after(text: str, i: int) -> int | None:
    """The end of the procedural statement starting at `i`, a block, a macro
    invocation or a statement up to its semicolon, or None if it runs into
    the end of what is around it."""
    if BEGIN_RE.match(text, i):
        depth = 0
        j = i
        while True:
            match = BLOCK_WORD_RE.search(text, j)
            if not match:
                return None
            token = match.group(0)
            if token == '"':
                j = skip_string(text, match.start())
            elif token in ("//", "/*"):
                j = skip_space(text, match.start())
            elif token == "begin":
                depth += 1
                j = match.end()
            elif token == "end":
                depth -= 1
                j = match.end()
                if depth == 0:
                    label = END_LABEL_RE.match(text, j)
                    return label.end() if label else j
            else:
                j = match.end()
    if text.startswith("`", i) and MACRO_NAME_RE.match(text, i):
        # As `ASSERT_ERROR(name)`, which expands to a whole statement
        end = macro_call_end(text, i)
        j = skip_space(text, end)
        return j + 1 if text.startswith(";", j) else end
    j = i
    while j < len(text):
        c = text[j]
        if c == '"':
            j = skip_string(text, j)
            continue
        if text.startswith("//", j) or text.startswith("/*", j):
            j = skip_space(text, j)
            continue
        if c in "([{":
            j = balanced_end(text, j)
            continue
        if c == ";":
            return j + 1
        match = CLOSING_WORD_RE.match(text, j)
        if match and (j == 0 or not (text[j - 1].isalnum() or text[j - 1] in "_$")):
            return None
        j += 1
    return None


def statement_end(text: str, i: int) -> int | None:
    """The end of the assertion statement at `i`: its label, the assertion with
    its parenthesized property, and its action block. None where it can't be
    told."""
    match = STATEMENT_RE.match(text, i)
    if not match:
        return None
    j = skip_space(text, match.end())
    if j >= len(text) or text[j] != "(":
        return None
    j = skip_space(text, balanced_end(text, j))
    if text.startswith(";", j):
        return j + 1
    if not ELSE_RE.match(text, j):
        # The pass statement
        end = statement_after(text, j)
        if end is None:
            return None
        j = skip_space(text, end)
        if not ELSE_RE.match(text, j):
            return end
    return statement_after(text, skip_space(text, j + len("else")))


def local_property_macros(texts: list[str]) -> dict[str, tuple[list[str], str]]:
    """The macros the sources define around assertion macros or statements,
    as in `define TLUL_COVER(SEQ) `COVER(...)`."""
    macros = {}
    for text in texts:
        lines = text.split("\n")
        k = 0
        while k < len(lines):
            match = DEFINE_RE.match(lines[k])
            if not match:
                k += 1
                continue
            body = [match.group("body")]
            while body[-1].rstrip().endswith("\\") and k + 1 < len(lines):
                body[-1] = body[-1].rstrip()[:-1]
                k += 1
                body.append(lines[k])
            k += 1
            name = match.group("name")
            text_body = "\n".join(body)
            if PROPERTY_BODY_RE.search(text_body) and not PROPERTY_MACRO_RE.match(f"`{name}("):
                formals = [a.strip() for a in (match.group("args") or "()")[1:-1].split(",")]
                macros[name] = ([f for f in formals if f], text_body)
    return macros


@dataclasses.dataclass
class Site:
    """An assertion statement or macro invocation, from `start` to `end`."""

    name: str
    kind: str
    line: int
    start: int
    end: int
    # The expansion of a macro the sources define, which the sequences and
    # properties the site uses are looked up in as well
    expansion: str = ""


class SourceText:
    """A source file with its assertion sites and its sequence, property and
    function declarations."""

    def __init__(self, path: Path, local_macros: dict[str, tuple[list[str], str]]):
        self.path = path
        self.text = path.read_text(encoding="utf-8", errors="replace")
        self.line_starts = [0]
        for match in re.finditer("\n", self.text):
            self.line_starts.append(match.end())
        self.skipped = self._skipped_regions()
        self.skipped_starts = [start for start, _ in self.skipped]
        self.sites = self._scan_sites(local_macros)
        self.decls = self._scan_decls()

    def offset(self, line: int, column: int) -> int:
        if line - 1 >= len(self.line_starts):
            return len(self.text)
        return self.line_starts[line - 1] + max(column - 1, 0)

    def line_of(self, offset: int) -> int:
        return bisect.bisect_right(self.line_starts, offset)

    def _skipped_regions(self) -> list[tuple[int, int]]:
        """Block comments and the bodies of macros, where no site starts."""
        regions = []
        text = self.text
        i = 0
        while True:
            match = SKIPPED_RE.search(text, i)
            if not match:
                break
            token = match.group(0)
            if token == "/*":
                end = text.find("*/", match.end())
                end = len(text) if end < 0 else end + 2
                regions.append((match.start(), end))
            elif token == "//":
                end = text.find("\n", match.end())
                end = len(text) if end < 0 else end
            elif token == '"':
                end = skip_string(text, match.start())
            else:
                # The define runs on over the lines that end in a backslash
                end = match.end()
                while True:
                    nl = text.find("\n", end)
                    if nl < 0:
                        end = len(text)
                        break
                    end = nl + 1
                    if not text[:nl].rstrip(" \t\r").endswith("\\"):
                        break
                regions.append((match.start(), end))
            i = max(end, match.end())
        return regions

    def _in_skipped(self, offset: int) -> bool:
        k = bisect.bisect_right(self.skipped_starts, offset) - 1
        return k >= 0 and offset < self.skipped[k][1]

    def _scan_sites(self, local_macros: dict[str, tuple[list[str], str]]) -> list[Site]:
        sites = []
        for line_no, start in enumerate(self.line_starts, 1):
            end_of_line = self.text.find("\n", start)
            line = self.text[start:] if end_of_line < 0 else self.text[start:end_of_line]
            site_start = start + len(line) - len(line.lstrip())
            if self._in_skipped(site_start) or (sites and site_start < sites[-1].end):
                continue
            match = PROPERTY_MACRO_RE.match(line)
            if match:
                sites.append(Site(match.group("name"), match.group("kind").lower(),
                                  line_no, site_start, macro_call_end(self.text, site_start)))
                continue
            match = MACRO_CALL_RE.match(line)
            if match and match.group("name") in local_macros:
                end = macro_call_end(self.text, site_start)
                formals, body = local_macros[match.group("name")]
                actuals = macro_arguments(self.text, site_start, end)
                name = actuals[0].strip() if actuals and actuals[0].strip() else match.group("name")
                kind = "cover" if "COVER" in match.group("name").upper() else "assert"
                sites.append(Site(name, kind, line_no, site_start, end,
                                  expand_macro(formals, body, actuals)))
                continue
            for regex in (LABELED_SITE_RE, UNLABELED_SITE_RE):
                match = regex.match(self.text, site_start)
                if match:
                    name = match.groupdict().get("name") or f"{match.group('kind')}@{line_no}"
                    end = statement_end(self.text, site_start)
                    # A statement whose end isn't clear isn't a site: what it
                    # rejects stays an error
                    if end is not None and balanced_block(self.text[site_start:end]):
                        sites.append(Site(name, match.group("kind"), line_no, site_start, end))
                    break
        return sites

    def _scan_decls(self) -> list[tuple[str, int, int]]:
        decls = []
        for regex, kind in ((DECL_RE, None), (FUNCTION_RE, "function")):
            for match in regex.finditer(self.text):
                if self._in_skipped(match.start("name")):
                    continue
                end = DECL_END[kind or match.group("kind")].search(self.text, match.end())
                if end:
                    decls.append((match.group("name"), match.start(), end.end()))
        return decls

    def site_at(self, offset: int) -> Site | None:
        for site in self.sites:
            if site.start <= offset < site.end:
                return site
        return None

    def decl_at(self, offset: int) -> str | None:
        """The innermost declaration around `offset`."""
        inner = None
        for name, start, end in self.decls:
            if start <= offset < end and (inner is None or start > inner[1]):
                inner = (name, start)
        return inner[0] if inner else None

    def site_uses(self, site: Site, names: set[str]) -> bool:
        words = set(WORD_RE.findall(self.text[site.start:site.end]))
        words.update(WORD_RE.findall(site.expansion))
        return bool(words & names)

    def decls_using(self, names: set[str]) -> set[str]:
        return {
            name
            for name, start, end in self.decls
            if set(WORD_RE.findall(self.text[start:end])) & names
        }


class TargetSources:
    """The source files of a target, with the sites the census has dropped
    from copies of them."""

    def __init__(self, sources: list[Path], filtered_dir: Path):
        self.sources = sources
        self.source_set = set(sources)
        self.filtered_dir = filtered_dir
        texts = []
        for source in sources:
            try:
                texts.append(source.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                pass
        self.macros = local_property_macros(texts)
        self.texts: dict[Path, SourceText | None] = {}
        self.dropped: dict[Path, list[Site]] = {}
        self.copies: dict[Path, Path] = {}

    def text(self, path: Path) -> SourceText | None:
        if path not in self.texts:
            try:
                self.texts[path] = SourceText(path, self.macros)
            except OSError:
                self.texts[path] = None
        return self.texts[path]

    def attribute(self, diagnostic: dict[str, Any]) -> list[tuple[Path, Site]]:
        """The sites a diagnostic is on: the one around it, or else the sites
        using the sequence, property or function it is in."""
        if not diagnostic.get("path"):
            return []
        path = Path(diagnostic["path"])
        source = self.text(path)
        if source is None:
            return []
        offset = source.offset(diagnostic["line"], diagnostic["column"])
        site = source.site_at(offset)
        if site:
            return [(path, site)]
        name = source.decl_at(offset)
        if name is None:
            return []
        texts = [t for t in (self.text(p) for p in self.sources) if t is not None]
        names = {name}
        while True:
            more = set().union(*(t.decls_using(names) for t in texts)) - names
            if not more:
                break
            names |= more
        return [(t.path, s) for t in texts for s in t.sites if t.site_uses(s, names)]

    def drop(self, sites: list[tuple[Path, Site]]) -> list[tuple[Path, Site]]:
        """Drop the sites from the copies of their files, returns those not
        dropped before."""
        new = []
        for path, site in sites:
            dropped = self.dropped.setdefault(path, [])
            if any(s.start == site.start for s in dropped):
                continue
            dropped.append(site)
            new.append((path, site))
        for path in dict.fromkeys(p for p, _ in new):
            chars = list(self.text(path).text)
            for site in self.dropped[path]:
                # A null statement or item takes the place of the site, which is
                # right in any context
                for k in range(site.start, site.end):
                    if chars[k] != "\n":
                        chars[k] = " "
                chars[site.start] = ";"
            copy = self.copies.get(path)
            if copy is None:
                copy = self.filtered_dir / str(len(self.copies)) / path.name
                copy.parent.mkdir(parents=True, exist_ok=True)
                self.copies[path] = copy
            copy.write_text("".join(chars), encoding="utf-8")
        return new

    def current(self) -> list[Path]:
        return [self.copies.get(source, source) for source in self.sources]

    def include_dirs(self, includes: list[Path]) -> list[Path]:
        """The include directories, then those of the copied sources, which
        the copies no longer sit in."""
        extra = [p.parent for p in self.copies if p.parent not in includes]
        return [*includes, *dict.fromkeys(extra)]

    def originals(self) -> dict[Path, Path]:
        return {copy.resolve(): path for path, copy in self.copies.items()}


def parse_diagnostics(log: str, cwd: Path, originals: dict[Path, Path],
                      opentitan: Path) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    for line in log.splitlines():
        match = DIAG_RE.match(line)
        if not match:
            if line.startswith("ERROR:") or line.startswith("error:"):
                diagnostics.append(
                    {
                        "file": "",
                        "line": 0,
                        "column": 0,
                        "severity": "error",
                        "message": line.split(":", 1)[1].strip(),
                        "class": "tool",
                    }
                )
            continue
        item: dict[str, Any] = match.groupdict()
        item["line"] = int(item["line"])
        item["column"] = int(item["column"])
        path = (cwd / item["file"]).resolve()
        path = originals.get(path, path)
        item["path"] = str(path)
        item["file"] = relative_source(path, opentitan)
        # A warning rejects nothing, such as the one on a weak sequence that
        # never fails, though its text may match
        item["class"] = (
            "unsupported_sva"
            if item["severity"] != "warning"
            and any(text.lower() in item["message"].lower() for text in SVA_DIAGNOSTIC_TEXT)
            else "frontend"
        )
        diagnostics.append(item)
    return diagnostics


def rejects_property(diagnostic: dict[str, Any], on_site: bool) -> bool:
    """Is the diagnostic the frontend rejecting SVA it can't lower?"""
    if diagnostic["severity"] == "warning" or diagnostic["class"] == "tool":
        return False
    if diagnostic["class"] == "unsupported_sva":
        return True
    return on_site and diagnostic["message"].startswith(SYSTEM_TASK_TEXT)


def read_cells(path: Path, originals: dict[Path, Path], opentitan: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    cells: list[dict[str, Any]] = []
    for module_name, module in data.get("modules", {}).items():
        for cell_name, cell in module.get("cells", {}).items():
            cell_type = cell.get("type")
            if cell_type not in ("$check", "$live", "$fair"):
                continue
            attrs = cell.get("attributes", {})
            source = attrs.get("src", "")
            if source:
                # Yosys source attributes are filename:line.column[-line.column].
                source_match = re.match(r"^(.*?):(\d+)\.(\d+)", source)
                if source_match:
                    original = Path(source_match.group(1)).resolve()
                    original = originals.get(original, original)
                    source = (
                        f"{relative_source(original, opentitan)}:"
                        f"{source_match.group(2)}.{source_match.group(3)}"
                    )
            cells.append(
                {
                    "module": module_name,
                    "name": cell_name,
                    "type": cell_type,
                    "flavor": cell.get("parameters", {}).get("FLAVOR", ""),
                    "source": source,
                }
            )
    return cells


def run_target(
    repo: Path,
    opentitan: Path,
    plugin: Path,
    build_root: Path,
    cfg: dict[str, Any],
    index: int,
    setup_timeout: int,
    lowering_timeout: int,
) -> dict[str, Any]:
    ident = target_id(cfg, index)
    target_dir = build_root / "targets" / ident
    target_dir.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "id": ident,
        "index": index,
        "name": cfg["name"],
        "variant": cfg["top_variant"],
        "config_source": cfg["config_source"],
        "core": cfg["fusesoc_core"],
        "resolved_core": cfg.get("resolved_core", cfg["fusesoc_core"]),
        "core_resolution": cfg.get("core_resolution", ""),
        "dut": cfg.get("dut", ""),
        "task": cfg.get("task", ""),
        "stopats": cfg.get("stopats", []),
        "after_load": cfg.get("after_load", []),
    }

    setup_log = target_dir / "fusesoc.log"
    setup_env = os.environ.copy()
    python_paths = [
        entry for entry in sys.path if entry and "site-packages" in entry
    ]
    if setup_env.get("PYTHONPATH"):
        python_paths.append(setup_env["PYTHONPATH"])
    setup_env["PYTHONPATH"] = os.pathsep.join(python_paths)
    fusesoc_work = target_dir / "fusesoc-work"
    if fusesoc_work.exists():
        shutil.rmtree(fusesoc_work)
    setup_rc, setup_timed_out = run_process(
        fusesoc_command(opentitan, fusesoc_work, cfg),
        repo,
        setup_timeout,
        setup_log,
        setup_env,
    )
    edam_paths = sorted(fusesoc_work.glob("*.eda.yml"))
    result["fusesoc_returncode"] = setup_rc
    result["fusesoc_timeout"] = setup_timed_out
    result["fusesoc_log"] = str(setup_log.relative_to(build_root))
    # FuseSoC 2.4.5 can fail while instantiating Edalize after successfully
    # writing EDAM.  The EDAM artifact, rather than that packaging failure, is
    # the setup success criterion.
    if len(edam_paths) != 1:
        if setup_timed_out:
            status, reason = "SETUP_TIMEOUT", "FuseSoC setup exceeded the configured timeout"
        else:
            status, reason = "SETUP_FAIL", "FuseSoC did not produce an EDAM file"
            setup_errors = [
                line.removeprefix("ERROR:").strip()
                for line in setup_log.read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()
                if line.startswith("ERROR:")
            ]
            if setup_errors:
                reason = setup_errors[-1]
        result.update(edam_produced=False, status=status, reason=reason,
                      diagnostics=[], cells=[])
        return result
    result["edam_produced"] = True

    edam_path = edam_paths[0]
    edam = yaml.safe_load(edam_path.read_text(encoding="utf-8"))
    sources, includes = edam_sources(edam, edam_path.parent)
    result["property_sites"] = scan_property_sites(sources, opentitan)
    setup = lowering_setup(cfg, edam, sources, includes)
    result.update(setup)
    filtered_dir = target_dir / "filtered"
    if filtered_dir.exists():
        shutil.rmtree(filtered_dir)
    target_sources = TargetSources(sources, filtered_dir)
    script_path = target_dir / "lower.ys"
    json_path = target_dir / "lower.json"
    lower_log = target_dir / "lower.log"
    # The diagnostics that dropped properties, on the runs before the last one
    dropping: list[dict[str, Any]] = []
    dropped_sites: list[dict[str, Any]] = []
    runs = 0
    for stale in target_dir.glob("lower.*.log"):
        stale.unlink()
    while True:
        runs += 1
        if runs > 1:
            # The log of each run that dropped properties stays
            lower_log.rename(target_dir / f"lower.{runs - 1}.log")
        json_path.unlink(missing_ok=True)
        write_yosys_script(script_path, setup, target_sources.current(),
                           target_sources.include_dirs(includes), json_path)
        yosys_rc, yosys_timed_out = run_process(
            ["yosys", "-m", str(plugin), "-l", str(lower_log), str(script_path)],
            target_dir,
            lowering_timeout,
            lower_log,
        )
        log_text = lower_log.read_text(encoding="utf-8", errors="replace")
        diagnostics = parse_diagnostics(
            log_text, target_dir, target_sources.originals(), opentitan
        )
        rejected: list[tuple[dict[str, Any], list[tuple[Path, Site]]]] = []
        blocking = []
        for diagnostic in diagnostics:
            sites = [
                (path, site) for path, site in target_sources.attribute(diagnostic)
                if path in target_sources.source_set
            ]
            if sites:
                # A diagnostic in a sequence or property declaration is on
                # every site using it
                diagnostic["properties"] = [
                    {"source": relative_source(path, opentitan), "name": site.name,
                     "kind": site.kind, "line": site.line}
                    for path, site in sites
                ]
            if rejects_property(diagnostic, bool(sites)):
                diagnostic["class"] = "unsupported_sva"
                if sites:
                    rejected.append((diagnostic, sites))
                    continue
            if diagnostic["severity"] != "warning" and diagnostic["class"] != "tool":
                blocking.append(diagnostic)
        if yosys_timed_out or (yosys_rc == 0 and json_path.exists()):
            break
        if blocking or not rejected:
            break
        new = target_sources.drop([site for _, sites in rejected for site in sites])
        if not new:
            break
        for path, site in new:
            dropped_sites.append(
                {
                    "source": relative_source(path, opentitan),
                    "line": site.line,
                    "name": site.name,
                    "kind": site.kind,
                    "run": runs,
                }
            )
        for diagnostic, _ in rejected:
            diagnostic["dropped"] = True
            dropping.append(diagnostic)

    originals = target_sources.originals()
    cells = read_cells(json_path, originals, opentitan) if json_path.exists() else []
    diagnostics = dropping + diagnostics
    for diagnostic in diagnostics:
        diagnostic.pop("path", None)
    if yosys_timed_out:
        status, reason = "LOWERING_TIMEOUT", "sv-elab exceeded the configured timeout"
    elif yosys_rc != 0 or not json_path.exists():
        status = "LOWERING_FAIL"
        # The first error no property was dropped for
        reason = blocking[0]["message"] if blocking else next(
            (d["message"] for d in diagnostics
             if d["severity"] != "warning" and not d.get("dropped")),
            "sv-elab failed before producing JSON",
        )
    elif dropped_sites:
        unsupported = sum(d["class"] == "unsupported_sva" for d in diagnostics)
        status, reason = "PARTIAL", f"{unsupported} unsupported SVA diagnostic(s)"
    else:
        status, reason = "PASS", "all elaborated properties lowered"
    result.update(
        yosys_returncode=yosys_rc,
        yosys_timeout=yosys_timed_out,
        yosys_log=str(lower_log.relative_to(build_root)),
        lowering_runs=runs,
        dropped_sites=dropped_sites,
        filtered_sources={
            relative_source(path, opentitan): str(copy.relative_to(build_root))
            for path, copy in target_sources.copies.items()
        },
        status=status,
        reason=reason,
        diagnostics=diagnostics,
        cells=cells,
    )
    return result


def scan_property_sites(sources: list[Path], opentitan: Path) -> list[dict[str, Any]]:
    """The inventory of assertion macro invocations and labeled concurrent
    assertions in the file list, counted as the census always has."""
    sites: list[dict[str, Any]] = []
    for source in sources:
        try:
            lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line_no, line in enumerate(lines, 1):
            match = PROPERTY_MACRO_RE.match(line) or PROPERTY_STMT_RE.match(line)
            if not match:
                continue
            sites.append(
                {
                    "source": relative_source(source, opentitan),
                    "line": line_no,
                    "name": match.group("name"),
                    "kind": match.group("kind").lower(),
                }
            )
    return sites


def tool_version(command: list[str]) -> str:
    try:
        output = subprocess.check_output(
            command, text=True, stderr=subprocess.STDOUT, timeout=15
        )
        return output.strip().splitlines()[0]
    except (OSError, subprocess.SubprocessError) as exc:
        return f"unavailable: {exc}"


def source_digest(sites: list[dict[str, Any]]) -> str:
    payload = json.dumps(
        sorted(
            (site["source"], site["line"], site["name"], site["kind"])
            for site in sites
        ),
        separators=(",", ":"),
    )
    return hashlib.sha256(payload.encode()).hexdigest()


def md(text: Any) -> str:
    return str(text).replace("|", "\\|")


def write_reports(
    output: Path,
    metadata: dict[str, Any],
    results: list[dict[str, Any]],
) -> None:
    output.mkdir(parents=True, exist_ok=True)
    complete = {"metadata": metadata, "targets": results}
    (output / "results.json").write_text(
        json.dumps(complete, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    with (output / "targets.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(
            [
                "id",
                "name",
                "variant",
                "core",
                "status",
                "reason",
                "property_sites",
                "check_cells",
                "assert_obligations",
                "assume_obligations",
                "cover_obligations",
                "live_obligations",
                "fair",
                "unsupported_sva",
                "other_diagnostics",
                "log",
            ]
        )
        for result in results:
            cell_counts = Counter(cell["type"] for cell in result.get("cells", []))
            flavor_counts = Counter(
                cell["flavor"] for cell in result.get("cells", [])
            )
            diagnostics = result.get("diagnostics", [])
            writer.writerow(
                [
                    result["id"],
                    result["name"],
                    result["variant"],
                    result["core"],
                    result["status"],
                    result["reason"],
                    len(result.get("property_sites", [])),
                    cell_counts["$check"],
                    flavor_counts["assert"],
                    flavor_counts["assume"],
                    flavor_counts["cover"],
                    flavor_counts["live"],
                    cell_counts["$fair"],
                    sum(d["class"] == "unsupported_sva" for d in diagnostics),
                    sum(d["class"] != "unsupported_sva" for d in diagnostics),
                    result.get("yosys_log", result.get("fusesoc_log", "")),
                ]
            )

    # A row for each property a diagnostic rejects
    unsupported_rows: list[dict[str, Any]] = []
    unsupported_count = 0
    for result in results:
        for diagnostic in result.get("diagnostics", []):
            if diagnostic["class"] != "unsupported_sva":
                continue
            unsupported_count += 1
            for prop in diagnostic.get("properties") or [{}]:
                unsupported_rows.append(
                    {
                        "target": result["id"],
                        "name": result["name"],
                        **diagnostic,
                        "property_name": prop.get("name", ""),
                        "property_kind": prop.get("kind", ""),
                        "property_line": prop.get("line", ""),
                        "property_source": prop.get("source", ""),
                    }
                )
    with (output / "unsupported.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        fields = [
            "target",
            "name",
            "file",
            "line",
            "column",
            "property_name",
            "property_kind",
            "property_line",
            "severity",
            "message",
            "property_source",
        ]
        writer = csv.DictWriter(
            handle,
            fieldnames=fields,
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(unsupported_rows)

    unique_sites: dict[tuple[Any, ...], dict[str, Any]] = {}
    for result in results:
        for site in result.get("property_sites", []):
            key = (site["source"], site["line"], site["name"], site["kind"])
            unique_sites[key] = site
    statuses = Counter(result["status"] for result in results)
    total_cells = Counter(
        cell["type"] for result in results for cell in result.get("cells", [])
    )
    total_flavors = Counter(
        cell["flavor"] for result in results for cell in result.get("cells", [])
    )
    unsupported_forms: dict[tuple[str, ...], set[str]] = {}
    for row in unsupported_rows:
        key = (
            str(row["property_source"] or row["file"]),
            str(row["property_line"] or row["line"]),
            str(row["property_name"] or "(unattributed)"),
            str(row["message"]),
        )
        unsupported_forms.setdefault(key, set()).add(row["target"])
    dropped = sum(len(result.get("dropped_sites", [])) for result in results)
    completed_targets = statuses["PASS"] + statuses["PARTIAL"]
    blocked_reasons = Counter(
        result["reason"]
        for result in results
        if result["status"] in ("LOWERING_FAIL", "SETUP_FAIL", "LOWERING_TIMEOUT", "SETUP_TIMEOUT")
    )
    resolved_cores = sum(bool(result.get("core_resolution")) for result in results)
    lines = [
        "# OpenTitan SVA compatibility census",
        "",
        f"Generated: `{metadata['generated_at']}`",
        "",
        f"OpenTitan revision: `{metadata['opentitan_revision']}`",
        "",
        f"sv-elab revision: `{metadata.get('sv_elab_revision', '')}`",
        "",
        "## Scope and interpretation",
        "",
        (
            f"All **{len(results)}** formal configurations in the pinned OpenTitan "
            "snapshot were attempted using their FuseSoC `formal` target. "
            "`FPV_ON` was enabled and bind modules were elaborated as extra tops."
        ),
        "",
        (
            "A PASS means every elaborated property lowered into Yosys formal cells. "
            "It does **not** mean the properties were mathematically proved. PARTIAL "
            "means lowering completed once the properties the frontend rejected had "
            "been dropped from a filtered copy of their source; each of them is "
            "listed below with the diagnostic that rejected it."
        ),
        "",
        (
            "Commercial-flow Tcl (`stopats`, `after_load`, task filtering) is recorded "
            "but not claimed to be reproduced by this frontend compatibility census. "
            "The SecCm primitive blackbox list is also recorded but deliberately not "
            "applied: this census checks source/SVA lowering, while those blackboxes "
            "are proof abstractions that would prevent open elaboration of the "
            "hierarchical countermeasure checks."
        ),
        "",
        "## Source filters",
        "",
        (
            "The census reads a copy of the OpenTitan tree with these rewrites, "
            "which keep what the sources do:"
        ),
        "",
        "| Source | Rewrite |",
        "| --- | --- |",
    ]
    for applied in metadata.get("source_filters", []):
        lines.append(f"| `{applied['path']}` | {md(applied['reason'])} |")
    lines.extend(["", "## Summary", "", "| Result | Targets |", "| --- | ---: |"])
    for status in ("PASS", "PARTIAL", "LOWERING_FAIL", "LOWERING_TIMEOUT", "SETUP_FAIL", "SETUP_TIMEOUT"):
        lines.append(f"| {status} | {statuses[status]} |")
    lines.extend(
        [
            "",
            (
                "De-duplicated source-level property candidates in attempted EDAM "
                f"file lists: **{len(unique_sites)}**."
            ),
            "",
            (
                "This source count includes transitive file-list candidates and is "
                "not presented as an elaborated-instance denominator. Emitted cells "
                "and diagnostics below are elaboration results."
            ),
            "",
            (
                f"Emitted formal cells: **{total_cells['$check']}** `$check`, "
                f"**{total_cells['$live']}** legacy `$live`, "
                f"**{total_cells['$fair']}** `$fair`."
            ),
            "",
            (
                "Obligations by `$check` flavor: "
                f"**{total_flavors['assert']}** assert, "
                f"**{total_flavors['assume']}** assume, "
                f"**{total_flavors['cover']}** cover, "
                f"**{total_flavors['live']}** live, "
                f"**{total_flavors['fair']}** fair."
            ),
            "",
            f"Unsupported-SVA diagnostics: **{unsupported_count}**.",
            "",
            f"Unique unsupported source forms: **{len(unsupported_forms)}**.",
            "",
            f"Property sites dropped, over all configurations: **{dropped}**.",
            "",
            "## Conclusions",
            "",
            (
                f"- **{completed_targets}/{len(results)}** configurations "
                f"({100.0 * completed_targets / len(results):.1f}%) completed "
                f"elaboration and lowering; {statuses['PASS']} were clean and "
                f"{statuses['PARTIAL']} were partial."
            ),
            (
                f"- The completed configurations emitted **{total_flavors['live']}** "
                "liveness obligations. This is direct evidence that OpenTitan "
                "liveness properties now reach Yosys, but it is not a proof result."
                if total_flavors["live"]
                else "- The completed configurations emitted no liveness obligations."
            ),
            (
                f"- **{len(unsupported_forms)}** unique unsupported source forms "
                f"produced **{unsupported_count}** configuration/instance "
                "diagnostics. Repetition is expected because common TL-UL, Ibex, "
                "and OTBN sources elaborate in many targets and generated instances."
            ),
            (
                f"- **{len(results) - completed_targets}** configurations remain "
                "blocked before a complete property inventory. Their exact root "
                "diagnostics are listed below; they must not be counted as property "
                "passes or failures."
            ),
            (
                f"- **{resolved_cores}** stale config core identifiers were mapped "
                "to an unambiguous same-component core in the pinned source tree. "
                "Every such resolution is retained in `results.json`."
            ),
            "",
            "## Blocked configuration roots",
            "",
            "| First blocking diagnostic | Configurations |",
            "| --- | ---: |",
        ]
    )
    for reason, count in blocked_reasons.most_common():
        lines.append(f"| {md(reason)} | {count} |")
    lines.extend(
        [
            "",
            "## Unsupported SVA source forms",
            "",
            "| Source | Property | Diagnostic | Affected configurations |",
            "| --- | --- | --- | ---: |",
        ]
    )
    for (source, line, name, message), targets in sorted(unsupported_forms.items()):
        lines.append(f"| `{source}:{line}` | `{name}` | {md(message)} | {len(targets)} |")
    lines.extend(
        [
            "",
            "## Target results",
            "",
            "| Configuration | Variant | Result | Checks | Assert | Assume | Cover | Live | Fair | Unsupported | Reason |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        ]
    )
    for result in results:
        cells = Counter(cell["type"] for cell in result.get("cells", []))
        flavors = Counter(cell["flavor"] for cell in result.get("cells", []))
        unsupported = sum(
            diag["class"] == "unsupported_sva"
            for diag in result.get("diagnostics", [])
        )
        lines.append(
            f"| `{result['name']}` | {result['variant']} | {result['status']} | "
            f"{cells['$check']} | {flavors['assert']} | {flavors['assume']} | "
            f"{flavors['cover']} | {flavors['live']} | {cells['$fair']} | "
            f"{unsupported} | {md(result['reason'])} |"
        )
    lines.extend(
        [
            "",
            "Machine-readable detail is in `results.json`, `targets.csv`, and "
            "`unsupported.csv`. Per-target scripts, filtered sources and complete "
            "logs are retained under the campaign build directory recorded in "
            "`results.json`.",
            "",
        ]
    )
    (output / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="sv-elab checkout",
    )
    parser.add_argument(
        "--opentitan",
        type=Path,
        help="OpenTitan checkout (default: tests/third_party/opentitan, checked "
        "out at the pinned revision on first use)",
    )
    parser.add_argument("--plugin", type=Path, help="built slang.so")
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--report-from",
        type=Path,
        help="regenerate report files from an existing results.json without rerunning targets",
    )
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    parser.add_argument("--setup-timeout", type=int, default=180)
    parser.add_argument("--lowering-timeout", type=int, default=300)
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()

    repo = args.repo.resolve()
    build_root = (
        args.build_root.resolve()
        if args.build_root
        else (repo / "build/opentitan_sva_census").resolve()
    )
    output = (
        args.output.resolve()
        if args.output
        else (build_root / "report").resolve()
    )
    if args.report_from:
        saved = json.loads(args.report_from.read_text(encoding="utf-8"))
        write_reports(output, saved["metadata"], saved["targets"])
        print(f"Report: {output / 'REPORT.md'}", flush=True)
        return 0
    plugin = (
        args.plugin.resolve() if args.plugin else (repo / "build/slang.so").resolve()
    )
    if not plugin.is_file():
        parser.error(f"plugin not found: {plugin}")
    if args.opentitan:
        checkout = args.opentitan.resolve()
    else:
        checkout = (repo / "tests/third_party/opentitan").resolve()
        subprocess.run(
            [str(repo / "tests/opentitan_sva_stress/common/fetch_opentitan.sh"), str(checkout)],
            check=True,
        )

    build_root.mkdir(parents=True, exist_ok=True)
    opentitan = build_root / "opentitan"
    source_filters = prepare_opentitan(checkout, opentitan)
    all_configs = discover_configs(opentitan)
    registry = core_registry(opentitan)
    for cfg in all_configs:
        resolved, reason = resolve_core_name(
            str(cfg["fusesoc_core"]), str(cfg["top_variant"]), registry
        )
        cfg["resolved_core"] = resolved
        cfg["core_resolution"] = reason
    selected = [
        (index, cfg)
        for index, cfg in enumerate(all_configs, 1)
        if not args.target
        or cfg["name"] in args.target
        or target_id(cfg, index) in args.target
    ]
    if args.limit is not None:
        selected = selected[: args.limit]
    if not selected:
        parser.error("no configurations selected")

    print(
        f"OpenTitan SVA census: {len(selected)} configurations, {args.jobs} jobs",
        flush=True,
    )
    results: list[dict[str, Any]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_target,
                repo,
                opentitan,
                plugin,
                build_root,
                cfg,
                index,
                args.setup_timeout,
                args.lowering_timeout,
            ): (index, cfg)
            for index, cfg in selected
        }
        for future in concurrent.futures.as_completed(futures):
            index, cfg = futures[future]
            try:
                result = future.result()
            except Exception as exc:  # Preserve a campaign result for harness bugs.
                result = {
                    "id": target_id(cfg, index),
                    "index": index,
                    "name": cfg["name"],
                    "variant": cfg["top_variant"],
                    "core": cfg["fusesoc_core"],
                    "config_source": cfg["config_source"],
                    "status": "HARNESS_FAIL",
                    "reason": f"{type(exc).__name__}: {exc}",
                    "diagnostics": [],
                    "cells": [],
                }
            results.append(result)
            print(
                f"[{len(results):3d}/{len(selected)}] {result['id']}: "
                f"{result['status']} ({result['reason']})",
                flush=True,
            )
    results.sort(key=lambda result: result["index"])

    metadata = {
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "campaign_build_root": str(build_root),
        "opentitan_revision": git_output(checkout, "rev-parse", "HEAD"),
        "opentitan_describe": git_output(checkout, "describe", "--always", "--dirty"),
        "sv_elab_revision": git_output(repo, "rev-parse", "HEAD"),
        "sv_elab_describe": git_output(repo, "describe", "--always", "--dirty"),
        "plugin": str(plugin),
        "yosys_version": tool_version(["yosys", "-V"]),
        "fusesoc_version": tool_version(["fusesoc", "--version"]),
        "python": sys.version.split()[0],
        "requested_configurations": len(selected),
        "official_configuration_count": len(all_configs),
        "source_filters": source_filters,
    }
    all_sites = [
        site for result in results for site in result.get("property_sites", [])
    ]
    metadata["property_site_digest"] = source_digest(all_sites)
    write_reports(output, metadata, results)
    print(f"Report: {output / 'REPORT.md'}", flush=True)

    # A completed census is useful even when compatibility failures are found.
    # Exit nonzero only for harness/setup failures that invalidate the inventory.
    invalid = {
        "HARNESS_FAIL",
        "SETUP_FAIL",
        "SETUP_TIMEOUT",
    }
    return 1 if any(result["status"] in invalid for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
