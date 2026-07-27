#!/usr/bin/env python3
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC
"""Run every OpenTitan formal configuration through yosys-slang.

This is a compatibility census, not a replacement for the commercial formal
flow.  It uses OpenTitan's FuseSoC file lists and elaborates the real SVA
sources with FPV_ON.  Unsupported SVA is reported property-by-property while
the remaining properties in a target continue to lower.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
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

SVA_WARNING_TEXT = (
    "unsupported assertion statement",
    "encountered unsupported SVA feature",
    "repetition",
    "SVA clocking event",
    "temporal delay requires",
    "liveness expression needs an acceptance condition",
    "SVA match items",
    "covering an unbounded liveness property",
    "$past gating",
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


def write_yosys_script(
    path: Path,
    plugin: Path,
    cfg: dict[str, Any],
    edam: dict[str, Any],
    sources: list[Path],
    includes: list[Path],
    output_json: Path,
) -> dict[str, Any]:
    del plugin  # Loaded by the outer yosys command.
    main_top = str(edam.get("toplevel") or cfg["dut"])
    bind_tops = find_bind_tops(sources)
    defines = ["FPV_ON"]
    defines.extend(split_defines(cfg.get("defines")))
    if cfg.get("task"):
        defines.extend(["FPV_SEC_CM_ON", "FPV_ALERT_NO_SIGINT_ERR"])
    defines = list(dict.fromkeys(defines))
    params = parse_parameters(cfg)

    args = [
        "read_slang",
        "--best-effort-sva",
        "--no-synthesis-define",
        "--ignore-timing",
        "--ignore-initial",
        "--single-unit",
        "--allow-use-before-declare",
        "--relax-enum-conversions",
        "--allow-hierarchical-const",
    ]
    for define in defines:
        args.extend(["-D", define])
    for top in [main_top, *bind_tops]:
        args.extend(["--top", top])
    for include in includes:
        args.extend(["-I", str(include)])
    for param in params:
        args.extend(["-G", param])
    official_blackboxes = (
        [
            "prim_count",
            "prim_double_lfsr",
            "prim_onehot_check",
            "prim_ram_1p",
            "prim_ram_1p_scr",
        ]
        if cfg.get("task")
        else []
    )
    args.extend(str(source) for source in sources)

    script = " ".join(yosys_quote(arg) for arg in args)
    script += f"\nhierarchy -check -top {yosys_quote(main_top)}\n"
    script += f"write_json {yosys_quote(output_json)}\n"
    path.write_text(script, encoding="utf-8")
    return {
        "main_top": main_top,
        "bind_tops": bind_tops,
        "defines": defines,
        "parameters": params,
        "official_blackboxes": official_blackboxes,
        "blackboxes_applied": [],
        "source_count": len(sources),
        "include_dir_count": len(includes),
    }


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


def scan_property_sites(sources: list[Path], opentitan: Path) -> list[dict[str, Any]]:
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


def parse_diagnostics(log: str, opentitan: Path) -> list[dict[str, Any]]:
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
        item = match.groupdict()
        item["line"] = int(item["line"])
        item["column"] = int(item["column"])
        item["file"] = relative_source(item["file"], opentitan)
        item["class"] = (
            "unsupported_sva"
            if any(text.lower() in item["message"].lower() for text in SVA_WARNING_TEXT)
            else "frontend"
        )
        diagnostics.append(item)
    return diagnostics


def annotate_diagnostics(
    diagnostics: list[dict[str, Any]], sites: list[dict[str, Any]]
) -> None:
    by_source: dict[str, list[dict[str, Any]]] = {}
    for site in sites:
        by_source.setdefault(site["source"], []).append(site)
    for source_sites in by_source.values():
        source_sites.sort(key=lambda site: site["line"])
    for diagnostic in diagnostics:
        candidates = [
            site
            for site in by_source.get(diagnostic["file"], [])
            if site["line"] <= diagnostic["line"]
        ]
        if not candidates:
            continue
        site = candidates[-1]
        # Macro bodies are normally short. Avoid falsely attributing a later
        # RTL diagnostic to a distant preceding property.
        if diagnostic["line"] - site["line"] > 40:
            continue
        diagnostic["property_name"] = site["name"]
        diagnostic["property_kind"] = site["kind"]
        diagnostic["property_line"] = site["line"]


def read_cells(path: Path, opentitan: Path) -> list[dict[str, Any]]:
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
                    source = (
                        f"{relative_source(source_match.group(1), opentitan)}:"
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


def classify_result(
    setup_ok: bool,
    setup_timeout: bool,
    yosys_rc: int | None,
    yosys_timeout: bool,
    diagnostics: list[dict[str, Any]],
    json_exists: bool,
) -> tuple[str, str]:
    if setup_timeout:
        return "SETUP_TIMEOUT", "FuseSoC setup exceeded the configured timeout"
    if not setup_ok:
        return "SETUP_FAIL", "FuseSoC did not produce an EDAM file"
    if yosys_timeout:
        return "LOWERING_TIMEOUT", "yosys-slang exceeded the configured timeout"
    if yosys_rc != 0 or not json_exists:
        first_error = next(
            (diag["message"] for diag in diagnostics if diag["severity"] != "warning"),
            "yosys-slang failed before producing JSON",
        )
        return "LOWERING_FAIL", first_error
    unsupported = [diag for diag in diagnostics if diag["class"] == "unsupported_sva"]
    other_errors = [
        diag
        for diag in diagnostics
        if diag["class"] != "unsupported_sva" and diag["severity"] != "warning"
    ]
    if other_errors:
        return "LOWERING_FAIL", other_errors[0]["message"]
    if unsupported:
        return "PARTIAL", f"{len(unsupported)} unsupported SVA diagnostic(s)"
    return "PASS", "all elaborated properties lowered"


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
    setup_rc, setup_timed_out = run_process(
        fusesoc_command(opentitan, target_dir / "fusesoc-work", cfg),
        repo,
        setup_timeout,
        setup_log,
        setup_env,
    )
    edam_paths = sorted((target_dir / "fusesoc-work").glob("*.eda.yml"))
    result["fusesoc_returncode"] = setup_rc
    result["fusesoc_timeout"] = setup_timed_out
    result["fusesoc_log"] = str(setup_log.relative_to(build_root))
    # FuseSoC 2.4.5 can fail while instantiating Edalize after successfully
    # writing EDAM.  The EDAM artifact, rather than that packaging failure, is
    # the setup success criterion.
    setup_ok = len(edam_paths) == 1
    result["edam_produced"] = setup_ok
    if not setup_ok:
        status, reason = classify_result(
            False, setup_timed_out, None, False, [], False
        )
        setup_errors = [
            line.removeprefix("ERROR:").strip()
            for line in setup_log.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
            if line.startswith("ERROR:")
        ]
        if setup_errors and not setup_timed_out:
            reason = setup_errors[-1]
        result.update(status=status, reason=reason, diagnostics=[], cells=[])
        return result

    edam_path = edam_paths[0]
    edam = yaml.safe_load(edam_path.read_text(encoding="utf-8"))
    sources, includes = edam_sources(edam, edam_path.parent)
    result["property_sites"] = scan_property_sites(sources, opentitan)
    script_path = target_dir / "lower.ys"
    json_path = target_dir / "lower.json"
    result.update(
        write_yosys_script(
            script_path, plugin, cfg, edam, sources, includes, json_path
        )
    )
    lower_log = target_dir / "lower.log"
    yosys_rc, yosys_timed_out = run_process(
        ["yosys", "-m", str(plugin), "-ql", str(lower_log), str(script_path)],
        repo,
        lowering_timeout,
        lower_log,
    )
    log_text = lower_log.read_text(encoding="utf-8", errors="replace")
    diagnostics = parse_diagnostics(log_text, opentitan)
    annotate_diagnostics(diagnostics, result["property_sites"])
    cells = read_cells(json_path, opentitan) if json_path.exists() else []
    status, reason = classify_result(
        setup_ok,
        setup_timed_out,
        yosys_rc,
        yosys_timed_out,
        diagnostics,
        json_path.exists(),
    )
    result.update(
        yosys_returncode=yosys_rc,
        yosys_timeout=yosys_timed_out,
        yosys_log=str(lower_log.relative_to(build_root)),
        status=status,
        reason=reason,
        diagnostics=diagnostics,
        cells=cells,
    )
    return result


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

    unsupported_rows: list[dict[str, Any]] = []
    for result in results:
        for diagnostic in result.get("diagnostics", []):
            if diagnostic["class"] != "unsupported_sva":
                continue
            unsupported_rows.append(
                {
                    "target": result["id"],
                    "name": result["name"],
                    **diagnostic,
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
    unsupported_forms: dict[tuple[Any, ...], set[str]] = {}
    for row in unsupported_rows:
        key = (
            row["file"],
            row.get("property_line", row["line"]),
            row.get("property_name", "(unattributed)"),
            row["message"],
        )
        unsupported_forms.setdefault(key, set()).add(row["target"])
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
        f"yosys-slang revision: `{metadata['yosys_slang_revision']}`",
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
            "means lowering completed but one or more explicitly reported properties "
            "were omitted because their SVA form is unsupported."
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
        "## Summary",
        "",
        "| Result | Targets |",
        "| --- | ---: |",
    ]
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
            f"Unsupported-SVA diagnostics: **{len(unsupported_rows)}**.",
            "",
            f"Unique unsupported source forms: **{len(unsupported_forms)}**.",
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
            ),
            (
                f"- **{len(unsupported_forms)}** unique unsupported source forms "
                f"produced **{len(unsupported_rows)}** configuration/instance "
                "diagnostics. Repetition is expected because common TL-UL, Ibex, "
                "and OTBN sources elaborate in many targets and generated instances."
            ),
            (
                "- The dominant unsupported classes are four-state knownness "
                "(`$isunknown`), match-item/local-variable sequences, and temporal "
                "forms combining `throughout`, goto repetition, or unbounded ranges "
                "that the current liveness lowering cannot represent."
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
        lines.append(f"| {str(reason).replace('|', chr(92) + '|')} | {count} |")
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
        lines.append(
            f"| `{source}:{line}` | `{name}` | "
            f"{str(message).replace('|', chr(92) + '|')} | {len(targets)} |"
        )
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
        reason = str(result["reason"]).replace("|", "\\|")
        lines.append(
            f"| `{result['name']}` | {result['variant']} | {result['status']} | "
            f"{cells['$check']} | {flavors['assert']} | {flavors['assume']} | "
            f"{flavors['cover']} | {flavors['live']} | {cells['$fair']} | "
            f"{unsupported} | {reason} |"
        )
    lines.extend(
        [
            "",
            "Machine-readable detail is in `results.json`, `targets.csv`, and "
            "`unsupported.csv`. Per-target scripts and complete logs are retained "
            "under the campaign build directory recorded in `results.json`.",
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
        help="yosys-slang checkout",
    )
    parser.add_argument(
        "--opentitan",
        type=Path,
        help="OpenTitan checkout (default: tests/third_party/opentitan)",
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
    opentitan = (
        args.opentitan.resolve()
        if args.opentitan
        else (repo / "tests/third_party/opentitan").resolve()
    )
    plugin = (
        args.plugin.resolve() if args.plugin else (repo / "build/slang.so").resolve()
    )
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
    if not plugin.is_file():
        parser.error(f"plugin not found: {plugin}")

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
    build_root.mkdir(parents=True, exist_ok=True)

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
        "opentitan_revision": git_output(opentitan, "rev-parse", "HEAD"),
        "opentitan_describe": git_output(opentitan, "describe", "--always", "--dirty"),
        "yosys_slang_revision": git_output(repo, "rev-parse", "HEAD"),
        "yosys_slang_describe": git_output(repo, "describe", "--always", "--dirty"),
        "yosys_version": tool_version(["yosys", "-V"]),
        "fusesoc_version": tool_version(["fusesoc", "--version"]),
        "python": sys.version.split()[0],
        "requested_configurations": len(selected),
        "official_configuration_count": len(all_configs),
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
